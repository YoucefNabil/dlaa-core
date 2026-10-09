#pragma once
// D3D11/DXGI interception shared by every mod. The core hooks patch the shared
// code inside d3d11.dll / dxgi.dll (MinHook on vtable targets) and do the
// generic work (shader registry, frame dump, mip bias, debug keys); the game
// plugs in through Callbacks. A hook whose only job is a game callback is
// installed only when that callback is set.

#include <d3d11.h>
#include <dxgi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dlaa_core::hooks {

enum class Stage { kVertex, kPixel, kCompute };

struct DrawArgs {
  enum Kind { kDraw, kDrawIndexed, kDrawInstanced, kDrawIndexedInstanced } kind;
  UINT count;           // vertices or indices
  UINT instances;       // 1 for Draw / DrawIndexed
  UINT start;           // start vertex or index
  INT base_vertex;      // DrawIndexed*
  UINT start_instance;  // *Instanced
  // The unhooked call of the context that issued it; use Reissue().
  void (*reissue)(ID3D11DeviceContext* ctx, const DrawArgs& args);
};

// before_draw / before_dispatch result bits.
enum : uint32_t {
  kProceed = 0,
  kSkip = 1,   // do not issue the game's call
  kAfter = 2,  // call after_draw / after_dispatch once it ran
};

struct Callbacks {
  // Device hooks are queued but not yet enabled: add the game's own hooks here
  // (MH_CreateHook + MH_QueueEnableHook); the core applies them all at once.
  void (*on_device_hooked)(ID3D11Device* device) = nullptr;
  void (*on_create_buffer)(const D3D11_BUFFER_DESC& desc, ID3D11Buffer* buffer) = nullptr;

  // Shader creation. rewrite_shader may fill `out` with replacement bytecode and
  // return the number of changed instructions (0 = keep the game's). If
  // creating the rewrite fails, the game's bytecode is used. The registry
  // always keys the shader by the game's bytecode hash.
  int (*rewrite_shader)(Stage stage, uint64_t hash, const void* code, size_t length,
                        std::vector<uint8_t>& out) = nullptr;
  // After a successful create; `code` is the bytecode actually created.
  void (*on_create_shader)(ID3D11Device* device, Stage stage, ID3D11DeviceChild* shader,
                           uint64_t hash, const void* code, size_t length) = nullptr;

  // State setters, called after the game's call went through.
  void (*on_vs_set_shader)(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs) = nullptr;
  void (*on_ps_set_shader)(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps) = nullptr;
  void (*on_clear_state)(ID3D11DeviceContext* ctx) = nullptr;
  // OMSetRenderTargets, and OMSetRenderTargetsAndUnorderedAccessViews unless it
  // keeps the render targets.
  void (*on_set_render_targets)(ID3D11DeviceContext* ctx, UINT count,
                                ID3D11RenderTargetView* const* rtvs,
                                ID3D11DepthStencilView* dsv) = nullptr;
  void (*on_set_blend_state)(ID3D11DeviceContext* ctx, ID3D11BlendState* blend,
                             const FLOAT factor[4], UINT mask) = nullptr;
  void (*on_set_depth_stencil_state)(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* state,
                                     UINT ref) = nullptr;

  // Draws and dispatches, every context. The frame dump records the call first.
  uint32_t (*before_draw)(ID3D11DeviceContext* ctx, const DrawArgs& args) = nullptr;
  void (*after_draw)(ID3D11DeviceContext* ctx, const DrawArgs& args) = nullptr;
  uint32_t (*before_dispatch)(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) = nullptr;
  void (*after_dispatch)(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) = nullptr;

  void (*on_clear_rtv)(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                       const FLOAT color[4]) = nullptr;
  void (*on_clear_dsv)(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags,
                       FLOAT depth, UINT8 stencil) = nullptr;

  // Map: after a successful map. Unmap: before the real Unmap, while the data
  // is still mapped (the game may still change it, e.g. jitter a camera).
  void (*on_map)(ID3D11DeviceContext* ctx, ID3D11Resource* r, D3D11_MAP type,
                 void* data) = nullptr;
  void (*on_unmap)(ID3D11DeviceContext* ctx, ID3D11Resource* r) = nullptr;
  // Returns the data to upload: `data` itself, or a copy the game changed
  // (kept alive until the call returns, e.g. a thread_local buffer).
  const void* (*on_update_subresource)(ID3D11DeviceContext* ctx, ID3D11Resource* r, UINT sub,
                                       const D3D11_BOX* box, const void* data) = nullptr;

  // Before the real Present, after the core's hotkeys and the frame dump.
  void (*on_present)(IDXGISwapChain* swap_chain, const DXGI_SWAP_CHAIN_DESC& desc) = nullptr;
  void (*on_resize_buffers)(IDXGISwapChain* swap_chain, UINT width, UINT height,
                            HRESULT result) = nullptr;
  void (*on_fullscreen_state)(IDXGISwapChain* swap_chain, BOOL fullscreen) = nullptr;
};

// Call from game::OnAttach, before the game creates its device.
void SetCallbacks(const Callbacks& callbacks);

// Hotkeys checked at Present while the game window has focus. debug_only keys
// need [Debug] Enabled=1. vk <= 0 disables the key.
void AddHotkey(int vk, bool debug_only, void (*fn)());

uint64_t PresentCount();  // Presents so far (the current one included, inside on_present)

// Issues a draw again without the hooks (for a game redraw in after_draw).
void Reissue(ID3D11DeviceContext* ctx, const DrawArgs& args);
// Creates a compute shader without the hooks (no registry, no rewrite).
HRESULT CreateComputeShaderUnhooked(ID3D11Device* device, const void* code, size_t length,
                                    ID3D11ComputeShader** out);

// From the proxy entry points (dllmain.cpp).
void OnDeviceCreated(ID3D11Device* device);
void OnSwapChainCreated(IDXGISwapChain* swap_chain);

}  // namespace dlaa_core::hooks
