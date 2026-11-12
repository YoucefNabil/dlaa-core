#pragma once
// NVIDIA NGX (DLSS SDK) for DLAA on D3D11: one Init per device, then any
// number of DLAA features (e.g. a second one for a separate highlight layer).
// nvngx_dlss.dll is loaded from ModuleDir(). Immediate context only.
// x86 builds run NGX in dlaa_ngx_host.exe (next to the proxy) with the same
// API; see ngx_remote_protocol.h. Params() is null there.

#include <d3d11.h>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace dlaa_core::ngx {

// project_id: any GUID string (NVSDK_NGX_ENGINE_TYPE_CUSTOM). NGX's own
// messages go to the log. False (logged) if DLSS is not available.
bool Init(ID3D11Device* device, const char* project_id);
bool Ready();
NVSDK_NGX_Parameter* Params();  // NGX capability parameters, after Init
// Release every feature first. Mods never call it (NGX lives until the game
// exits); a test process does, or NGX's teardown can change its exit code.
void Shutdown(ID3D11Device* device);

struct FeatureDesc {
  UINT width = 0, height = 0;   // render = output size (DLAA)
  int preset = 0;               // 0 = NGX default, 1..26 = preset A..Z (J = 10)
  bool hdr = true;              // linear HDR colour input
  bool mv_low_res = true;       // motion vectors at render size
  bool auto_exposure = true;
  bool depth_inverted = false;  // reversed Z (1 = near)
};

struct EvalInputs {
  ID3D11Resource* color = nullptr;
  ID3D11Resource* output = nullptr;
  ID3D11Resource* depth = nullptr;
  ID3D11Resource* motion = nullptr;    // pixels, current -> previous
  ID3D11Resource* bias_mask = nullptr;  // optional pInBiasCurrentColorMask
  float jitter_x = 0, jitter_y = 0;    // render pixels, as DLSS defines the offset
  float mv_scale_x = 1, mv_scale_y = 1;
  UINT subrect_width = 0, subrect_height = 0;  // 0 = the feature size
  bool reset = false;
};

// Whether a Dlaa::Evaluate result is a success (NVSDK_NGX_Result_Success).
inline bool Succeeded(int result) { return result == 1; }

class Dlaa {
 public:
  Dlaa() = default;
  ~Dlaa() { Release(); }
  Dlaa(const Dlaa&) = delete;
  Dlaa& operator=(const Dlaa&) = delete;

  // Creates the feature, or recreates it when the description changed.
  // `name` only labels the log lines. Returns true when the feature is ready;
  // *recreated is set when the history is new (reset it).
  bool Ensure(ID3D11DeviceContext* ctx, const FeatureDesc& desc, const char* name = "DLAA",
              bool* recreated = nullptr);
  // NVSDK_NGX_Result (0x1 = success).
  int Evaluate(ID3D11DeviceContext* ctx, const EvalInputs& in);
  void Release();
  bool Created() const { return handle_ != nullptr || slot_ >= 0; }

 private:
  NVSDK_NGX_Handle* handle_ = nullptr;  // x64: the NGX feature
  int slot_ = -1;                        // x86: the feature's slot in dlaa_ngx_host.exe
  FeatureDesc desc_;
};

}  // namespace dlaa_core::ngx
