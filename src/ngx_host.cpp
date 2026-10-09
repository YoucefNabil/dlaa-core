// dlaa_ngx_host.exe: the 64-bit side of ngx_remote.cpp. A 32-bit game's proxy
// starts it with
//   dlaa_ngx_host.exe <IPC base name> <game pid> <mod prefix>
// and it runs NGX (ngx.cpp) on its own D3D11 device on the game's adapter,
// one command at a time (ngx_remote_protocol.h), until the game exits or
// sends kShutdown. Logs to <prefix>_ngx_host.log next to the exe.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cstdlib>
#include <string>

#include "dlaa_core/log.h"
#include "dlaa_core/ngx.h"
#include "ngx_remote_protocol.h"

using dlaa_core::Log;
using Microsoft::WRL::ComPtr;
namespace ngx = dlaa_core::ngx;
namespace rp = dlaa_core::ngx::remote;

namespace {

ComPtr<ID3D11Device5> g_device;
ComPtr<ID3D11DeviceContext4> g_ctx;
ComPtr<ID3D11Fence> g_to_host, g_to_game;
ngx::Dlaa g_features[rp::kMaxFeatures];
ComPtr<ID3D11Texture2D> g_textures[rp::kMaxFeatures][rp::kRoleCount];
bool g_ngx = false;

HANDLE AsHandle(uint64_t v) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v)); }

bool InitDevice(const rp::Shared& in) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  ComPtr<IDXGIAdapter1> adapter;
  DXGI_ADAPTER_DESC1 desc = {};
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    adapter->GetDesc1(&desc);
    if (desc.AdapterLuid.LowPart == in.adapter_luid_low &&
        desc.AdapterLuid.HighPart == in.adapter_luid_high) {
      break;
    }
    adapter.Reset();
  }
  if (!adapter) {
    Log("no adapter with the game's LUID %08X:%08X", in.adapter_luid_high, in.adapter_luid_low);
    return false;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                 static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device,
                                 nullptr, &ctx);
  if (SUCCEEDED(hr)) hr = device.As(&g_device);
  if (SUCCEEDED(hr)) hr = ctx.As(&g_ctx);
  if (SUCCEEDED(hr))
    hr = g_device->OpenSharedFence(AsHandle(in.fence_to_host), IID_PPV_ARGS(&g_to_host));
  if (SUCCEEDED(hr))
    hr = g_device->OpenSharedFence(AsHandle(in.fence_to_game), IID_PPV_ARGS(&g_to_game));
  CloseHandle(AsHandle(in.fence_to_host));
  CloseHandle(AsHandle(in.fence_to_game));
  if (FAILED(hr)) {
    Log("device / shared fences failed: 0x%08lX", hr);
    return false;
  }
  Log("device on %ls", desc.Description);
  return true;
}

