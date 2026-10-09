// dlaa_core::ngx for x86 mods. NGX has no 32-bit build, so this side keeps the
// ngx.h API and forwards it to dlaa_ngx_host.exe (x64, next to the proxy),
// which owns the NGX device and features. Inputs and outputs travel through
// shared twin textures, ordered on the GPU by two shared fences; see
// ngx_remote_protocol.h for the sequence.
#include "dlaa_core/ngx.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "dlaa_core/log.h"
#include "ngx_remote_protocol.h"

using Microsoft::WRL::ComPtr;

namespace dlaa_core::ngx {
namespace {
namespace rp = remote;

// NVSDK_NGX_Result values (nvsdk_ngx_defs.h), without the SDK on x86.
constexpr int kNgxSuccess = 0x1;
constexpr int kNgxFail = static_cast<int>(0xBAD00000);
bool NgxSucceeded(int r) { return (static_cast<unsigned>(r) & 0xFFF00000u) != 0xBAD00000u; }

constexpr DWORD kInitTimeoutMs = 20000;  // process start + NGX init + driver load
constexpr DWORD kCreateTimeoutMs = 10000;
constexpr DWORD kCallTimeoutMs = 2000;

struct Twin {
  ComPtr<ID3D11Texture2D> texture;
  UINT width = 0, height = 0;
  DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
};

struct State {
  bool ready = false;
  bool tried = false;  // Init failed once: don't relaunch the host every frame
  ComPtr<ID3D11Device5> device;
  ComPtr<ID3D11DeviceContext4> immediate;
  ComPtr<ID3D11Fence> to_host, to_game;
  uint64_t to_host_value = 0, to_game_value = 0;
  HANDLE mapping = nullptr;
  rp::Shared* shared = nullptr;
  HANDLE request = nullptr, done = nullptr;
  HANDLE process = nullptr, job = nullptr;
  bool used[rp::kMaxFeatures] = {};
  Twin twins[rp::kMaxFeatures][rp::kRoleCount];
};

// Never destroyed: Dlaa destructors may still run at process exit.
State& S() {
  static State* s = new State;
  return *s;
}

bool SameDesc(const FeatureDesc& a, const FeatureDesc& b) {
  return a.width == b.width && a.height == b.height && a.preset == b.preset && a.hdr == b.hdr &&
         a.mv_low_res == b.mv_low_res && a.auto_exposure == b.auto_exposure &&
         a.depth_inverted == b.depth_inverted;
}

// Typed twin of a typeless source (CopySubresourceRegion accepts the same
// format group), so NGX in the host can view it without knowing the type.
DXGI_FORMAT TwinFormat(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return f;
  }
}

void CloseAll() {
  auto& s = S();
  s.ready = false;
  for (auto& feature : s.twins)
    for (auto& twin : feature) twin = Twin{};
  for (bool& u : s.used) u = false;
  if (s.process) {
    TerminateProcess(s.process, 1);
    CloseHandle(s.process);
  }
  if (s.job) CloseHandle(s.job);
  if (s.shared) UnmapViewOfFile(s.shared);
  for (HANDLE h : {s.mapping, s.request, s.done})
    if (h) CloseHandle(h);
  s.process = s.job = s.mapping = s.request = s.done = nullptr;
  s.shared = nullptr;
  s.to_host.Reset();
  s.to_game.Reset();
  s.immediate.Reset();
  s.device.Reset();
}

// Sends the command filled in S().shared and waits until the host answered.
// A timeout or a dead host ends remote DLSS for the session.
bool Call(rp::Command command, DWORD timeout_ms) {
  auto& s = S();
  if (!s.shared || !s.process) return false;
  s.shared->command = command;
  s.shared->result = kNgxFail;
  s.shared->recreated = 0;
  SetEvent(s.request);
  const HANDLE wait[2] = {s.done, s.process};
  const DWORD w = WaitForMultipleObjects(2, wait, FALSE, timeout_ms);
  if (w == WAIT_OBJECT_0) return true;
  if (w == WAIT_OBJECT_0 + 1) {
    DWORD code = 0;
    GetExitCodeProcess(s.process, &code);
    Log("DLAA: %ls exited (code %lu) during command %u; DLSS off", rp::kHostExe, code, command);
  } else {
    Log("DLAA: %ls did not answer command %u within %lu ms; DLSS off", rp::kHostExe, command,
        timeout_ms);
  }
  CloseAll();
  return false;
}

// Duplicates `h` into the host (closing ours) and returns the host's value.
uint64_t GiveToHost(HANDLE h) {
  HANDLE out = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), h, S().process, &out, 0, FALSE,
                       DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
    Log("DLAA: DuplicateHandle into the host failed (error %lu)", GetLastError());
    CloseHandle(h);
    return 0;
  }
  return reinterpret_cast<uintptr_t>(out);
}

