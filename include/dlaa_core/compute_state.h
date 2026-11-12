#pragma once
// Saves the game's compute-stage bindings, which a DLAA pass and NGX
// overwrite, and restores them when it goes out of scope. Only compute work
// and copies may run in between. (SwapDeviceContextState hung the GPU on
// NVIDIA drivers in DS3: do not use it for this.)

#include <d3d11_1.h>
#include <wrl/client.h>

namespace dlaa_core {

class ComputeState {
 public:
  explicit ComputeState(ID3D11DeviceContext* ctx) : ctx_(ctx) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    uav_count_ = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT
                                                                     : D3D11_PS_CS_UAV_REGISTER_COUNT;
    instance_count_ = D3D11_SHADER_MAX_INTERFACES;
    ctx->CSGetShader(&shader_, instances_, &instance_count_);
    ctx->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs_);
    ctx->CSGetUnorderedAccessViews(0, uav_count_, uavs_);
    ctx->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs_);
    ctx->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers_);
  }
  ~ComputeState() {
    ctx_->CSSetShader(shader_, instances_, instance_count_);
    ctx_->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs_);
    UINT keep_counts[D3D11_1_UAV_SLOT_COUNT];
    for (UINT& c : keep_counts) c = UINT(-1);
    ctx_->CSSetUnorderedAccessViews(0, uav_count_, uavs_, keep_counts);
    ctx_->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs_);
    ctx_->CSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers_);
    Release(shader_);
    for (UINT i = 0; i < instance_count_; ++i) Release(instances_[i]);
    for (auto* p : srvs_) Release(p);
    for (auto* p : uavs_) Release(p);
    for (auto* p : cbs_) Release(p);
    for (auto* p : samplers_) Release(p);
  }
  ComputeState(const ComputeState&) = delete;
  ComputeState& operator=(const ComputeState&) = delete;

  // What the game had bound (not owned by the caller; valid while this lives).
  ID3D11ShaderResourceView* Srv(UINT slot) const { return srvs_[slot]; }
  ID3D11Buffer* Cb(UINT slot) const { return cbs_[slot]; }
  ID3D11UnorderedAccessView* Uav(UINT slot) const { return uavs_[slot]; }

 private:
  template <typename T>
  static void Release(T* p) {
    if (p) p->Release();
  }
  ID3D11DeviceContext* ctx_;
  UINT uav_count_;
  UINT instance_count_;
  ID3D11ComputeShader* shader_ = nullptr;
  ID3D11ClassInstance* instances_[D3D11_SHADER_MAX_INTERFACES] = {};
  ID3D11ShaderResourceView* srvs_[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
  ID3D11UnorderedAccessView* uavs_[D3D11_1_UAV_SLOT_COUNT] = {};
  ID3D11Buffer* cbs_[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
  ID3D11SamplerState* samplers_[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
};

}  // namespace dlaa_core