// Handles one command; false = exit.
bool Handle(rp::Shared& s) {
  const uint32_t f = s.feature;
  const bool feature_ok = f < static_cast<uint32_t>(rp::kMaxFeatures);
  s.result = static_cast<int>(0xBAD00000);
  switch (s.command) {
    case rp::kInit: {
      if (!g_device && !InitDevice(s)) break;
      g_ngx = g_ngx || ngx::Init(g_device.Get(), s.project_id);
      s.result = g_ngx ? 1 : 0;
      break;
    }
    case rp::kCreate: {
      if (!g_ngx || !feature_ok) break;
      ngx::FeatureDesc d;
      d.width = s.width;
      d.height = s.height;
      d.preset = s.preset;
      d.hdr = s.hdr != 0;
      d.mv_low_res = s.mv_low_res != 0;
      d.auto_exposure = s.auto_exposure != 0;
      d.depth_inverted = s.depth_inverted != 0;
      char name[32];
      sprintf_s(name, "feature %u", f);
      bool recreated = false;
      s.result = g_features[f].Ensure(g_ctx.Get(), d, name, &recreated) ? 1 : 0;
      s.recreated = recreated;
      break;
    }
    case rp::kOpenTexture: {
      if (!g_device || !feature_ok || s.role >= rp::kRoleCount) {
        if (s.texture) CloseHandle(AsHandle(s.texture));
        break;
      }
      auto& slot = g_textures[f][s.role];
      slot.Reset();
      if (!s.texture) {
        s.result = 1;
        break;
      }
      const HRESULT hr = g_device->OpenSharedResource1(AsHandle(s.texture), IID_PPV_ARGS(&slot));
      CloseHandle(AsHandle(s.texture));
      if (FAILED(hr)) Log("OpenSharedResource1 (feature %u, role %u) failed: 0x%08lX", f, s.role, hr);
      s.result = SUCCEEDED(hr) ? 1 : 0;
      break;
    }
    case rp::kEvaluate: {
      if (!g_ngx) break;
      // Always signal, also on failure, or the game's GPU waits forever.
      g_ctx->Wait(g_to_host.Get(), s.wait_value);
      if (feature_ok) {
        auto& t = g_textures[f];
        ngx::EvalInputs e;
        e.color = t[rp::kColor].Get();
        e.output = t[rp::kOutput].Get();
        e.depth = t[rp::kDepth].Get();
        e.motion = t[rp::kMotion].Get();
        e.bias_mask = s.has_bias ? t[rp::kBias].Get() : nullptr;
        e.jitter_x = s.jitter_x;
        e.jitter_y = s.jitter_y;
        e.mv_scale_x = s.mv_scale_x;
        e.mv_scale_y = s.mv_scale_y;
        e.subrect_width = s.subrect_width;
        e.subrect_height = s.subrect_height;
        e.reset = s.reset != 0;
        if (e.color && e.output && e.depth && e.motion) s.result = g_features[f].Evaluate(g_ctx.Get(), e);
      }
      g_ctx->Signal(g_to_game.Get(), s.signal_value);
      g_ctx->Flush();
      static int logged = 0;
      if ((static_cast<unsigned>(s.result) & 0xFFF00000u) == 0xBAD00000u && logged < 8) {
        ++logged;
        Log("evaluate (feature %u) failed: 0x%08X", f, static_cast<unsigned>(s.result));
      }
      break;
    }
    case rp::kRelease: {
      if (!feature_ok) break;
      g_features[f].Release();
      for (auto& t : g_textures[f]) t.Reset();
      s.result = 1;
      break;
    }
    case rp::kShutdown:
      s.result = 1;
      return false;
    default:
      Log("unknown command %u", s.command);
      break;
  }
  return true;
}

void Cleanup() {
  for (auto& f : g_features) f.Release();
  for (auto& feature : g_textures)
    for (auto& t : feature) t.Reset();
  if (g_ngx) ngx::Shutdown(g_device.Get());
  g_ngx = false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc < 4) return 2;
  const std::wstring base = argv[1];
  const DWORD pid = wcstoul(argv[2], nullptr, 10);
  const std::wstring prefix = std::wstring(argv[3]) + L"_ngx_host";
  LocalFree(argv);

  dlaa_core::SetSelfModule(nullptr);
  dlaa_core::SetPrefix(prefix.c_str());
  dlaa_core::LogInit();

  const HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
  const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (base + L"_shm").c_str());
  auto* shared = mapping ? static_cast<rp::Shared*>(MapViewOfFile(
                               mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(rp::Shared)))
                         : nullptr;
  const HANDLE request = OpenEventW(SYNCHRONIZE, FALSE, (base + L"_req").c_str());
  const HANDLE done = OpenEventW(EVENT_MODIFY_STATE, FALSE, (base + L"_done").c_str());
  if (!game || !shared || !request || !done) {
    Log("IPC setup for pid %lu failed (error %lu)", pid, GetLastError());
    return 3;
  }
  if (shared->magic != rp::kMagic || shared->version != rp::kVersion) {
    Log("protocol mismatch: magic %08X version %u, expected version %u", shared->magic,
        shared->version, rp::kVersion);
    return 4;
  }
  Log("serving pid %lu (%ls)", pid, base.c_str());

  for (;;) {
    const HANDLE wait[2] = {request, game};
    if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0) {
      Log("game exited");
      break;
    }
    const bool go_on = Handle(*shared);
    SetEvent(done);
    if (!go_on) break;
  }
  Cleanup();
  Log("exit");
  return 0;
}
