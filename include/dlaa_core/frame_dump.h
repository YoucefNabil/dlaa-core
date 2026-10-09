#pragma once
#include <d3d11.h>

#include <atomic>
#include <string>

// Debug tool for Phase 1: records one frame's worth of immediate-context work
// (draws, dispatches, copies, clears) with shader hashes and render-target
// descriptions, and writes it to <prefix>_frame_<time>.txt next to the DLL.
// Deferred contexts are recorded from one frame earlier, so command lists built
// ahead of time are known; each list's work is spliced in where it executes.
namespace dlaa_core::frame_dump {

extern std::atomic<bool> g_active;  // checked on every draw; cheap when false

inline bool Active() { return g_active.load(std::memory_order_relaxed); }

void Request();
// Optional game section at the end of the text file: on_start runs when a dump
// starts (e.g. remember the frame number), text() when it is written.
void SetAppendix(void (*on_start)(), std::string (*text)());
void OnPresent();  // starts a requested capture, or finishes the running one

void RecordDraw(ID3D11DeviceContext* ctx, const char* op, UINT count, UINT instances);
void RecordDispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z);
void RecordCopy(ID3D11DeviceContext* ctx, const char* op, ID3D11Resource* dst, ID3D11Resource* src);
void RecordClear(ID3D11DeviceContext* ctx, const char* op, ID3D11View* view, const float value[4]);
void RecordExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list);
void RecordFinishCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list);
// Constant-buffer uploads: every one is listed in the pass list, and its bytes
// go to the <prefix>_frame_<time>.cb file (see WriteFile) for offline search.
void RecordMap(ID3D11DeviceContext* ctx, ID3D11Resource* r, D3D11_MAP type, void* data);
void RecordUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* r);
void RecordUpdate(ID3D11DeviceContext* ctx, const char* op, ID3D11Resource* r,
                  const D3D11_BOX* box, const void* data);

}  // namespace dlaa_core::frame_dump
