// D3D11/DXGI API interception. Hooks patch the shared code inside d3d11.dll /
// dxgi.dll (via MinHook on vtable targets); the game executable is not touched
// here (a game may add its own hooks from on_device_hooked).
#include "dlaa_core/hooks.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "dlaa_core/config.h"
#include "dlaa_core/frame_dump.h"
#include "dlaa_core/log.h"
#include "dlaa_core/mip_bias.h"
#include "dlaa_core/shader_registry.h"
#include "vtable_indices.h"

namespace dlaa_core::hooks {
namespace {

Callbacks g_cb;

struct Hotkey {
  int vk;
  bool debug_only;
  void (*fn)();
};
std::vector<Hotkey> g_hotkeys;
std::atomic<uint64_t> g_presents{0};

std::once_flag g_device_hooks_once;
std::once_flag g_deferred_hooks_once;
std::once_flag g_swap_chain_hooks_once;

bool MipBiasOn() { return GetCoreConfig().mip_bias != 0.0f; }

// Returns false (and logs) when the slot's code is already hooked, e.g. when a
// deferred context shares its implementation with the immediate context.
template <typename Fn>
bool HookSlot(void* object, size_t slot, Fn detour, Fn* original, const char* name) {
  void* target = (*reinterpret_cast<void***>(object))[slot];
  MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(detour),
                                   reinterpret_cast<void**>(original));
  // Enabling is queued and applied in one batch (ApplyQueued): each apply
  // suspends every thread in the process, so doing it per hook stalls startup.
  if (status == MH_OK) status = MH_QueueEnableHook(target);
  // Not an error: that code already has the hook (see above).
  Log("hook %-36s slot %2zu at %p: %s", name, slot, target,
      status == MH_ERROR_ALREADY_CREATED ? "shares code that is already hooked"
                                         : MH_StatusToString(status));
  return status == MH_OK;
}

void ApplyQueued() {
  const MH_STATUS status = MH_ApplyQueued();
  Log("hooks enabled: %s", MH_StatusToString(status));
}

// ---- ID3D11Device --------------------------------------------------------

using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                         ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                        ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateComputeShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                          ID3D11ClassLinkage*, ID3D11ComputeShader**);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT,
                                                            ID3D11DeviceContext**);
using CreateSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_SAMPLER_DESC*,
                                                         ID3D11SamplerState**);
using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*,
                                                   const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
CreateVertexShaderFn o_CreateVertexShader;
CreatePixelShaderFn o_CreatePixelShader;
CreateComputeShaderFn o_CreateComputeShader;
CreateDeferredContextFn o_CreateDeferredContext;
CreateSamplerStateFn o_CreateSamplerState;
CreateBufferFn o_CreateBuffer;

HRESULT STDMETHODCALLTYPE Hk_CreateBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* desc,
                                          const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** out) {
  const HRESULT hr = o_CreateBuffer(self, desc, data, out);
  if (SUCCEEDED(hr) && desc && out && *out) g_cb.on_create_buffer(*desc, *out);
  return hr;
}

// Every mip-mapped sampler gets a twin with the DLAA mip bias (see mip_bias.h).
HRESULT STDMETHODCALLTYPE Hk_CreateSamplerState(ID3D11Device* self, const D3D11_SAMPLER_DESC* desc,
                                                ID3D11SamplerState** out) {
  const HRESULT hr = o_CreateSamplerState(self, desc, out);
  D3D11_SAMPLER_DESC biased;
  if (SUCCEEDED(hr) && desc && out && *out && mip_bias::WantsTwin(*desc, &biased)) {
    ID3D11SamplerState* twin = nullptr;
    if (SUCCEEDED(o_CreateSamplerState(self, &biased, &twin))) {
      mip_bias::Register(*out, twin);
      twin->Release();
    }
  }
  return hr;
}

const char* StageName(Stage stage) {
  return stage == Stage::kVertex ? "vs" : stage == Stage::kPixel ? "ps" : "cs";
}

