#pragma once
#include <string>

namespace dlaa_core {

// Directory containing our d3d11.dll (the game folder), with trailing backslash.
const std::wstring& ModuleDir();
void SetSelfModule(void* hmodule);

// Name every file the mod writes starts with ("NIER_DLAA": NIER_DLAA.log,
// NIER_DLAA_shaders\, NIER_DLAA_frame_*.txt ...). Set before LogInit.
void SetPrefix(const wchar_t* prefix);
const std::wstring& Prefix();

void LogInit();  // opens <ModuleDir><Prefix>.log
void Log(const char* fmt, ...);

}  // namespace dlaa_core
