"""Scaffold a new DLAA game repo from template/ (step 0 of the dlaa-injection workflow).

    python tools/new_game.py --exe "G:\\games\\Foo\\Foo.exe" --name foo --prefix FOO_DLAA --title "Foo: The Game"

1. Inspects the exe: x86/x64, version, how d3d11/dxgi are imported (static, delay-load,
   or not at all), section entropy (packed/protected?), and other proxy dlls / mods
   already next to it.
2. Copies template/ to <root>\\<name>, renames EXAMPLE_DLAA / example_dlaa / example::,
   writes CMakeLists.txt (x64, or x86 + x64 NGX helper), tools\\build.ps1, .gitignore,
   a facts-only CLAUDE.md, .claude\\deploy.json, and git init (no commit).
3. Builds and runs the smoke test (skip with --no-build).
4. Creates the per-game notes stub in the dlaa-injection skill if missing.

Never touches the game folder. Refuses if <root>\\<name> exists.
"""
import argparse
import datetime
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

import pefile

CORE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKILL_GAMES = os.path.join(os.path.expanduser('~'), '.claude', 'skills', 'dlaa-injection', 'references', 'games')
PROXY_NAMES = {'d3d11.dll', 'dxgi.dll', 'd3d9.dll', 'dinput8.dll', 'version.dll', 'winmm.dll', 'xinput1_3.dll',
               'xinput1_4.dll', 'dsound.dll', 'nvngx_dlss.dll', 'reshade.ini', 'dxvk.conf'}