// Shared by the three shader hooks: optional game rewrite, registry, callback.
template <typename Shader, typename Create>
HRESULT CreateShader(ID3D11Device* self, Stage stage, const void* code, SIZE_T len,
                     ID3D11ClassLinkage* linkage, Shader** out, Create create) {
  HRESULT hr = E_FAIL;
  std::vector<uint8_t> rewritten;
  const void* used = code;
  size_t used_len = len;
  uint64_t hash = 0;
  if (g_cb.rewrite_shader && code && out) {
    hash = shaders::HashBytecode(code, len);
    if (const int n = g_cb.rewrite_shader(stage, hash, code, len, rewritten)) {
      hr = create(self, rewritten.data(), rewritten.size(), linkage, out);
      Log("%s %016llX: game rewrite, %d instructions changed: 0x%08X",
          stage == Stage::kVertex ? "VS" : stage == Stage::kPixel ? "PS" : "CS",
          static_cast<unsigned long long>(hash), n, hr);
      if (SUCCEEDED(hr)) {
        used = rewritten.data();
        used_len = rewritten.size();
      }
    }
  }
  if (FAILED(hr)) hr = create(self, code, len, linkage, out);
  if (SUCCEEDED(hr) && out) {
    hash = shaders::Register(*out, code, len, StageName(stage));
    if (g_cb.on_create_shader && *out)
      g_cb.on_create_shader(self, stage, *out, hash, used, used_len);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateVertexShader(ID3D11Device* self, const void* code, SIZE_T len,
                                                ID3D11ClassLinkage* linkage, ID3D11VertexShader** out) {
  return CreateShader(self, Stage::kVertex, code, len, linkage, out, o_CreateVertexShader);
}

HRESULT STDMETHODCALLTYPE Hk_CreatePixelShader(ID3D11Device* self, const void* code, SIZE_T len,
                                               ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
  return CreateShader(self, Stage::kPixel, code, len, linkage, out, o_CreatePixelShader);
}

HRESULT STDMETHODCALLTYPE Hk_CreateComputeShader(ID3D11Device* self, const void* code, SIZE_T len,
                                                 ID3D11ClassLinkage* linkage,
                                                 ID3D11ComputeShader** out) {
  const HRESULT hr =
      CreateShader(self, Stage::kCompute, code, len, linkage, out, o_CreateComputeShader);
  if (FAILED(hr)) Log("CreateComputeShader (%zu bytes) failed: 0x%08X", len, hr);
  return hr;
}

// ---- ID3D11DeviceContext -------------------------------------------------
// Instantiated once for the immediate context (0) and once for deferred
// contexts (1), in case d3d11.dll implements them with different code.

using VSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*,
                                               ID3D11ClassInstance* const*, UINT);
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*,
                                               ID3D11ClassInstance* const*, UINT);
using PSSetSamplersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                 ID3D11SamplerState* const*);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                      ID3D11RenderTargetView* const*,
                                                      ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUavsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT,
    UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using OMSetBlendStateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*,
                                                   const FLOAT[4], UINT);
using OMSetDepthStencilStateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                          ID3D11DepthStencilState*, UINT);
using ClearStateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT,
                                                        INT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*,
                                                ID3D11Resource*);
using CopySubresourceRegionFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*,
                                                         UINT, UINT, UINT, UINT, ID3D11Resource*,
                                                         UINT, const D3D11_BOX*);
using ResolveSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                                      ID3D11Resource*, UINT, DXGI_FORMAT);
using ClearRenderTargetViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                         ID3D11RenderTargetView*, const FLOAT[4]);
using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                         ID3D11DepthStencilView*, UINT, FLOAT,
                                                         UINT8);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*,
                                                      BOOL);
using FinishCommandListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL,
                                                        ID3D11CommandList**);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP,
                                          UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                                     const D3D11_BOX*, const void*, UINT, UINT);
using UpdateSubresource1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT,
                                                      const D3D11_BOX*, const void*, UINT, UINT,
                                                      UINT);

