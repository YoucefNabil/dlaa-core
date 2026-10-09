#include "dlaa_core/config.h"

#include <windows.h>

#include <cmath>
#include <cwctype>

#include "dlaa_core/log.h"

namespace dlaa_core {
namespace {
CoreConfig g_core;
}

const CoreConfig& GetCoreConfig() { return g_core; }
CoreConfig& MutableCoreConfig() { return g_core; }

Ini::Ini() : path_(ModuleDir() + Prefix() + L".ini") {}

std::wstring Ini::String(const wchar_t* section, const wchar_t* key,
                         const wchar_t* fallback) const {
  wchar_t v[512] = {};
  GetPrivateProfileStringW(section, key, fallback, v, 512, path_.c_str());
  return v;
}

int Ini::Int(const wchar_t* section, const wchar_t* key, int fallback) const {
  const std::wstring v = String(section, key, L"");
  if (v.empty()) return fallback;
  wchar_t* end = nullptr;
  const long n = wcstol(v.c_str(), &end, 0);
  return end == v.c_str() ? fallback : static_cast<int>(n);
}

bool Ini::Bool(const wchar_t* section, const wchar_t* key, bool fallback) const {
  return Int(section, key, fallback ? 1 : 0) != 0;
}

float Ini::Float(const wchar_t* section, const wchar_t* key, float fallback, float lo,
                 float hi) const {
  const std::wstring v = String(section, key, L"");
  if (v.empty()) return fallback;
  const float f = static_cast<float>(_wtof(v.c_str()));
  return std::isfinite(f) ? std::fmin(hi, std::fmax(lo, f)) : fallback;
}

int Ini::Preset(const wchar_t* section, const wchar_t* key) const {
  const std::wstring v = String(section, key, L"Default");
  if (v.size() != 1) return 0;
  const wchar_t c = static_cast<wchar_t>(std::towupper(v[0]));
  return c >= L'A' && c <= L'Z' ? c - L'A' + 1 : 0;
}

void Ini::ReadCore(CoreConfig* c) const {
  c->enabled = Bool(L"General", L"Enabled", true);
  c->debug_tools = Bool(L"Debug", L"Enabled", false);
  c->dump_frame_key = Int(L"Debug", L"DumpFrameKey", 0x79);
  c->dump_frame_at = Int(L"Debug", L"DumpFrameAt", 0);
  c->dump_shaders = c->debug_tools && Bool(L"Debug", L"DumpShaders", false);
  const int frames = Int(L"Debug", L"CaptureFrames", 8);
  c->capture_frames = frames < 1 ? 1 : frames > 64 ? 64 : frames;
}

}  // namespace dlaa_core
