#pragma once
// Settings the core itself uses, plus helpers for reading the game's ini.
// Each game keeps its own Config struct; its LoadConfig fills CoreConfig too.

#include <string>

namespace dlaa_core {

struct CoreConfig {
  bool enabled = true;        // [General] Enabled: 0 = pure pass-through proxy, no hooks
  bool debug_tools = false;   // [Debug] Enabled: gates the core debug keys and DumpFrameAt
  int dump_frame_key = 0x79;  // [Debug] DumpFrameKey: VK_F10, pass list of the next frame
  int dump_frame_at = 0;      // [Debug] DumpFrameAt: >0 = dump after this many Presents
  bool dump_shaders = false;  // [Debug] DumpShaders: bytecode to <prefix>_shaders\ once each
  int capture_frames = 8;     // [Debug] CaptureFrames: frames per capture::Request
  // Texture LOD bias while DLAA runs (mip_bias.h); 0 = off, and the sampler
  // hooks are not installed at all.
  float mip_bias = 0.0f;
};

const CoreConfig& GetCoreConfig();
CoreConfig& MutableCoreConfig();  // for the game's LoadConfig only

// Reads <ModuleDir><Prefix>.ini. Everything is read as text: GetPrivateProfileInt
// turns negative numbers into 0.
class Ini {
 public:
  Ini();  // the mod's own ini
  explicit Ini(std::wstring path) : path_(std::move(path)) {}
  const std::wstring& Path() const { return path_; }

  std::wstring String(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) const;
  int Int(const wchar_t* section, const wchar_t* key, int fallback) const;  // hex (0x..) or decimal
  bool Bool(const wchar_t* section, const wchar_t* key, bool fallback) const;
  float Float(const wchar_t* section, const wchar_t* key, float fallback, float lo,
              float hi) const;  // clamped; not finite = fallback
  // One letter A..Z = preset 1..26 (J = 10), anything else = 0 (NGX default).
  int Preset(const wchar_t* section, const wchar_t* key) const;

  // [General] Enabled and the [Debug] keys every mod has.
  void ReadCore(CoreConfig* c) const;

 private:
  std::wstring path_;
};

}  // namespace dlaa_core