template <int Instance>
struct ContextHooks {
  static inline VSSetShaderFn o_VSSetShader;
  static inline PSSetShaderFn o_PSSetShader;
  static inline PSSetSamplersFn o_PSSetSamplers;
  static inline OMSetRenderTargetsFn o_OMSetRenderTargets;
  static inline OMSetRenderTargetsAndUavsFn o_OMSetRenderTargetsAndUavs;
  static inline OMSetBlendStateFn o_OMSetBlendState;
  static inline OMSetDepthStencilStateFn o_OMSetDepthStencilState;
  static inline ClearStateFn o_ClearState;
  static inline DrawFn o_Draw;
  static inline DrawIndexedFn o_DrawIndexed;
  static inline DrawInstancedFn o_DrawInstanced;
  static inline DrawIndexedInstancedFn o_DrawIndexedInstanced;
  static inline DispatchFn o_Dispatch;
  static inline CopyResourceFn o_CopyResource;
  static inline CopySubresourceRegionFn o_CopySubresourceRegion;
  static inline ResolveSubresourceFn o_ResolveSubresource;
  static inline ClearRenderTargetViewFn o_ClearRenderTargetView;
  static inline ClearDepthStencilViewFn o_ClearDepthStencilView;
  static inline ExecuteCommandListFn o_ExecuteCommandList;
  static inline FinishCommandListFn o_FinishCommandList;
  static inline MapFn o_Map;
  static inline UnmapFn o_Unmap;
  static inline UpdateSubresourceFn o_UpdateSubresource;
  static inline UpdateSubresource1Fn o_UpdateSubresource1;

  static void STDMETHODCALLTYPE VSSetShader(ID3D11DeviceContext* self, ID3D11VertexShader* vs,
                                            ID3D11ClassInstance* const* instances, UINT count) {
    o_VSSetShader(self, vs, instances, count);
    g_cb.on_vs_set_shader(self, vs);
  }

  static void STDMETHODCALLTYPE PSSetShader(ID3D11DeviceContext* self, ID3D11PixelShader* ps,
                                            ID3D11ClassInstance* const* instances, UINT count) {
    o_PSSetShader(self, ps, instances, count);
    g_cb.on_ps_set_shader(self, ps);
  }

  static void STDMETHODCALLTYPE PSSetSamplers(ID3D11DeviceContext* self, UINT start, UINT count,
                                              ID3D11SamplerState* const* samplers) {
    ID3D11SamplerState* scratch[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    o_PSSetSamplers(self, start, count, mip_bias::Substitute(count, samplers, scratch));
  }

  static void STDMETHODCALLTYPE OMSetRenderTargets(ID3D11DeviceContext* self, UINT count,
                                                   ID3D11RenderTargetView* const* rtvs,
                                                   ID3D11DepthStencilView* dsv) {
    o_OMSetRenderTargets(self, count, rtvs, dsv);
    g_cb.on_set_render_targets(self, count, rtvs, dsv);
  }

  static void STDMETHODCALLTYPE OMSetRenderTargetsAndUavs(
      ID3D11DeviceContext* self, UINT count, ID3D11RenderTargetView* const* rtvs,
      ID3D11DepthStencilView* dsv, UINT uav_start, UINT uav_count,
      ID3D11UnorderedAccessView* const* uavs, const UINT* initial_counts) {
    o_OMSetRenderTargetsAndUavs(self, count, rtvs, dsv, uav_start, uav_count, uavs,
                                initial_counts);
    if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
      g_cb.on_set_render_targets(self, count, rtvs, dsv);
  }

  static void STDMETHODCALLTYPE OMSetBlendState(ID3D11DeviceContext* self, ID3D11BlendState* blend,
                                                const FLOAT factor[4], UINT mask) {
    o_OMSetBlendState(self, blend, factor, mask);
    g_cb.on_set_blend_state(self, blend, factor, mask);
  }

  static void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DeviceContext* self,
                                                       ID3D11DepthStencilState* state, UINT ref) {
    o_OMSetDepthStencilState(self, state, ref);
    g_cb.on_set_depth_stencil_state(self, state, ref);
  }

  static void STDMETHODCALLTYPE ClearState(ID3D11DeviceContext* self) {
    o_ClearState(self);
    g_cb.on_clear_state(self);
  }