bool LaunchHost(const std::wstring& base) {
  auto& s = S();
  const std::wstring exe = ModuleDir() + rp::kHostExe;
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    Log("DLAA: %ls not found; DLSS needs it on 32-bit games", exe.c_str());
    return false;
  }
  std::wstring cmd = L"\"" + exe + L"\" " + base + L" " + std::to_wstring(GetCurrentProcessId()) +
                     L" " + Prefix();
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, ModuleDir().c_str(), &si,
                      &pi)) {
    Log("DLAA: starting %ls failed (error %lu)", exe.c_str(), GetLastError());
    return false;
  }
  // The host dies with the game, even when the game is killed.
  s.job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!s.job ||
      !SetInformationJobObject(s.job, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits)) ||
      !AssignProcessToJobObject(s.job, pi.hProcess)) {
    Log("DLAA: no job object for the host (error %lu); it exits on its own with the game",
        GetLastError());
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  s.process = pi.hProcess;
  Log("DLAA: started %ls (pid %lu)", rp::kHostExe, pi.dwProcessId);
  return true;
}

// Makes the host see a shared copy of `source` for (feature, role); recreated
// when the size or format changes.
ID3D11Texture2D* EnsureTwin(int feature, rp::Role role, ID3D11Resource* source) {
  auto& s = S();
  ComPtr<ID3D11Texture2D> tex;
  if (FAILED(source->QueryInterface(IID_PPV_ARGS(&tex)))) return nullptr;
  D3D11_TEXTURE2D_DESC src;
  tex->GetDesc(&src);
  Twin& twin = s.twins[feature][role];
  if (twin.texture && twin.width == src.Width && twin.height == src.Height &&
      twin.source_format == src.Format) {
    return twin.texture.Get();
  }
  twin = Twin{};
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = src.Width;
  d.Height = src.Height;
  d.MipLevels = d.ArraySize = 1;
  d.Format = TwinFormat(src.Format);
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (role == rp::kOutput ? D3D11_BIND_UNORDERED_ACCESS : 0);
  d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  ComPtr<ID3D11Texture2D> t;
  HRESULT hr = s.device->CreateTexture2D(&d, nullptr, &t);
  ComPtr<IDXGIResource1> dxgi;
  HANDLE h = nullptr;
  if (SUCCEEDED(hr)) hr = t.As(&dxgi);
  if (SUCCEEDED(hr))
    hr = dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                  nullptr, &h);
  if (FAILED(hr)) {
    Log("DLAA: shared texture for role %u (%ux%u, format %d) failed: 0x%08lX", role, d.Width,
        d.Height, d.Format, hr);
    return nullptr;
  }
  s.shared->feature = feature;
  s.shared->role = role;
  s.shared->texture = GiveToHost(h);
  if (!s.shared->texture || !Call(rp::kOpenTexture, kCallTimeoutMs) ||
      s.shared->result != kNgxSuccess) {
    Log("DLAA: the host could not open the shared texture for role %u", role);
    return nullptr;
  }
  twin.texture = t;
  twin.width = src.Width;
  twin.height = src.Height;
  twin.source_format = src.Format;
  return t.Get();
}

}  // namespace

