#include "dlaa_core/capture.h"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dlaa_core/config.h"
#include "dlaa_core/log.h"

using Microsoft::WRL::ComPtr;

namespace dlaa_core::capture {
namespace {

std::atomic<bool> g_requested{false};
bool g_active = false;
int g_frame = 0;  // index within the capture
std::wstring g_dir;
FILE* g_info = nullptr;

UINT BytesPerPixel(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return 8;
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return 4;
    case DXGI_FORMAT_R8_UNORM:
      return 1;
    default:
      return 0;
  }
}

// DDS with the DX10 extension header, rows tightly packed.
bool WriteDds(const std::wstring& path, const D3D11_TEXTURE2D_DESC& d, const uint8_t* data,
              UINT pitch, UINT bpp) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  uint32_t header[32] = {};
  header[0] = 0x20534444;  // "DDS "
  header[1] = 124;
  header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8;  // caps, height, width, pixelformat, pitch
  header[3] = d.Height;
  header[4] = d.Width;
  header[5] = d.Width * bpp;
  header[7] = 1;      // mip count
  header[19] = 32;    // pixel format size
  header[20] = 0x4;   // DDPF_FOURCC
  header[21] = 0x30315844;  // "DX10"
  header[27] = 0x1000;      // DDSCAPS_TEXTURE
  const uint32_t dx10[5] = {static_cast<uint32_t>(d.Format), 3 /* TEXTURE2D */, 0, 1, 0};
  fwrite(header, sizeof(header), 1, f);
  fwrite(dx10, sizeof(dx10), 1, f);
  for (UINT y = 0; y < d.Height; ++y) fwrite(data + y * pitch, d.Width * bpp, 1, f);
  fclose(f);
  return true;
}

}  // namespace

void Request() {
  if (g_active || g_requested.exchange(true)) return;
  Log("capture: requested, %d frames", GetCoreConfig().capture_frames);
}

bool Active() { return g_active; }

void Note(const char* fmt, ...) {
  if (!g_active || !g_info) return;
  fprintf(g_info, "f%02d ", g_frame);
  va_list args;
  va_start(args, fmt);
  vfprintf(g_info, fmt, args);
  va_end(args);
  fputc('\n', g_info);
}

void Save(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const char* name) {
  if (!g_active || !tex) return;
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  const UINT bpp = BytesPerPixel(d.Format);
  if (!bpp || d.SampleDesc.Count != 1) {
    Note("%s: format %d not saved", name, d.Format);
    return;
  }
  ComPtr<ID3D11Device> device;
  ctx->GetDevice(&device);
  D3D11_TEXTURE2D_DESC s = d;
  s.MipLevels = 1;
  s.ArraySize = 1;
  s.Usage = D3D11_USAGE_STAGING;
  s.BindFlags = 0;
  s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  s.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&s, nullptr, &staging))) {
    Note("%s: staging texture failed", name);
    return;
  }
  ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, nullptr);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
    Note("%s: map failed", name);
    return;
  }
  wchar_t file[64];
  swprintf(file, 64, L"f%02d_%hs.dds", g_frame, name);
  const bool ok = WriteDds(g_dir + file, d, static_cast<const uint8_t*>(m.pData), m.RowPitch, bpp);
  ctx->Unmap(staging.Get(), 0);
  Note("%s: %ux%u format %d %s", name, d.Width, d.Height, d.Format, ok ? "saved" : "write failed");
}

void OnPresent(IDXGISwapChain* swap_chain) {
  if (g_active) {
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11Device> device;
    if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back))) &&
        SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(&device)))) {
      ComPtr<ID3D11DeviceContext> ctx;
      device->GetImmediateContext(&ctx);
      Save(ctx.Get(), back.Get(), "final");
    }
    if (++g_frame >= GetCoreConfig().capture_frames) {
      g_active = false;
      if (g_info) fclose(g_info);
      g_info = nullptr;
      Log("capture: done");
    }
    return;
  }
  if (!g_requested.exchange(false)) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf(name, 64, L"_capture_%04d%02d%02d_%02d%02d%02d\\", t.wYear, t.wMonth, t.wDay, t.wHour,
           t.wMinute, t.wSecond);
  g_dir = ModuleDir() + Prefix() + name;
  if (!CreateDirectoryW(g_dir.c_str(), nullptr)) {
    Log("capture: cannot create the capture folder");
    return;
  }
  if (_wfopen_s(&g_info, (g_dir + L"info.txt").c_str(), L"w") != 0) g_info = nullptr;
  g_frame = 0;
  g_active = true;  // the next frame
  Log("capture: started");
}

}  // namespace dlaa_core::capture