  static void Reissue(ID3D11DeviceContext* ctx, const DrawArgs& a) {
    switch (a.kind) {
      case DrawArgs::kDraw:
        o_Draw(ctx, a.count, a.start);
        break;
      case DrawArgs::kDrawIndexed:
        o_DrawIndexed(ctx, a.count, a.start, a.base_vertex);
        break;
      case DrawArgs::kDrawInstanced:
        o_DrawInstanced(ctx, a.count, a.instances, a.start, a.start_instance);
        break;
      case DrawArgs::kDrawIndexedInstanced:
        o_DrawIndexedInstanced(ctx, a.count, a.instances, a.start, a.base_vertex,
                               a.start_instance);
        break;
    }
  }

  // Frame dump, game callbacks around the call, the call itself.
  static void RunDraw(ID3D11DeviceContext* self, const char* op, const DrawArgs& args) {
    if (frame_dump::Active()) frame_dump::RecordDraw(self, op, args.count, args.instances);
    const uint32_t flags = g_cb.before_draw ? g_cb.before_draw(self, args) : kProceed;
    if (flags & kSkip) return;
    Reissue(self, args);
    if ((flags & kAfter) && g_cb.after_draw) g_cb.after_draw(self, args);
  }

  static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* self, UINT vertices, UINT start) {
    RunDraw(self, "Draw", {DrawArgs::kDraw, vertices, 1, start, 0, 0, &Reissue});
  }

  static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* self, UINT indices, UINT start,
                                            INT base) {
    RunDraw(self, "DrawIndexed", {DrawArgs::kDrawIndexed, indices, 1, start, base, 0, &Reissue});
  }

  static void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* self, UINT vertices,
                                              UINT instances, UINT start, UINT start_instance) {
    RunDraw(self, "DrawInstanced",
            {DrawArgs::kDrawInstanced, vertices, instances, start, 0, start_instance, &Reissue});
  }

  static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* self, UINT indices,
                                                     UINT instances, UINT start, INT base,
                                                     UINT start_instance) {
    RunDraw(self, "DrawIndexedInstanced",
            {DrawArgs::kDrawIndexedInstanced, indices, instances, start, base, start_instance,
             &Reissue});
  }

  static void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    if (frame_dump::Active()) frame_dump::RecordDispatch(self, x, y, z);
    const uint32_t flags = g_cb.before_dispatch ? g_cb.before_dispatch(self, x, y, z) : kProceed;
    if (flags & kSkip) return;
    o_Dispatch(self, x, y, z);
    if ((flags & kAfter) && g_cb.after_dispatch) g_cb.after_dispatch(self, x, y, z);
  }

  static void STDMETHODCALLTYPE CopyResource(ID3D11DeviceContext* self, ID3D11Resource* dst,
                                             ID3D11Resource* src) {
    if (frame_dump::Active()) frame_dump::RecordCopy(self, "CopyResource", dst, src);
    o_CopyResource(self, dst, src);
  }

  static void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11DeviceContext* self,
                                                      ID3D11Resource* dst, UINT dst_sub, UINT x,
                                                      UINT y, UINT z, ID3D11Resource* src,
                                                      UINT src_sub, const D3D11_BOX* box) {
    if (frame_dump::Active()) frame_dump::RecordCopy(self, "CopySubresourceRegion", dst, src);
    o_CopySubresourceRegion(self, dst, dst_sub, x, y, z, src, src_sub, box);
  }

  static void STDMETHODCALLTYPE ResolveSubresource(ID3D11DeviceContext* self, ID3D11Resource* dst,
                                                   UINT dst_sub, ID3D11Resource* src,
                                                   UINT src_sub, DXGI_FORMAT format) {
    if (frame_dump::Active()) frame_dump::RecordCopy(self, "ResolveSubresource", dst, src);
    o_ResolveSubresource(self, dst, dst_sub, src, src_sub, format);
  }

  static void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11DeviceContext* self,
                                                      ID3D11RenderTargetView* rtv,
                                                      const FLOAT color[4]) {
    if (frame_dump::Active()) frame_dump::RecordClear(self, "ClearRTV", rtv, color);
    if (g_cb.on_clear_rtv) g_cb.on_clear_rtv(self, rtv, color);
    o_ClearRenderTargetView(self, rtv, color);
  }

  static void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DeviceContext* self,
                                                      ID3D11DepthStencilView* dsv, UINT flags,
                                                      FLOAT depth, UINT8 stencil) {
    if (frame_dump::Active()) {
      const FLOAT value[4] = {depth, static_cast<FLOAT>(stencil), static_cast<FLOAT>(flags), 0};
      frame_dump::RecordClear(self, "ClearDSV", dsv, value);
    }
    if (g_cb.on_clear_dsv) g_cb.on_clear_dsv(self, dsv, flags, depth, stencil);
    o_ClearDepthStencilView(self, dsv, flags, depth, stencil);
  }

  static void STDMETHODCALLTYPE ExecuteCommandList(ID3D11DeviceContext* self,
                                                   ID3D11CommandList* list, BOOL restore) {
    if (g_cb.before_execute_command_list) g_cb.before_execute_command_list(self, list);
    if (frame_dump::Active()) frame_dump::RecordExecuteCommandList(self, list);
    o_ExecuteCommandList(self, list, restore);
    if (!restore && g_cb.on_command_list_reset) g_cb.on_command_list_reset(self);
  }

  static HRESULT STDMETHODCALLTYPE FinishCommandList(ID3D11DeviceContext* self, BOOL restore,
                                                     ID3D11CommandList** list) {
    const HRESULT hr = o_FinishCommandList(self, restore, list);
    if (!restore && g_cb.on_command_list_reset) g_cb.on_command_list_reset(self);
    if (SUCCEEDED(hr) && list && *list) {
      if (g_cb.on_finish_command_list) g_cb.on_finish_command_list(self, *list);
      if (frame_dump::Active()) frame_dump::RecordFinishCommandList(self, *list);
    }
    return hr;
  }

  static HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* self, ID3D11Resource* r, UINT sub,
                                       D3D11_MAP type, UINT flags,
                                       D3D11_MAPPED_SUBRESOURCE* mapped) {
    const HRESULT hr = o_Map(self, r, sub, type, flags, mapped);
    if (SUCCEEDED(hr) && mapped) {
      if (g_cb.on_map) g_cb.on_map(self, r, type, mapped->pData);
      if (frame_dump::Active()) frame_dump::RecordMap(self, r, type, mapped->pData);
    }
    return hr;
  }

  static void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* self, ID3D11Resource* r, UINT sub) {
    if (g_cb.on_unmap) g_cb.on_unmap(self, r);
    if (frame_dump::Active()) frame_dump::RecordUnmap(self, r);
    o_Unmap(self, r, sub);
  }

  static void STDMETHODCALLTYPE UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* r,
                                                  UINT sub, const D3D11_BOX* box,
                                                  const void* data, UINT row, UINT depth) {
    const void* sent =
        g_cb.on_update_subresource ? g_cb.on_update_subresource(self, r, sub, box, data) : data;
    if (frame_dump::Active()) frame_dump::RecordUpdate(self, "UpdateSubresource", r, box, sent);
    o_UpdateSubresource(self, r, sub, box, sent, row, depth);
  }

  static void STDMETHODCALLTYPE UpdateSubresource1(ID3D11DeviceContext1* self, ID3D11Resource* r,
                                                   UINT sub, const D3D11_BOX* box,
                                                   const void* data, UINT row, UINT depth,
                                                   UINT flags) {
    const void* sent =
        g_cb.on_update_subresource ? g_cb.on_update_subresource(self, r, sub, box, data) : data;
    if (frame_dump::Active()) frame_dump::RecordUpdate(self, "UpdateSubresource1", r, box, sent);
    o_UpdateSubresource1(self, r, sub, box, sent, row, depth, flags);
  }

  static void Install(ID3D11DeviceContext* ctx, const char* kind) {
    char name[64];
    auto hook = [&](size_t slot, auto detour, auto* original, const char* method) {
      std::snprintf(name, sizeof(name), "%s::%s", kind, method);
      HookSlot(ctx, slot, detour, original, name);
    };
    if (g_cb.on_vs_set_shader) hook(kSlotCtx_VSSetShader, &VSSetShader, &o_VSSetShader, "VSSetShader");
    if (g_cb.on_ps_set_shader) hook(kSlotCtx_PSSetShader, &PSSetShader, &o_PSSetShader, "PSSetShader");
    if (MipBiasOn()) hook(kSlotCtx_PSSetSamplers, &PSSetSamplers, &o_PSSetSamplers, "PSSetSamplers");
    if (g_cb.on_set_render_targets) {
      hook(kSlotCtx_OMSetRenderTargets, &OMSetRenderTargets, &o_OMSetRenderTargets,
           "OMSetRenderTargets");
      hook(kSlotCtx_OMSetRenderTargetsAndUnorderedAccessViews, &OMSetRenderTargetsAndUavs,
           &o_OMSetRenderTargetsAndUavs, "OMSetRenderTargetsAndUnorderedAccessViews");
    }
    if (g_cb.on_set_blend_state)
      hook(kSlotCtx_OMSetBlendState, &OMSetBlendState, &o_OMSetBlendState, "OMSetBlendState");
    if (g_cb.on_set_depth_stencil_state)
      hook(kSlotCtx_OMSetDepthStencilState, &OMSetDepthStencilState, &o_OMSetDepthStencilState,
           "OMSetDepthStencilState");
    if (g_cb.on_clear_state) hook(kSlotCtx_ClearState, &ClearState, &o_ClearState, "ClearState");
    hook(kSlotCtx_Draw, &Draw, &o_Draw, "Draw");
    hook(kSlotCtx_DrawIndexed, &DrawIndexed, &o_DrawIndexed, "DrawIndexed");
    hook(kSlotCtx_DrawInstanced, &DrawInstanced, &o_DrawInstanced, "DrawInstanced");
    hook(kSlotCtx_DrawIndexedInstanced, &DrawIndexedInstanced, &o_DrawIndexedInstanced,
         "DrawIndexedInstanced");
    hook(kSlotCtx_Dispatch, &Dispatch, &o_Dispatch, "Dispatch");
    hook(kSlotCtx_CopyResource, &CopyResource, &o_CopyResource, "CopyResource");
    hook(kSlotCtx_CopySubresourceRegion, &CopySubresourceRegion, &o_CopySubresourceRegion,
         "CopySubresourceRegion");
    hook(kSlotCtx_ResolveSubresource, &ResolveSubresource, &o_ResolveSubresource,
         "ResolveSubresource");
    hook(kSlotCtx_ClearRenderTargetView, &ClearRenderTargetView, &o_ClearRenderTargetView,
         "ClearRenderTargetView");
    hook(kSlotCtx_ClearDepthStencilView, &ClearDepthStencilView, &o_ClearDepthStencilView,
         "ClearDepthStencilView");
    hook(kSlotCtx_ExecuteCommandList, &ExecuteCommandList, &o_ExecuteCommandList,
         "ExecuteCommandList");
    hook(kSlotCtx_FinishCommandList, &FinishCommandList, &o_FinishCommandList,
         "FinishCommandList");
    hook(kSlotCtx_Map, &Map, &o_Map, "Map");
    hook(kSlotCtx_Unmap, &Unmap, &o_Unmap, "Unmap");
    hook(kSlotCtx_UpdateSubresource, &UpdateSubresource, &o_UpdateSubresource,
         "UpdateSubresource");
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
      std::snprintf(name, sizeof(name), "%s::UpdateSubresource1", kind);
      HookSlot(ctx1, kSlotCtx1_UpdateSubresource1, &UpdateSubresource1, &o_UpdateSubresource1,
               name);
      ctx1->Release();
    }
  }
};

HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext(ID3D11Device* self, UINT flags,
                                                   ID3D11DeviceContext** out) {
  const HRESULT hr = o_CreateDeferredContext(self, flags, out);
  Log("CreateDeferredContext -> 0x%08X (%p)", hr, out ? static_cast<void*>(*out) : nullptr);
  if (SUCCEEDED(hr) && out && *out) {
    // Slots whose code is shared with the immediate context report
    // "already created" and stay covered by the immediate-context hooks.
    std::call_once(g_deferred_hooks_once, [ctx = *out] {
      ContextHooks<1>::Install(ctx, "Deferred");
      ApplyQueued();
    });
  }
  return hr;
}

// ---- IDXGISwapChain ------------------------------------------------------

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT,
                                                    UINT);
using SetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
PresentFn o_Present;
ResizeBuffersFn o_ResizeBuffers;
SetFullscreenStateFn o_SetFullscreenState;

bool GameHasFocus() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

// Each key's state is read once per Present, so two hotkeys may share a key.
void PollHotkeys(uint64_t presents) {
  static bool was_down[256] = {};
  bool down[256] = {};
  bool polled[256] = {};
  auto pressed = [&](int vk) {
    if (vk <= 0 || vk > 255) return false;
    if (!polled[vk]) {
      polled[vk] = true;
      down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    return down[vk] && !was_down[vk];
  };
  const bool focus = GameHasFocus();
  const CoreConfig& cfg = GetCoreConfig();
  // Game keys may work without [Debug] Enabled=1; the core's keys never do.
  if (pressed(cfg.dump_frame_key) && focus && cfg.debug_tools) frame_dump::Request();
  if (cfg.debug_tools && cfg.dump_frame_at > 0 &&
      presents == static_cast<uint64_t>(cfg.dump_frame_at))
    frame_dump::Request();
  for (const Hotkey& k : g_hotkeys)
    if (pressed(k.vk) && focus && (!k.debug_only || cfg.debug_tools)) k.fn();
  for (int vk = 0; vk < 256; ++vk)
    if (polled[vk]) was_down[vk] = down[vk];
}

HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
  static std::atomic<bool> logged_first{false};
  DXGI_SWAP_CHAIN_DESC desc = {};
  self->GetDesc(&desc);
  if (!logged_first.exchange(true))
    Log("first Present: %ux%u fmt=%d buffers=%u windowed=%d swap_effect=%d",
        desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format, desc.BufferCount,
        desc.Windowed, desc.SwapEffect);
  const uint64_t presents = ++g_presents;
  PollHotkeys(presents);
  frame_dump::OnPresent();
  if (g_cb.on_present) g_cb.on_present(self, desc);
  return o_Present(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE Hk_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT width,
                                           UINT height, DXGI_FORMAT format, UINT flags) {
  const HRESULT hr = o_ResizeBuffers(self, count, width, height, format, flags);
  Log("ResizeBuffers(%u, %ux%u, fmt=%d) -> 0x%08X", count, width, height, format, hr);
  if (g_cb.on_resize_buffers) g_cb.on_resize_buffers(self, width, height, hr);
  return hr;
}

HRESULT STDMETHODCALLTYPE Hk_SetFullscreenState(IDXGISwapChain* self, BOOL fullscreen,
                                                IDXGIOutput* target) {
  g_cb.on_fullscreen_state(self, fullscreen);
  return o_SetFullscreenState(self, fullscreen, target);
}

// ---- IDXGIFactory --------------------------------------------------------

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                      DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
CreateSwapChainFn o_CreateSwapChain;
CreateSwapChainForHwndFn o_CreateSwapChainForHwnd;

HRESULT STDMETHODCALLTYPE Hk_CreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                             DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
  const HRESULT hr = o_CreateSwapChain(self, device, desc, out);
  Log("IDXGIFactory::CreateSwapChain -> 0x%08X", hr);
  if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(*out);
  return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device,
                                                    HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                    IDXGIOutput* output, IDXGISwapChain1** out) {
  const HRESULT hr = o_CreateSwapChainForHwnd(self, device, hwnd, desc, fs, output, out);
  Log("IDXGIFactory2::CreateSwapChainForHwnd -> 0x%08X", hr);
  if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(*out);
  return hr;
}

