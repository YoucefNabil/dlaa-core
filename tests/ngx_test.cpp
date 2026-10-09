// dlaa_core::ngx on a real device: Init, two DLAA features (the multi-feature
// case), a few Evaluates on a flat synthetic frame, and a readback that the
// output kept the input colour. Needs an NVIDIA RTX GPU; nvngx_dlss.dll is
// copied next to the exe. Exits 0 on success; NGX's messages go to
// ngx_test.log next to the exe.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dlaa_core/log.h"
#include "dlaa_core/ngx.h"

using Microsoft::WRL::ComPtr;
namespace ngx = dlaa_core::ngx;

namespace {

constexpr UINT kSize = 256;

float HalfToFloat(uint16_t h) {
  const int exponent = (h >> 10) & 0x1F;
  const int mantissa = h & 0x3FF;
  const float sign = (h & 0x8000) ? -1.0f : 1.0f;
  if (exponent == 0) return sign * std::ldexp(static_cast<float>(mantissa), -24);
  if (exponent == 31) return sign * INFINITY;
  return sign * std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
}

ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* device, DXGI_FORMAT format, UINT bind,
                                    const void* data, UINT pitch) {
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = d.Height = kSize;
  d.MipLevels = d.ArraySize = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = bind;
  D3D11_SUBRESOURCE_DATA init = {data, pitch, 0};
  ComPtr<ID3D11Texture2D> t;
  device->CreateTexture2D(&d, data ? &init : nullptr, &t);
  return t;
}

}  // namespace

int main() {
  dlaa_core::SetSelfModule(nullptr);
  dlaa_core::SetPrefix(L"ngx_test");
  dlaa_core::LogInit();

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &ctx);
  if (FAILED(hr)) {
    std::printf("D3D11CreateDevice failed: 0x%08lX\n", hr);
    return 1;
  }
  if (!ngx::Init(device.Get(), "5b6c3f2e-1d4a-4e8b-9c7f-0a1b2c3d4e5f")) {
    std::printf("ngx::Init failed (see ngx_test.log)\n");
    return 1;
  }

  // Flat colour (0.5, 0.25, 0.125, 1), depth 0.5, no motion.
  const uint16_t kColor[4] = {0x3800, 0x3400, 0x3000, 0x3C00};
  std::vector<uint16_t> color(kSize * kSize * 4);
  for (size_t i = 0; i < color.size(); ++i) color[i] = kColor[i % 4];
  std::vector<float> depth(kSize * kSize, 0.5f);
  std::vector<uint16_t> motion(kSize * kSize * 2, 0);
  auto color_tex = MakeTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                               D3D11_BIND_SHADER_RESOURCE, color.data(), kSize * 8);
  auto depth_tex = MakeTexture(device.Get(), DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                               depth.data(), kSize * 4);
  auto motion_tex = MakeTexture(device.Get(), DXGI_FORMAT_R16G16_FLOAT,
                                D3D11_BIND_SHADER_RESOURCE, motion.data(), kSize * 4);
  auto out_tex = MakeTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
  auto out2_tex = MakeTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
  if (!color_tex || !depth_tex || !motion_tex || !out_tex || !out2_tex) {
    std::printf("texture creation failed\n");
    return 1;
  }

  ngx::FeatureDesc desc;
  desc.width = desc.height = kSize;
  ngx::Dlaa main_feature, second_feature;
  bool recreated = false;
  if (!main_feature.Ensure(ctx.Get(), desc, "main", &recreated) || !recreated) {
    std::printf("Ensure(main) failed\n");
    return 1;
  }
  ngx::FeatureDesc desc2 = desc;
  desc2.preset = 10;  // J: a different description, its own feature
  if (!second_feature.Ensure(ctx.Get(), desc2, "second", &recreated) || !recreated) {
    std::printf("Ensure(second) failed\n");
    return 1;
  }
  if (!main_feature.Ensure(ctx.Get(), desc, "main", &recreated) || recreated) {
    std::printf("Ensure(main) with the same description recreated the feature\n");
    return 1;
  }

  for (int frame = 0; frame < 4; ++frame) {
    ngx::EvalInputs in;
    in.color = color_tex.Get();
    in.depth = depth_tex.Get();
    in.motion = motion_tex.Get();
    in.jitter_x = frame & 1 ? 0.25f : -0.25f;
    in.jitter_y = frame & 2 ? 0.25f : -0.25f;
    in.reset = frame == 0;
    in.output = out_tex.Get();
    const int r1 = main_feature.Evaluate(ctx.Get(), in);
    in.output = out2_tex.Get();
    const int r2 = second_feature.Evaluate(ctx.Get(), in);
    if (r1 != 1 || r2 != 1) {
      std::printf("Evaluate failed on frame %d: 0x%08X 0x%08X\n", frame, r1, r2);
      return 1;
    }
  }

  D3D11_TEXTURE2D_DESC sd = {};
  out_tex->GetDesc(&sd);
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  device->CreateTexture2D(&sd, nullptr, &staging);
  int failures = 0;
  for (ID3D11Texture2D* out : {out_tex.Get(), out2_tex.Get()}) {
    ctx->CopyResource(staging.Get(), out);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return 1;
    // Centre pixel: a flat, still frame must come out as it went in.
    const auto* row = static_cast<const uint16_t*>(m.pData) + (kSize / 2) * (m.RowPitch / 2);
    const uint16_t* px = row + (kSize / 2) * 4;
    float rgb[3];
    for (int c = 0; c < 3; ++c) rgb[c] = HalfToFloat(px[c]);
    ctx->Unmap(staging.Get(), 0);
    const float expect[3] = {0.5f, 0.25f, 0.125f};
    bool ok = true;
    for (int c = 0; c < 3; ++c) ok &= std::fabs(rgb[c] - expect[c]) < 0.02f;
    std::printf("output %s: %.4f %.4f %.4f %s\n", out == out_tex.Get() ? "main" : "second", rgb[0],
                rgb[1], rgb[2], ok ? "ok" : "WRONG");
    failures += ok ? 0 : 1;
  }
  main_feature.Release();
  second_feature.Release();
  ngx::Shutdown(device.Get());
  if (failures) return 1;
  std::printf("OK\n");
  return 0;
}
