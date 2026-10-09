#include "dlaa_core/ngx.h"

#include <nvsdk_ngx_helpers.h>

#include <string>

#include "dlaa_core/log.h"

namespace dlaa_core::ngx {
namespace {

NVSDK_NGX_Parameter* g_params = nullptr;
bool g_ready = false;

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string m = message ? message : "";
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  Log("NGX: %s", m.c_str());
}

bool SameDesc(const FeatureDesc& a, const FeatureDesc& b) {
  return a.width == b.width && a.height == b.height && a.preset == b.preset && a.hdr == b.hdr &&
         a.mv_low_res == b.mv_low_res && a.auto_exposure == b.auto_exposure &&
         a.depth_inverted == b.depth_inverted;
}

}  // namespace

bool Init(ID3D11Device* device, const char* project_id) {
  if (g_ready) return true;
  static const std::wstring dir = ModuleDir();
  static const wchar_t* paths[] = {dir.c_str()};
  NVSDK_NGX_FeatureCommonInfo info = {};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = 1;
  info.LoggingInfo.LoggingCallback = &NgxLog;
  info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
  NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(
      project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir.c_str(), device, &info);
  if (NVSDK_NGX_FAILED(r)) {
    Log("DLAA: NGX init failed: 0x%08X", r);
    return false;
  }
  r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
  if (NVSDK_NGX_FAILED(r) || !g_params) {
    Log("DLAA: NGX GetCapabilityParameters failed: 0x%08X", r);
    return false;
  }
  int available = 0, needs_driver = 0;
  NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                           &needs_driver);
  if (!available) {
    Log("DLAA: DLSS not available on this system (needs updated driver: %d)", needs_driver);
    return false;
  }
  g_ready = true;
  Log("DLAA: NGX ready");
  return true;
}

bool Ready() { return g_ready; }
NVSDK_NGX_Parameter* Params() { return g_params; }

void Shutdown(ID3D11Device* device) {
  if (!g_ready) return;
  if (g_params) NVSDK_NGX_D3D11_DestroyParameters(g_params);
  g_params = nullptr;
  NVSDK_NGX_D3D11_Shutdown1(device);
  g_ready = false;
}

bool Dlaa::Ensure(ID3D11DeviceContext* ctx, const FeatureDesc& desc, const char* name,
                  bool* recreated) {
  if (recreated) *recreated = false;
  if (!g_ready) return false;
  if (handle_ && SameDesc(desc, desc_)) return true;
  Release();
  // The preset hint is read at creation; 0 restores NGX's default.
  NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                            static_cast<unsigned int>(desc.preset));
  NVSDK_NGX_DLSS_Create_Params p = {};
  p.Feature.InWidth = desc.width;
  p.Feature.InHeight = desc.height;
  p.Feature.InTargetWidth = desc.width;
  p.Feature.InTargetHeight = desc.height;
  p.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
  int flags = 0;
  if (desc.hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
  if (desc.mv_low_res) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
  if (desc.auto_exposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  if (desc.depth_inverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  p.InFeatureCreateFlags = flags;
  const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &handle_, g_params, &p);
  if (NVSDK_NGX_FAILED(r)) {
    Log("%s: creating the DLSS feature (%ux%u) failed: 0x%08X", name, desc.width, desc.height, r);
    handle_ = nullptr;
    return false;
  }
  desc_ = desc;
  if (recreated) *recreated = true;
  Log("%s: DLSS feature created, %ux%u, preset %d, flags 0x%X", name, desc.width, desc.height,
      desc.preset, flags);
  return true;
}

int Dlaa::Evaluate(ID3D11DeviceContext* ctx, const EvalInputs& in) {
  if (!handle_) return NVSDK_NGX_Result_Fail;
  NVSDK_NGX_D3D11_DLSS_Eval_Params e = {};
  e.Feature.pInColor = in.color;
  e.Feature.pInOutput = in.output;
  e.pInDepth = in.depth;
  e.pInMotionVectors = in.motion;
  e.pInBiasCurrentColorMask = in.bias_mask;
  e.InJitterOffsetX = in.jitter_x;
  e.InJitterOffsetY = in.jitter_y;
  e.InRenderSubrectDimensions.Width = in.subrect_width ? in.subrect_width : desc_.width;
  e.InRenderSubrectDimensions.Height = in.subrect_height ? in.subrect_height : desc_.height;
  e.InReset = in.reset ? 1 : 0;
  e.InMVScaleX = in.mv_scale_x;
  e.InMVScaleY = in.mv_scale_y;
  return NGX_D3D11_EVALUATE_DLSS_EXT(ctx, handle_, g_params, &e);
}

void Dlaa::Release() {
  if (handle_) NVSDK_NGX_D3D11_ReleaseFeature(handle_);
  handle_ = nullptr;
}

}  // namespace dlaa_core::ngx