bool Init(ID3D11Device* device, const char* project_id) {
  auto& s = S();
  if (s.ready) return true;
  if (s.tried) return false;
  s.tried = true;

  ComPtr<ID3D11DeviceContext> immediate;
  device->GetImmediateContext(&immediate);
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&s.device))) ||
      FAILED(immediate.As(&s.immediate))) {
    Log("DLAA: no ID3D11Device5 / ID3D11DeviceContext4 (needs Windows 10 1703+)");
    CloseAll();
    return false;
  }
  HRESULT hr = s.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.to_host));
  if (SUCCEEDED(hr))
    hr = s.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.to_game));
  HANDLE fence_handles[2] = {};
  if (SUCCEEDED(hr)) hr = s.to_host->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handles[0]);
  if (SUCCEEDED(hr)) hr = s.to_game->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handles[1]);
  if (FAILED(hr)) {
    Log("DLAA: shared fences failed: 0x%08lX", hr);
    for (HANDLE h : fence_handles)
      if (h) CloseHandle(h);
    CloseAll();
    return false;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC adapter_desc = {};
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
      SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
    adapter->GetDesc(&adapter_desc);
  }

  wchar_t base[96];
  swprintf(base, std::size(base), L"Local\\dlaa_ngx_%lu_%llu", GetCurrentProcessId(),
           static_cast<unsigned long long>(GetTickCount64()));
  const std::wstring b = base;
  s.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                 sizeof(rp::Shared), (b + L"_shm").c_str());
  if (s.mapping)
    s.shared = static_cast<rp::Shared*>(
        MapViewOfFile(s.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(rp::Shared)));
  s.request = CreateEventW(nullptr, FALSE, FALSE, (b + L"_req").c_str());
  s.done = CreateEventW(nullptr, FALSE, FALSE, (b + L"_done").c_str());
  if (!s.shared || !s.request || !s.done || !LaunchHost(b)) {
    if (!s.shared || !s.request || !s.done) Log("DLAA: IPC setup failed (error %lu)", GetLastError());
    for (HANDLE h : fence_handles) CloseHandle(h);
    CloseAll();
    return false;
  }
  std::memset(s.shared, 0, sizeof(rp::Shared));
  s.shared->magic = rp::kMagic;
  s.shared->version = rp::kVersion;
  s.shared->adapter_luid_low = adapter_desc.AdapterLuid.LowPart;
  s.shared->adapter_luid_high = adapter_desc.AdapterLuid.HighPart;
  strncpy_s(s.shared->project_id, project_id, _TRUNCATE);
  s.shared->fence_to_host = GiveToHost(fence_handles[0]);
  s.shared->fence_to_game = GiveToHost(fence_handles[1]);
  if (!s.shared->fence_to_host || !s.shared->fence_to_game || !Call(rp::kInit, kInitTimeoutMs) ||
      s.shared->result != kNgxSuccess) {
    Log("DLAA: NGX init in %ls failed (see its log)", rp::kHostExe);
    if (s.shared) Call(rp::kShutdown, kCallTimeoutMs);
    CloseAll();
    return false;
  }
  s.ready = true;
  Log("DLAA: NGX ready in %ls (adapter %ls)", rp::kHostExe, adapter_desc.Description);
  return true;
}

bool Ready() { return S().ready; }
NVSDK_NGX_Parameter* Params() { return nullptr; }

void Shutdown(ID3D11Device*) {
  auto& s = S();
  if (!s.ready) return;
  if (Call(rp::kShutdown, kCallTimeoutMs)) WaitForSingleObject(s.process, kCallTimeoutMs);
  CloseAll();
}

