// Example game for dlaa-core: reads EXAMPLE_DLAA.ini, plugs into the core
// hooks and logs what the game does each frame, which is the first thing to
// look at in a new game (where the scene is drawn, which shaders, which
// constant buffers). The DLAA pass itself is game specific: see NIER /
// sunsetoverdrive / batman arkham knight src/dlss/dlaa.cpp for the three
// patterns (scene camera from a cbuffer, jitter from a game hook, shader patch)
// and dlaa_core/ngx.h, compute_state.h, d3d_util.h + math.h for the parts they share.
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "dlaa_core/config.h"
#include "dlaa_core/game.h"
#include "dlaa_core/hooks.h"
#include "dlaa_core/log.h"

using dlaa_core::Log;
namespace hooks = dlaa_core::hooks;

namespace example {
namespace {

struct Config {
  bool dlaa = true;
  int toggle_key = VK_F8;
  int stats_every = 0;  // [Debug] StatsEvery: log draw / dispatch counts every N frames
};
Config g_cfg;

bool g_on = true;
std::atomic<uint32_t> g_draws{0}, g_dispatches{0}, g_shaders{0}, g_maps{0};

void LoadConfig() {
  const dlaa_core::Ini ini;
  auto& core = dlaa_core::MutableCoreConfig();
  ini.ReadCore(&core);
  g_cfg.dlaa = ini.Bool(L"DLAA", L"Enabled", true);
  g_cfg.toggle_key = ini.Int(L"DLAA", L"ToggleKey", VK_F8);
  g_cfg.stats_every = core.debug_tools ? ini.Int(L"Debug", L"StatsEvery", 0) : 0;
  core.mip_bias = g_cfg.dlaa ? ini.Float(L"DLAA", L"MipBias", 0.0f, -2.0f, 0.0f) : 0.0f;
  g_on = g_cfg.dlaa;
}

void OnCreateShader(ID3D11Device*, hooks::Stage, ID3D11DeviceChild*, uint64_t, const void*,
                    size_t) {
  ++g_shaders;
}

uint32_t BeforeDraw(ID3D11DeviceContext*, const hooks::DrawArgs&) {
  ++g_draws;
  return hooks::kProceed;
}

uint32_t BeforeDispatch(ID3D11DeviceContext*, UINT, UINT, UINT) {
  ++g_dispatches;
  return hooks::kProceed;
}

void OnMap(ID3D11DeviceContext*, ID3D11Resource*, D3D11_MAP, void*) { ++g_maps; }

void OnPresent(IDXGISwapChain*, const DXGI_SWAP_CHAIN_DESC& desc) {
  const uint64_t n = hooks::PresentCount();
  if (g_cfg.stats_every > 0 && n % g_cfg.stats_every == 0) {
    Log("frame %llu %ux%u: %u draws, %u dispatches, %u maps, %u shaders created so far",
        static_cast<unsigned long long>(n), desc.BufferDesc.Width, desc.BufferDesc.Height,
        g_draws.load(), g_dispatches.load(), g_maps.load(), g_shaders.load());
  }
  g_draws = g_dispatches = g_maps = 0;
  // A game's DLAA usually runs earlier, where the scene colour is finished
  // (before_draw of the first post-processing pass); Present is the fallback
  // for games without HDR scene access.
}

void Toggle() {
  g_on = !g_on;
  Log("DLAA %s", g_on ? "on" : "off");
}

}  // namespace
}  // namespace example

namespace dlaa_core::game {

const wchar_t* Prefix() { return L"EXAMPLE_DLAA"; }

bool OnAttach() {
  example::LoadConfig();
  hooks::Callbacks cb;
  cb.on_create_shader = &example::OnCreateShader;
  cb.before_draw = &example::BeforeDraw;
  cb.before_dispatch = &example::BeforeDispatch;
  cb.on_map = &example::OnMap;
  cb.on_present = &example::OnPresent;
  hooks::SetCallbacks(cb);
  hooks::AddHotkey(example::g_cfg.toggle_key, false, &example::Toggle);
  return true;
}

}  // namespace dlaa_core::game
