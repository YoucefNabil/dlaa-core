#pragma once
// Debug view: draws a texture over the back buffer at Present, after tone
// mapping, auto exposure and UI, so its colours are not changed by the game.

#include <d3d11.h>
#include <dxgi.h>

namespace dlaa_core::debug_overlay {

// Full-screen copy of `srv` onto back buffer 0, only when the back buffer is
// width x height. Saves and restores the pipeline state it touches. Call from
// on_present (immediate context thread).
void Draw(IDXGISwapChain* swap_chain, ID3D11ShaderResourceView* srv, UINT width, UINT height);

}  // namespace dlaa_core::debug_overlay