void HookFactoryOf(ID3D11Device* device) {
  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory* factory = nullptr;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
      SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
      SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    HookSlot(factory, kSlotFactory_CreateSwapChain, &Hk_CreateSwapChain, &o_CreateSwapChain,
             "IDXGIFactory::CreateSwapChain");
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
      HookSlot(factory2, kSlotFactory2_CreateSwapChainForHwnd, &Hk_CreateSwapChainForHwnd,
               &o_CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
      factory2->Release();
    }
  } else {
    Log("warning: could not reach the DXGI factory from the device");
  }
  if (factory) factory->Release();
  if (adapter) adapter->Release();
  if (dxgi_device) dxgi_device->Release();
}

}  // namespace

void SetCallbacks(const Callbacks& callbacks) { g_cb = callbacks; }

void AddHotkey(int vk, bool debug_only, void (*fn)()) {
  if (vk > 0 && vk <= 255 && fn) g_hotkeys.push_back({vk, debug_only, fn});
}

uint64_t PresentCount() { return g_presents.load(std::memory_order_relaxed); }

void Reissue(ID3D11DeviceContext* ctx, const DrawArgs& args) { args.reissue(ctx, args); }

HRESULT CreateComputeShaderUnhooked(ID3D11Device* device, const void* code, size_t length,
                                    ID3D11ComputeShader** out) {
  if (o_CreateComputeShader) return o_CreateComputeShader(device, code, length, nullptr, out);
  return device->CreateComputeShader(code, length, nullptr, out);
}

