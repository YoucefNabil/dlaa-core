#pragma once
// Wire format between ngx_remote.cpp (the x86 mod, in the game process) and
// ngx_host.cpp (dlaa_ngx_host.exe, x64). NGX has no 32-bit build, so a 32-bit
// game runs DLSS in the helper:
//
//   game (x86)                              dlaa_ngx_host.exe (x64)
//   inputs -> shared twin textures
//   Signal(to_host, n), Flush
//   command kEvaluate --------------------> Wait(to_host, n), NGX evaluate,
//                                           Signal(to_game, m), Flush
//   <------------------------------ done (the evaluate is submitted, not run)
//   Wait(to_game, m) on the GPU
//   output twin -> game output
//
// Textures are D3D11 shared NT handles and the fences are D3D11 shared fences.
// The game process duplicates every handle into the host. One request at a time.
// Plain data only: both sides have different pointer sizes, so handles are uint64_t.
#include <cstdint>

namespace dlaa_core::ngx::remote {

constexpr uint32_t kMagic = 0x4E524C44;  // "DLRN"
constexpr uint32_t kVersion = 1;
constexpr int kMaxFeatures = 4;
constexpr wchar_t kHostExe[] = L"dlaa_ngx_host.exe";

enum Role : uint32_t { kColor, kOutput, kDepth, kMotion, kBias, kRoleCount };

enum Command : uint32_t {
  kNone,
  kInit,         // adapter, project_id, fences -> result 1/0
  kCreate,       // feature + desc -> NVSDK_NGX_Result-ish 1/0, recreated
  kOpenTexture,  // feature, role, texture (0 = drop the role)
  kEvaluate,     // feature + eval params -> NVSDK_NGX_Result
  kRelease,      // feature
  kShutdown,     // the host releases everything and exits
};

struct Shared {
  uint32_t magic;
  uint32_t version;
  uint32_t command;
  uint32_t feature;

  // kInit
  uint32_t adapter_luid_low;
  int32_t adapter_luid_high;
  uint64_t fence_to_host;  // handle values valid in the host process
  uint64_t fence_to_game;
  char project_id[64];

  // kCreate
  uint32_t width, height;
  int32_t preset;
  uint32_t hdr, mv_low_res, auto_exposure, depth_inverted;

  // kOpenTexture
  uint32_t role;
  uint64_t texture;

  // kEvaluate
  float jitter_x, jitter_y;
  float mv_scale_x, mv_scale_y;
  uint32_t subrect_width, subrect_height;
  uint32_t reset;
  uint32_t has_bias;
  uint64_t wait_value;    // on fence_to_host before the evaluate
  uint64_t signal_value;  // on fence_to_game after it, also on failure

  // Reply
  int32_t result;
  uint32_t recreated;
};

}  // namespace dlaa_core::ngx::remote
