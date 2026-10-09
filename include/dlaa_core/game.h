#pragma once
// What each game mod provides to the core at link time.

namespace dlaa_core::game {

// File-name prefix, e.g. L"NIER_DLAA": NIER_DLAA.log, NIER_DLAA.ini, ...
const wchar_t* Prefix();

// DllMain time (loader lock held: no threads, no LoadLibrary of other mods).
// Load the config (fill MutableCoreConfig), hooks::SetCallbacks, AddHotkey,
// and any process tweaks. Return false to run as a pure pass-through proxy.
bool OnAttach();

}  // namespace dlaa_core::game