def entropy(data):
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def inspect(exe):
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT'],
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    arch = 'x64' if pe.FILE_HEADER.Machine == 0x8664 else 'x86'

    def imports(entries):
        out = {}
        for e in entries or []:
            dll = e.dll.decode(errors='replace').lower()
            if dll in ('d3d11.dll', 'dxgi.dll', 'd3d12.dll', 'd3d9.dll'):
                out[dll] = [i.name.decode(errors='replace') if i.name else f'#{i.ordinal}' for i in e.imports]
        return out

    static = imports(getattr(pe, 'DIRECTORY_ENTRY_IMPORT', None))
    delay = imports(getattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT', None))

    version = ''
    for fi in getattr(pe, 'FileInfo', None) or []:
        for entry in fi:
            for st in getattr(entry, 'StringTable', []) or []:
                for k in (b'ProductVersion', b'FileVersion'):
                    if k in st.entries and not version:
                        version = st.entries[k].decode(errors='replace').strip()

    sections = []
    for s in pe.sections:
        name = s.Name.rstrip(b'\0').decode(errors='replace')
        sections.append((name, s.SizeOfRawData, s.Misc_VirtualSize, entropy(s.get_data()[:4 << 20]),
                         bool(s.Characteristics & 0x20000000)))
    # Packed/protected: a code section that is high-entropy or has no raw data, or any
    # non-resource section that looks encrypted (MGSV's .impdata is 7.85).
    code = [s for s in sections if s[4]]
    packed = (any(e > 7.2 for _, _, _, e, _ in code) or any(raw == 0 and virt > 0 for _, raw, virt, _, x in code)
              or any(e > 7.5 and n != '.rsrc' for n, _, _, e, _ in sections))
    odd = [n for n, *_ in sections if n not in ('.text', '.rdata', '.data', '.pdata', '.rsrc', '.reloc', '.tls',
                                                 '_RDATA', '.didat', '.gfids', '.00cfg', '.idata', '.CRT', '.bss')]

    folder = os.path.dirname(exe)
    neighbours = sorted(f for f in os.listdir(folder)
                        if f.lower() in PROXY_NAMES or f.lower().endswith('.asi'))

    # No d3d import in the exe: find the game dll that has one (TQ: Direct3D11.dll).
    importers = []
    if not static and not delay:
        for f in sorted(os.listdir(folder)):
            if not f.lower().endswith('.dll') or f.lower() in PROXY_NAMES:
                continue
            try:
                dpe = pefile.PE(os.path.join(folder, f), fast_load=True)
                dpe.parse_data_directories(directories=[
                    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
                    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT']])
                dlls = {e.dll.decode(errors='replace').lower() for e in
                        (getattr(dpe, 'DIRECTORY_ENTRY_IMPORT', None) or []) + (getattr(dpe, 'DIRECTORY_ENTRY_DELAY_IMPORT', None) or [])}
                hit = sorted(dlls & {'d3d11.dll', 'dxgi.dll', 'd3d12.dll', 'd3d9.dll'})
                if hit:
                    importers.append(f"{f} ({', '.join(hit)})")
            except Exception:
                continue
    return dict(arch=arch, static=static, delay=delay, version=version, sections=sections, importers=importers,
                packed=packed, odd=odd, neighbours=neighbours, size_mb=os.path.getsize(exe) / 1e6)


def loading_line(info):
    parts = []
    for kind, imp in (('static', info['static']), ('delay-load', info['delay'])):
        for dll, names in imp.items():
            parts.append(f"{kind} `{dll}`: {', '.join(names[:6])}")
    if not parts:
        if info['importers']:
            return f"not imported by the exe; imported by {', '.join(info['importers'])} (a d3d11.dll proxy next to the exe still loads first)"
        return 'no d3d11/dxgi import in the exe or its folder dlls: loaded at run time (LoadLibrary); check before relying on a d3d11.dll proxy'
    if 'd3d12.dll' in info['static'] or 'd3d12.dll' in info['delay']:
        parts.append('**imports d3d12: dlaa-core is D3D11 only**')
    return '; '.join(parts)


CMAKE_X64 = """cmake_minimum_required(VERSION 3.21)
# {prefix}: DLAA for {title} (x64, D3D11), a d3d11.dll proxy built on
# ..\\dlaa-core. Build with tools\\build.ps1.
project({prefix} LANGUAGES C CXX ASM_MASM)

# Shared proxy / hooks / NGX (C:\\_APPS\\CLAUDE\\dlaa-core).
set(DLAA_CORE_DIR ${{CMAKE_SOURCE_DIR}}/../dlaa-core CACHE PATH "dlaa-core checkout")
add_subdirectory(${{DLAA_CORE_DIR}} dlaa_core)

# The game's own shaders: dlaa_core_compile_shader(src/mv.hlsl cs_5_0 kMvCs mv.h)
# then SHADERS mv below.

add_library({lower} SHARED src/game.cpp)
dlaa_core_add_mod({lower})

# tests/smoke_test.cpp from dlaa-core: drives the built proxy.
add_executable(smoke_test ${{DLAA_CORE_DIR}}/tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE dxgi d3dcompiler)
"""

CMAKE_X86 = """cmake_minimum_required(VERSION 3.21)
# {prefix}: DLAA for {title} (32-bit, D3D11). The d3d11.dll proxy is built
# with -A Win32 (build\\), and dlaa-core's 64-bit NGX helper dlaa_ngx_host.exe
# with -A x64 (build64\\). tools\\build.ps1 does both and puts the helper next
# to the proxy.
project({prefix} LANGUAGES C CXX ASM_MASM)

# Shared proxy / hooks / NGX (C:\\_APPS\\CLAUDE\\dlaa-core).
set(DLAA_CORE_DIR ${{CMAKE_SOURCE_DIR}}/../dlaa-core CACHE PATH "dlaa-core checkout")
add_subdirectory(${{DLAA_CORE_DIR}} dlaa_core)

if(NOT DLAA_CORE_X86)
  # x64 configure: only the helper (target dlaa_ngx_host, defined by the core).
  return()
endif()

# The game's own shaders: dlaa_core_compile_shader(src/mv.hlsl cs_5_0 kMvCs mv.h)
# then SHADERS mv below.

add_library({lower} SHARED src/game.cpp)
dlaa_core_add_mod({lower})

# tests/smoke_test.cpp from dlaa-core: drives the built proxy.
add_executable(smoke_test ${{DLAA_CORE_DIR}}/tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE dxgi d3dcompiler)
"""

BUILD_X64 = """# Quiet build: errors/warnings only, then one OK line. Forwards to
# ..\\dlaa-core\\tools\\build.ps1 (options: -Config, -Target a,b, -Clangd, -Reconfigure).
& (Join-Path $PSScriptRoot "..\\..\\dlaa-core\\tools\\build.ps1") -Root (Split-Path -Parent $PSScriptRoot) @args
exit $LASTEXITCODE
"""

# Same as Titanquest dlaa\\tools\\build.ps1.
BUILD_X86 = """# Quiet build of both halves: the 32-bit d3d11.dll proxy (build\\) and the
# 64-bit NGX helper dlaa_ngx_host.exe (build64\\), copied next to the proxy.
# Forwards to ..\\dlaa-core\\tools\\build.ps1 (options: -Config, -Target a,b,
# -Clangd, -Reconfigure apply to the proxy build).
param([string]$Config = 'Release')
$core = Join-Path $PSScriptRoot "..\\..\\dlaa-core\\tools\\build.ps1"
$root = Split-Path -Parent $PSScriptRoot

& $core -Root $root -Arch Win32 -BuildDir build -Config $Config @args
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$out = & $core -Root $root -Arch x64 -BuildDir build64 -Config $Config -Target dlaa_ngx_host
if ($LASTEXITCODE -ne 0) { $out; exit $LASTEXITCODE }
$host_exe = Get-ChildItem (Join-Path $root build64) -Recurse -Filter dlaa_ngx_host.exe |
  Where-Object { $_.DirectoryName -like "*\\$Config" } | Select-Object -First 1
$dest = Join-Path $root "build\\$Config"
Copy-Item $host_exe.FullName $dest -Force
Write-Output ("OK  build\\{0}\\dlaa_ngx_host.exe  {1} bytes" -f $Config, $host_exe.Length)
exit 0
"""

GITIGNORE = """build/
build64/
out/
.vs/
.cache/
*.log
__pycache__/
{prefix}_frame_*
{prefix}_shaders/
{prefix}_capture_*/
dumps/
releases/
ida/
.claude/settings.local.json

# Screenshots
*.png
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True, help='full path of the game exe')
    ap.add_argument('--name', required=True, help='repo folder name under --root')
    ap.add_argument('--prefix', required=True, help='e.g. FOO_DLAA (ini, log and dump names)')
    ap.add_argument('--title', required=True, help='game title for docs')
    ap.add_argument('--root', default=os.path.dirname(CORE))
    ap.add_argument('--no-build', action='store_true')
    a = ap.parse_args()

    if not re.fullmatch(r'[A-Z][A-Z0-9_]*', a.prefix):
        sys.exit('prefix must be UPPER_SNAKE, e.g. FOO_DLAA')
    exe = os.path.abspath(a.exe)
    if not os.path.isfile(exe):
        sys.exit(f'no exe at {exe}')
    dest = os.path.join(a.root, a.name)
    if os.path.exists(dest):
        sys.exit(f'{dest} already exists')
    lower = a.prefix.lower()
    ns = re.sub(r'_dlaa$', '', lower) or lower
    slug = re.sub(r'[^a-z0-9]+', '-', a.name.lower()).strip('-')

    info = inspect(exe)
    x86 = info['arch'] == 'x86'

    # Template copy + renames.
    shutil.copytree(os.path.join(CORE, 'template'), dest)
    os.rename(os.path.join(dest, 'EXAMPLE_DLAA.ini'), os.path.join(dest, f'{a.prefix}.ini'))
    for dirpath, _, files in os.walk(dest):
        for f in files:
            p = os.path.join(dirpath, f)
            with open(p, encoding='utf-8') as fh:
                t = fh.read()
            t = (t.replace('EXAMPLE_DLAA', a.prefix).replace('example_dlaa', lower)
                  .replace('namespace example', f'namespace {ns}').replace('example::', f'{ns}::'))
            with open(p, 'w', encoding='utf-8', newline='') as fh:
                fh.write(t)
    with open(os.path.join(dest, 'CMakeLists.txt'), 'w', newline='\n') as fh:
        fh.write((CMAKE_X86 if x86 else CMAKE_X64).format(prefix=a.prefix, title=a.title, lower=lower))
    os.makedirs(os.path.join(dest, 'tools'))
    with open(os.path.join(dest, 'tools', 'build.ps1'), 'w', newline='\r\n') as fh:
        fh.write(BUILD_X86 if x86 else BUILD_X64)
    with open(os.path.join(dest, '.gitignore'), 'w', newline='\n') as fh:
        fh.write(GITIGNORE.format(prefix=a.prefix))

    # Build + smoke test.
    built, smoke = [], 'skipped (--no-build)'
    if not a.no_build:
        r = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                            os.path.join(dest, 'tools', 'build.ps1')], capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode != 0:
            print(r.stderr.strip())
            smoke = f'not run: build failed (exit {r.returncode})'
        else:
            rel = os.path.join(dest, 'build', 'Release')
            built = [f for f in ('d3d11.dll', 'nvngx_dlss.dll', 'dlaa_ngx_host.exe') if os.path.exists(os.path.join(rel, f))]
            tmp = tempfile.mkdtemp(prefix=f'{lower}_smoke_')
            shutil.copy(os.path.join(rel, 'd3d11.dll'), tmp)
            shutil.copy(os.path.join(dest, f'{a.prefix}.ini'), tmp)
            s = subprocess.run([os.path.join(rel, 'smoke_test.exe'), os.path.join(tmp, 'd3d11.dll')],
                               capture_output=True, text=True, timeout=120)
            last = (s.stdout.strip().splitlines() or ['(no output)'])[-1]
            smoke = f'{"OK" if s.returncode == 0 and last.endswith("OK") else "FAILED"}: {last} (log in {tmp})'
    files = [f'build\\Release\\{f}' for f in (built or (['d3d11.dll', 'dlaa_ngx_host.exe', 'nvngx_dlss.dll'] if x86
                                                       else ['d3d11.dll', 'nvngx_dlss.dll']))]

    os.makedirs(os.path.join(dest, '.claude'))
    with open(os.path.join(dest, '.claude', 'deploy.json'), 'w') as fh:
        json.dump({'build': 'tools\\build.ps1', 'target': os.path.dirname(exe),
                   'process': os.path.splitext(os.path.basename(exe))[0],
                   'files': files, 'ini': [f'{a.prefix}.ini']}, fh, indent=2)
        fh.write('\n')

    secs = ', '.join(f'`{n}` {raw / 1e6:.1f} MB e={e:.2f}' for n, raw, _, e, _ in info['sections'])
    layout = ('**likely packed/protected** (high-entropy or empty code section): plan an in-process dump (`DumpExeAt`) before IDA. '
              if info['packed'] else '') + secs
    today = datetime.date.today().isoformat()
    game_lines = [
        f"- `{exe}`, {info['size_mb']:.0f} MB, {info['arch']}" + (f", version {info['version']}" if info['version'] else ''),
        f"- Loading: {loading_line(info)}",
        f"- Exe layout: {layout}",
    ]
    if info['neighbours']:
        game_lines.append(f"- Already next to the exe: {', '.join(info['neighbours'])}")
    claude_md = f"""# {a.prefix}: DLAA for {a.title} ({'32-bit' if x86 else 'x64'}, DX11)

A `d3d11.dll` proxy built on `..\\dlaa-core`, which holds the hooks, NGX, dumps and capture.{' The game is 32-bit and NGX is x64 only, so DLSS runs in the 64-bit helper `dlaa_ngx_host.exe`.' if x86 else ''} Cross-game notes: the `dlaa-injection` skill (`references/games/{slug}.md`). Scaffolded {today} by `dlaa-core\\tools\\new_game.py`.

## Build / test
- `tools\\build.ps1` does a quiet Release build.
- Smoke test: copy `build\\Release\\d3d11.dll` + `{a.prefix}.ini` to a temp folder, then `build\\Release\\smoke_test.exe <full path>\\d3d11.dll` must end with "OK"; read `{a.prefix}.log` there.
- Deploy: `/deploy` (`.claude\\deploy.json`).

## Game
{chr(10).join(game_lines)}

## Layout
- `src/game.cpp`: config, core hooks and per-frame stats, from dlaa-core's template. The DLAA pass is still to be written.

## Rules
"""
    with open(os.path.join(dest, 'CLAUDE.md'), 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(claude_md)

    notes = os.path.join(SKILL_GAMES, f'{slug}.md')
    if os.path.isdir(SKILL_GAMES) and not os.path.exists(notes):
        with open(notes, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(f"# {a.title} ({info['arch']} D3D11)\n\nStarted {today}. Project `{dest}`, prefix `{a.prefix}`.\n\n"
                     f"| | |\n|---|---|\n| Exe | `{exe}`, {info['size_mb']:.0f} MB{', version ' + info['version'] if info['version'] else ''} |\n"
                     f"| Loading | {loading_line(info)} |\n| Exe layout | {layout} |\n")

    subprocess.run(['git', 'init', '-q', '-b', 'main', dest], check=False)

    print(f"\nSCAFFOLDED {dest}")
    print(f"  arch {info['arch']}, version {info['version'] or '?'}, {info['size_mb']:.0f} MB")
    print(f"  loading: {loading_line(info)}")
    odd = info['odd'][:8] + (['...'] if len(info['odd']) > 8 else [])
    print(f"  packed/protected: {'LIKELY' if info['packed'] else 'no'}" + (f"; unusual sections: {', '.join(odd)}" if odd else ''))
    if info['neighbours']:
        print(f"  already next to the exe: {', '.join(info['neighbours'])}")
    print(f"  smoke test: {smoke}")
    print(f"  deploy files: {', '.join(files)}")
    print(f"  notes: {notes if os.path.exists(notes) else '(skill folder not found)'}")


if __name__ == '__main__':
    main()