bool Dlaa::Ensure(ID3D11DeviceContext*, const FeatureDesc& desc, const char* name,
                  bool* recreated) {
  if (recreated) *recreated = false;
  auto& s = S();
  if (!s.ready) {
    slot_ = -1;
    return false;
  }
  if (slot_ >= 0 && SameDesc(desc, desc_)) return true;
  if (slot_ < 0) {
    for (int i = 0; i < rp::kMaxFeatures; ++i) {
      if (!s.used[i]) {
        s.used[i] = true;
        slot_ = i;
        break;
      }
    }
    if (slot_ < 0) {
      Log("%s: no free feature slot in the host (max %d)", name, rp::kMaxFeatures);
      return false;
    }
  }
  s.shared->feature = slot_;
  s.shared->width = desc.width;
  s.shared->height = desc.height;
  s.shared->preset = desc.preset;
  s.shared->hdr = desc.hdr;
  s.shared->mv_low_res = desc.mv_low_res;
  s.shared->auto_exposure = desc.auto_exposure;
  s.shared->depth_inverted = desc.depth_inverted;
  if (!Call(rp::kCreate, kCreateTimeoutMs) || s.shared->result != kNgxSuccess) {
    Log("%s: creating the DLSS feature (%ux%u) in the host failed", name, desc.width, desc.height);
    Release();
    return false;
  }
  desc_ = desc;
  if (recreated) *recreated = true;
  Log("%s: DLSS feature created in the host (slot %d), %ux%u, preset %d", name, slot_, desc.width,
      desc.height, desc.preset);
  return true;
}

int Dlaa::Evaluate(ID3D11DeviceContext* ctx, const EvalInputs& in) {
  auto& s = S();
  if (slot_ < 0 || !s.ready) return kNgxFail;
  if (!in.color || !in.output || !in.depth || !in.motion) return kNgxFail;
  ID3D11Resource* const sources[rp::kRoleCount] = {in.color, in.output, in.depth, in.motion,
                                                   in.bias_mask};
  ID3D11Texture2D* twins[rp::kRoleCount] = {};
  for (uint32_t role = 0; role < rp::kRoleCount; ++role) {
    if (!sources[role]) continue;
    twins[role] = EnsureTwin(slot_, static_cast<rp::Role>(role), sources[role]);
    if (!twins[role]) return kNgxFail;
  }
  for (uint32_t role = 0; role < rp::kRoleCount; ++role) {
    if (role != rp::kOutput && twins[role])
      ctx->CopySubresourceRegion(twins[role], 0, 0, 0, 0, sources[role], 0, nullptr);
  }
  s.immediate->Signal(s.to_host.Get(), ++s.to_host_value);
  ctx->Flush();

  s.shared->feature = slot_;
  s.shared->jitter_x = in.jitter_x;
  s.shared->jitter_y = in.jitter_y;
  s.shared->mv_scale_x = in.mv_scale_x;
  s.shared->mv_scale_y = in.mv_scale_y;
  s.shared->subrect_width = in.subrect_width;
  s.shared->subrect_height = in.subrect_height;
  s.shared->reset = in.reset;
  s.shared->has_bias = in.bias_mask != nullptr;
  s.shared->wait_value = s.to_host_value;
  s.shared->signal_value = ++s.to_game_value;
  if (!Call(rp::kEvaluate, kCallTimeoutMs)) return kNgxFail;
  // The host submitted the evaluate and its signal: the GPU orders the rest.
  const int result = s.shared->result;
  s.immediate->Wait(s.to_game.Get(), s.to_game_value);
  if (NgxSucceeded(result))
    ctx->CopySubresourceRegion(in.output, 0, 0, 0, 0, twins[rp::kOutput], 0, nullptr);
  return result;
}

void Dlaa::Release() {
  if (slot_ < 0) return;
  auto& s = S();
  if (s.ready) {
    s.shared->feature = slot_;
    Call(rp::kRelease, kCallTimeoutMs);
  }
  if (s.ready) {  // Call may have closed everything
    for (auto& twin : s.twins[slot_]) twin = Twin{};
    s.used[slot_] = false;
  }
  slot_ = -1;
}

}  // namespace dlaa_core::ngx
