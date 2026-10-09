#pragma once
#include <d3d11.h>
#include <dxgi.h>

// Capture of a few consecutive frames for offline analysis ([Debug] CaptureKey):
// DLSS's inputs and output, and the finished back buffer, as .dds files in
// <prefix>_capture_<time>\ next to the dll, plus info.txt with each frame's
// parameters. Runs on the immediate context's thread; each save stalls for
// the GPU, which only matters during the capture.
namespace dlaa_core::capture {

void Request();  // hotkey: capture the next [Debug] CaptureFrames frames
bool Active();   // this frame is being captured
// Copies `tex` (any 2D texture, one mip) to a file named f<frame>_<name>.dds.
void Save(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const char* name);
void Note(const char* fmt, ...);  // a line in info.txt, prefixed with the frame
// Before Present: saves the back buffer, then moves on to the next frame.
void OnPresent(IDXGISwapChain* swap_chain);

}  // namespace dlaa_core::capture
