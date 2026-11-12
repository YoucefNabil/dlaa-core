#pragma once
// Small D3D11 helpers the DLAA passes of every game need.

#include <d3d11.h>
#include <wrl/client.h>

namespace dlaa_core {

// A single-mip, single-sample 2D texture (the working targets of a DLAA pass).
inline HRESULT MakeTexture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT bind,
                           Microsoft::WRL::ComPtr<ID3D11Texture2D>* out) {
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = bind;
  out->Reset();
  return device->CreateTexture2D(&d, nullptr, out->GetAddressOf());
}

// The 2D texture behind a view; null for no view or another kind of resource.
inline Microsoft::WRL::ComPtr<ID3D11Texture2D> TextureOf(ID3D11View* view) {
  Microsoft::WRL::ComPtr<ID3D11Resource> resource;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  if (view) view->GetResource(&resource);
  if (resource) resource.As(&tex);
  return tex;
}

// Byte size if r is a constant buffer, else 0.
inline UINT ConstantBufferBytes(ID3D11Resource* r) {
  if (!r) return 0;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
  return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ? d.ByteWidth : 0;
}

}  // namespace dlaa_core