void OnDeviceCreated(ID3D11Device* device) {
  std::call_once(g_device_hooks_once, [device] {
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
      Log("MH_Initialize failed: %s", MH_StatusToString(init));
      return;
    }
    Log("device %p feature level 0x%X: installing hooks", static_cast<void*>(device),
        device->GetFeatureLevel());

    HookSlot(device, kSlotDevice_CreateVertexShader, &Hk_CreateVertexShader,
             &o_CreateVertexShader, "Device::CreateVertexShader");
    HookSlot(device, kSlotDevice_CreatePixelShader, &Hk_CreatePixelShader, &o_CreatePixelShader,
             "Device::CreatePixelShader");
    HookSlot(device, kSlotDevice_CreateComputeShader, &Hk_CreateComputeShader,
             &o_CreateComputeShader, "Device::CreateComputeShader");
    if (MipBiasOn())
      HookSlot(device, kSlotDevice_CreateSamplerState, &Hk_CreateSamplerState,
               &o_CreateSamplerState, "Device::CreateSamplerState");
    if (g_cb.on_create_buffer)
      HookSlot(device, kSlotDevice_CreateBuffer, &Hk_CreateBuffer, &o_CreateBuffer,
               "Device::CreateBuffer");
    HookSlot(device, kSlotDevice_CreateDeferredContext, &Hk_CreateDeferredContext,
             &o_CreateDeferredContext, "Device::CreateDeferredContext");

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    ContextHooks<0>::Install(ctx, "Immediate");
    ctx->Release();

    HookFactoryOf(device);
    if (g_cb.on_device_hooked) g_cb.on_device_hooked(device);
    ApplyQueued();
  });
}

void OnSwapChainCreated(IDXGISwapChain* swap_chain) {
  std::call_once(g_swap_chain_hooks_once, [swap_chain] {
    HookSlot(swap_chain, kSlotSwapChain_Present, &Hk_Present, &o_Present, "SwapChain::Present");
    HookSlot(swap_chain, kSlotSwapChain_ResizeBuffers, &Hk_ResizeBuffers, &o_ResizeBuffers,
             "SwapChain::ResizeBuffers");
    if (g_cb.on_fullscreen_state)
      HookSlot(swap_chain, kSlotSwapChain_SetFullscreenState, &Hk_SetFullscreenState,
               &o_SetFullscreenState, "SwapChain::SetFullscreenState");
    ApplyQueued();
  });
}

}  // namespace dlaa_core::hooks
