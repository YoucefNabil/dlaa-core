# Quiet CMake build for a dlaa-core game repo (or dlaa-core itself).
# Prints only compiler / linker / fxc errors and warnings, then one OK line
# with the built d3d11.dll. Exit code 0 = success.
#
#   tools\build.ps1                    # configure if needed, build Release
#   tools\build.ps1 -Config Debug
#   tools\build.ps1 -Target smoke_test,rain_test
#   tools\build.ps1 -Clangd            # also refresh build\compile_commands.json for clangd
#   tools\build.ps1 -Arch Win32 -BuildDir build32   # 32-bit game (NGX in dlaa_ngx_host.exe)
#
# Each repo's tools\build.ps1 forwards here with -Root set to the repo.
param(
  [string]$Root = (Split-Path -Parent $PSScriptRoot),
  [string]$Config = 'Release',
  [string]$BuildDir = 'build',
  [ValidateSet('x64', 'Win32')][string]$Arch = 'x64',
  [string[]]$Target = @(),
  [switch]$Clangd,
  [switch]$Reconfigure
)
# 'Continue': Windows PowerShell 5.1 turns native stderr into terminating errors under 'Stop'.
$ErrorActionPreference = 'Continue'
$Root = (Resolve-Path $Root).Path
$build = Join-Path $Root $BuildDir
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  $env:PATH = "$env:ProgramFiles\CMake\bin;$env:PATH"
}

function Show-Problems([string[]]$lines) {
  # MSBuild repeats every diagnostic in its summary; MSB8029 (build dir under %TEMP%) is noise.
  $seen = @{}
  foreach ($l in $lines) {
    if ($l -notmatch '(error|warning) [A-Z]*\d+|fatal error|CMake Error') { continue }
    if ($l -match 'MSB8029') { continue }
    $t = ($l -replace '\s*\[[^\]]+\.vcxproj\]\s*$', '').Trim()
    $t = $t.Replace("$Root\", '')
    if (-not $seen.ContainsKey($t)) { $seen[$t] = 1; Write-Output $t }
  }
}

function Import-VsDevEnv {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vs) { throw 'Visual Studio with the C++ x64 tools not found' }
  # Match the game's bitness: a 32-bit repo's CMakeLists only adds its own sources on x86.
  $vcvars = Join-Path $vs ('VC\Auxiliary\Build\' + $(if ($Arch -eq 'Win32') { 'vcvars32.bat' } else { 'vcvars64.bat' }))
  $env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars calls vswhere itself
  foreach ($line in (& cmd.exe /c "`"$vcvars`" >nul 2>nul && set")) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
  }
}

# ---- configure (once; CMake re-runs itself when a CMakeLists changes) ----
if ($Reconfigure -or -not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
  $out = & cmake -S $Root -B $build -G 'Visual Studio 17 2022' -A $Arch -Wno-dev -Wno-deprecated 2>&1 |
    ForEach-Object { "$_" }
  if ($LASTEXITCODE -ne 0) {
    Show-Problems $out
    $out | Select-Object -Last 20
    Write-Output "FAILED: configure ($LASTEXITCODE)"
    exit 1
  }
}

# ---- build ----
$args_ = @('--build', $build, '--config', $Config)
if ($Target.Count) { $args_ += @('--target') + $Target }
$args_ += @('--', '-m', '-v:m', '-nologo')
$start = Get-Date
$out = & cmake @args_ 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$problems = @(Show-Problems $out)
$problems
if ($code -ne 0) {
  if (-not $problems.Count) { $out | Select-Object -Last 30 }
  Write-Output "FAILED: build ($code)"
  exit 1
}

# ---- clangd compile database (Ninja + cl.exe, generated shader headers built) ----
# Also refreshed on a full build when it's missing or older than a CMakeLists.txt, so clangd
# always has one without passing -Clangd.
$dbFile = Join-Path $build 'compile_commands.json'
$lists = @(Join-Path $Root 'CMakeLists.txt') + @(Join-Path $PSScriptRoot '..\CMakeLists.txt') +
  @(Get-ChildItem $Root -Directory -Exclude 'build*', 'external', '.*' |
    ForEach-Object { Join-Path $_.FullName 'CMakeLists.txt' })
$dbTime = if (Test-Path $dbFile) { (Get-Item $dbFile).LastWriteTime } else { [datetime]::MinValue }
$stale = @($lists | Where-Object { (Test-Path $_) -and (Get-Item $_).LastWriteTime -gt $dbTime }).Count -gt 0
if ($Clangd -or ($stale -and -not $Target.Count)) {
  $db = Join-Path $build 'clangd'
  # A cache made with the other bitness's cl.exe would be wiped mid-configure, dropping the -D options.
  $cache = Join-Path $db 'CMakeCache.txt'
  $want = if ($Arch -eq 'Win32') { '/x86/cl.exe' } else { '/x64/cl.exe' }
  if ((Test-Path $cache) -and -not (Select-String -Path $cache -SimpleMatch "$want" -Quiet)) {
    Remove-Item $cache, (Join-Path $db 'CMakeFiles') -Recurse -Force -ErrorAction SilentlyContinue
  }
  Import-VsDevEnv
  $cfg = & cmake -S $Root -B $db -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -Wno-dev -Wno-deprecated 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -ne 0) { Show-Problems $cfg; Write-Output 'FAILED: clangd configure'; exit 1 }
  $fxc = & ninja -C $db -t targets all | ForEach-Object { if ($_ -match '^(fxc_\w+): phony') { $Matches[1] } }
  if ($fxc) { & ninja -C $db @fxc | Out-Null }
  Copy-Item (Join-Path $db 'compile_commands.json') (Join-Path $build 'compile_commands.json') -Force
  Write-Output "clangd: $BuildDir\compile_commands.json"
}

$dll = Get-ChildItem -Path $build -Recurse -Filter d3d11.dll -ErrorAction SilentlyContinue |
  Where-Object { $_.DirectoryName -like "*\$Config" -and $_.FullName -notlike '*\clangd\*' } |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
$secs = [int]((Get-Date) - $start).TotalSeconds
if ($dll) {
  Write-Output ("OK  {0}  {1} bytes  ({2}s)" -f $dll.FullName.Replace("$Root\", ''), $dll.Length, $secs)
} else {
  Write-Output "OK  ($secs s)"
}
exit 0
