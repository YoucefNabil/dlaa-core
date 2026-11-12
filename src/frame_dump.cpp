#include "dlaa_core/frame_dump.h"

#include <windows.h>
#include <d3d11_1.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dlaa_core/d3d_util.h"
#include "dlaa_core/log.h"
#include "dlaa_core/shader_registry.h"

namespace dlaa_core::frame_dump {

std::atomic<bool> g_active{false};

namespace {

struct Entry {
  std::string key;  // everything except the call count, used to collapse repeats
  std::string text;
  uint32_t repeats = 1;
  uint64_t total_count = 0;
};

// Idle -> (request) -> Pre: one frame recording deferred contexts only, so
// command lists built a frame ahead are known -> Main: the dumped frame,
// immediate + deferred contexts -> file written, back to Idle.
enum class Phase { Idle, Pre, Main };

std::atomic<bool> g_requested{false};
std::mutex g_mutex;
Phase g_phase = Phase::Idle;
std::vector<Entry> g_entries;  // immediate context, Main frame only
std::unordered_map<ID3D11DeviceContext*, std::vector<Entry>> g_ctx_entries;  // open recordings
std::unordered_map<ID3D11CommandList*, std::vector<Entry>> g_list_entries;   // finished lists
std::unordered_map<ID3D11CommandList*, int> g_list_ids;
// Capture frame (-1 = Pre, 0 = the dumped frame) in which each deferred
// recording started and finished, to see how far ahead lists are built.
int g_frame = -1;
std::unordered_map<ID3D11DeviceContext*, int> g_ctx_start;
std::unordered_map<ID3D11CommandList*, std::pair<int, int>> g_list_frames;
std::unordered_map<ID3D11Resource*, int> g_resource_ids;
std::vector<std::string> g_resource_descs;
uint32_t g_execute_command_lists = 0;
uint32_t g_unrecorded_lists = 0;
struct PendingMap {
  const uint8_t* data;
  UINT bytes;
  D3D11_MAP type;
};
std::map<std::pair<ID3D11DeviceContext*, ID3D11Resource*>, PendingMap> g_pending_maps;

// Bytes of every constant-buffer upload of the dumped frame, written to the .cb
// file: per record, uint32 record id, resource id, byte offset, byte count, data.
std::vector<uint8_t> g_cb_blob;
uint32_t g_cb_records = 0;
// Game section appended to the text file (SetAppendix).
void (*g_appendix_start)() = nullptr;
std::string (*g_appendix_text)() = nullptr;

const char* FormatName(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "RGBA16_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16_UNORM";
    case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32S8";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "RGB10A2_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
    case DXGI_FORMAT_R16G16_TYPELESS: return "RG16_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
    case DXGI_FORMAT_R16G16_SNORM: return "RG16_SNORM";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT: return "D32F";
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case DXGI_FORMAT_R8G8_UNORM: return "RG8";
    case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT: return "R16F";
    case DXGI_FORMAT_D16_UNORM: return "D16";
    case DXGI_FORMAT_R8_UNORM: return "R8";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "BGRA8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
    case DXGI_FORMAT_BC1_UNORM: return "BC1";
    case DXGI_FORMAT_BC3_UNORM: return "BC3";
    case DXGI_FORMAT_BC7_UNORM: return "BC7";
    default: return nullptr;
  }
}

std::string DescribeNew(ID3D11Resource* r) {
  char buf[160];
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D11_TEXTURE2D_DESC d;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    const char* name = FormatName(d.Format);
    char fmt[24];
    if (!name) std::snprintf(fmt, sizeof(fmt), "fmt%d", d.Format), name = fmt;
    std::snprintf(buf, sizeof(buf), "tex2d %ux%u %s mips=%u array=%u msaa=%u bind=0x%X", d.Width,
                  d.Height, name, d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.BindFlags);
  } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    D3D11_BUFFER_DESC d;
    static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
    std::snprintf(buf, sizeof(buf), "buffer %u bytes bind=0x%X", d.ByteWidth, d.BindFlags);
  } else {
    std::snprintf(buf, sizeof(buf), "resource dim=%d", dim);
  }
  return buf;
}

// Short per-dump id ("T12") for a resource; full descriptions go in a table
// at the end of the file.
std::string Id(ID3D11Resource* r) {
  if (!r) return "-";
  auto [it, inserted] = g_resource_ids.try_emplace(r, static_cast<int>(g_resource_ids.size()));
  if (inserted) g_resource_descs.push_back(DescribeNew(r));
  return "T" + std::to_string(it->second);
}

std::string ViewId(ID3D11View* v) {
  if (!v) return "-";
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  std::string id = Id(r);
  if (r) r->Release();
  // Views of a lower mip: note the mip.
  ID3D11ShaderResourceView* srv = nullptr;
  if (SUCCEEDED(v->QueryInterface(IID_PPV_ARGS(&srv)))) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d;
    srv->GetDesc(&d);
    srv->Release();
    if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && d.Texture2D.MostDetailedMip)
      id += "m" + std::to_string(d.Texture2D.MostDetailedMip);
  }
  return id;
}

template <typename View>
std::string ViewList(View* const* views, UINT n) {
  UINT last = 0;
  for (UINT i = 0; i < n; ++i)
    if (views[i]) last = i + 1;
  std::string s = "[";
  for (UINT i = 0; i < last; ++i) {
    if (i) s += ',';
    s += ViewId(views[i]);
  }
  return s + "]";
}

template <typename T>
void ReleaseAll(T* const* items, UINT n) {
  for (UINT i = 0; i < n; ++i)
    if (items[i]) items[i]->Release();
}

std::string Hex(uint64_t v) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
  return buf;
}

// Where a call on ctx is recorded, or nullptr if it isn't recorded right now
// (the immediate context is only recorded during the Main frame).
std::vector<Entry>* Target(ID3D11DeviceContext* ctx) {
  if (g_phase == Phase::Idle) return nullptr;
  if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
    g_ctx_start.try_emplace(ctx, g_frame);
    return &g_ctx_entries[ctx];
  }
  return g_phase == Phase::Main ? &g_entries : nullptr;
}

void Push(std::vector<Entry>& out, std::string key, const char* op, uint64_t count) {
  if (!out.empty() && out.back().key == key) {
    out.back().repeats++;
    out.back().total_count += count;
    return;
  }
  Entry e;
  e.text = std::string(op) + "  " + key;
  e.key = std::move(key);
  e.total_count = count;
  out.push_back(std::move(e));
}

// Constant buffers bound to one stage, "b<slot>:<id>" for each bound slot,
// plus "+<bytes>" when bound at an offset (D3D11.1 *SetConstantBuffers1).
enum class Stage { VS, PS, CS };
std::string CbList(ID3D11DeviceContext* ctx, Stage stage) {
  constexpr UINT n = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
  ID3D11Buffer* b[n] = {};
  UINT first[n] = {}, num[n] = {};
  ID3D11DeviceContext1* ctx1 = nullptr;
  if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
    if (stage == Stage::VS) ctx1->VSGetConstantBuffers1(0, n, b, first, num);
    else if (stage == Stage::PS) ctx1->PSGetConstantBuffers1(0, n, b, first, num);
    else ctx1->CSGetConstantBuffers1(0, n, b, first, num);
    ctx1->Release();
  } else if (stage == Stage::VS) {
    ctx->VSGetConstantBuffers(0, n, b);
  } else if (stage == Stage::PS) {
    ctx->PSGetConstantBuffers(0, n, b);
  } else {
    ctx->CSGetConstantBuffers(0, n, b);
  }
  std::string s = "[";
  for (UINT i = 0; i < n; ++i) {
    if (!b[i]) continue;
    if (s.size() > 1) s += ',';
    s += "b" + std::to_string(i) + ":" + Id(b[i]);
    if (first[i]) s += "+" + std::to_string(first[i] * 16);
    b[i]->Release();
  }
  return s + "]";
}

// Appends one upload to the .cb blob; returns its record id.
uint32_t AddCbRecord(int resource_id, UINT offset, const void* data, UINT bytes) {
  const uint32_t header[4] = {g_cb_records, static_cast<uint32_t>(resource_id), offset, bytes};
  const auto* h = reinterpret_cast<const uint8_t*>(header);
  g_cb_blob.insert(g_cb_blob.end(), h, h + sizeof(header));
  const auto* d = static_cast<const uint8_t*>(data);
  g_cb_blob.insert(g_cb_blob.end(), d, d + bytes);
  return g_cb_records++;
}

std::string ListId(ID3D11CommandList* list) {
  auto [it, inserted] = g_list_ids.try_emplace(list, static_cast<int>(g_list_ids.size()));
  return "L" + std::to_string(it->second);
}

void WriteFile() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf_s(name, L"_frame_%04u%02u%02u_%02u%02u%02u_%03u.txt", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  const std::wstring path = ModuleDir() + Prefix() + name;
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) {
    Log("frame dump: could not write %ls", path.c_str());
    return;
  }
  std::fprintf(f, "# One frame of GPU work, in submission order. Deferred command lists are\n");
  std::fprintf(f, "# spliced in (lines starting with '|') where the immediate context executes them.\n");
  std::fprintf(f, "# Consecutive identical calls are collapsed as 'xN'. Resource ids are listed at the end.\n\n");
  for (size_t i = 0; i < g_entries.size(); ++i) {
    const Entry& e = g_entries[i];
    std::fprintf(f, "#%04zu x%-4u n=%-8llu %s\n", i, e.repeats,
                 static_cast<unsigned long long>(e.total_count), e.text.c_str());
  }
  std::fprintf(f, "\n# Resources\n");
  for (size_t i = 0; i < g_resource_descs.size(); ++i)
    std::fprintf(f, "T%zu  %s\n", i, g_resource_descs[i].c_str());
  std::fprintf(f, "\n# ExecuteCommandList calls: %u (%u with a list not recorded during capture)\n",
               g_execute_command_lists, g_unrecorded_lists);
  if (g_appendix_text) std::fprintf(f, "\n%s", g_appendix_text().c_str());
  std::fclose(f);

  std::wstring cb_path = path;
  cb_path.replace(cb_path.size() - 4, 4, L".cb");
  FILE* cb = nullptr;
  if (_wfopen_s(&cb, cb_path.c_str(), L"wb") == 0 && cb) {
    std::fwrite(g_cb_blob.data(), 1, g_cb_blob.size(), cb);
    std::fclose(cb);
  }
  Log("frame dump written: %ls (%zu entries, %zu resources, %u lists, %u unrecorded, %u constant "
      "buffer uploads)",
      path.c_str(), g_entries.size(), g_resource_descs.size(), g_execute_command_lists,
      g_unrecorded_lists, g_cb_records);
}

}  // namespace

void Request() { g_requested = true; }

void SetAppendix(void (*on_start)(), std::string (*text)()) {
  g_appendix_start = on_start;
  g_appendix_text = text;
}

void OnPresent() {
  std::lock_guard lock(g_mutex);
  switch (g_phase) {
    case Phase::Main:
      WriteFile();
      g_phase = Phase::Idle;
      g_active = false;
      g_entries = {};
      g_ctx_entries = {};
      g_list_entries = {};
      g_list_ids = {};
      g_resource_ids = {};
      g_resource_descs = {};
      g_pending_maps = {};
      g_ctx_start = {};
      g_list_frames = {};
      g_cb_blob = {};
      g_cb_records = 0;
      return;
    case Phase::Pre:
      g_phase = Phase::Main;
      g_frame = 0;
      return;
    case Phase::Idle:
      if (!g_requested.exchange(false)) return;
      g_execute_command_lists = 0;
      g_unrecorded_lists = 0;
      g_phase = Phase::Pre;
      g_frame = -1;
      if (g_appendix_start) g_appendix_start();
      g_active = true;
      Log("frame dump: capturing (deferred lists this frame, full frame next)");
      return;
  }
}

void RecordDraw(ID3D11DeviceContext* ctx, const char* op, UINT count, UINT instances) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;

  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  ctx->PSGetShader(&ps, nullptr, nullptr);
  ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
  ID3D11ShaderResourceView* srvs[16] = {};
  ctx->PSGetShaderResources(0, 16, srvs);
  ID3D11ShaderResourceView* vs_srvs[8] = {};
  ctx->VSGetShaderResources(0, 8, vs_srvs);
  ID3D11BlendState* blend = nullptr;
  FLOAT blend_factor[4];
  UINT sample_mask = 0;
  ctx->OMGetBlendState(&blend, blend_factor, &sample_mask);
  ID3D11DepthStencilState* depth_state = nullptr;
  UINT stencil_ref = 0;
  ctx->OMGetDepthStencilState(&depth_state, &stencil_ref);
  D3D11_VIEWPORT vp = {};
  UINT num_vp = 1;
  ctx->RSGetViewports(&num_vp, &vp);

  char vp_text[64];
  std::snprintf(vp_text, sizeof(vp_text), "%.0fx%.0f", vp.Width, vp.Height);
  if (vp.TopLeftX != 0.0f || vp.TopLeftY != 0.0f)
    std::snprintf(vp_text + std::strlen(vp_text), 32, "@%.0f,%.0f", vp.TopLeftX, vp.TopLeftY);
  char state_text[96] = "";
  if (blend) {
    D3D11_BLEND_DESC bd;
    blend->GetDesc(&bd);
    const auto& rt0 = bd.RenderTarget[0];
    if (rt0.BlendEnable)
      std::snprintf(state_text, sizeof(state_text), " blend=%d/%d/%d", rt0.SrcBlend, rt0.DestBlend,
                    rt0.BlendOp);
  }
  if (depth_state) {
    D3D11_DEPTH_STENCIL_DESC dd;
    depth_state->GetDesc(&dd);
    const size_t len = std::strlen(state_text);
    std::snprintf(state_text + len, sizeof(state_text) - len, " z=%d%s%d st=%d", dd.DepthEnable,
                  dd.DepthWriteMask ? "w" : "", dd.DepthFunc, dd.StencilEnable);
  }
  std::string key = "VS=" + Hex(shaders::Lookup(vs)) + " PS=" + Hex(shaders::Lookup(ps)) +
                    " RT=" + ViewList(rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) +
                    " DS=" + ViewId(dsv) + " VP=" + vp_text + " PS.SRV=" + ViewList(srvs, 16) +
                    " VS.SRV=" + ViewList(vs_srvs, 8) + state_text + " VS.CB=" + CbList(ctx, Stage::VS) + " PS.CB=" + CbList(ctx, Stage::PS);
  if (instances > 1) key += " inst=" + std::to_string(instances);
  Push(*out, std::move(key), op, count);

  if (vs) vs->Release();
  if (ps) ps->Release();
  if (dsv) dsv->Release();
  if (blend) blend->Release();
  if (depth_state) depth_state->Release();
  ReleaseAll(rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
  ReleaseAll(srvs, 16);
  ReleaseAll(vs_srvs, 8);
}

void RecordDispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;

  ID3D11ComputeShader* cs = nullptr;
  ctx->CSGetShader(&cs, nullptr, nullptr);
  ID3D11UnorderedAccessView* uavs[8] = {};
  ctx->CSGetUnorderedAccessViews(0, 8, uavs);
  ID3D11ShaderResourceView* srvs[16] = {};
  ctx->CSGetShaderResources(0, 16, srvs);

  char groups[48];
  std::snprintf(groups, sizeof(groups), " groups=%ux%ux%u", x, y, z);
  std::string key = "CS=" + Hex(shaders::Lookup(cs)) + groups + " UAV=" + ViewList(uavs, 8) +
                    " CS.SRV=" + ViewList(srvs, 16) + " CS.CB=" + CbList(ctx, Stage::CS);
  Push(*out, std::move(key), "Dispatch", 1);

  if (cs) cs->Release();
  ReleaseAll(uavs, 8);
  ReleaseAll(srvs, 16);
}

void RecordCopy(ID3D11DeviceContext* ctx, const char* op, ID3D11Resource* dst, ID3D11Resource* src) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  Push(*out, "dst=" + Id(dst) + " src=" + Id(src), op, 1);
}

// value: the clear colour, or for ClearDSV {depth, stencil, flags, 0}.
void RecordClear(ID3D11DeviceContext* ctx, const char* op, ID3D11View* view,
                 const float value[4]) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  char v[96] = "";
  if (value)
    std::snprintf(v, sizeof(v), " value=[%g %g %g %g]", value[0], value[1], value[2], value[3]);
  Push(*out, "view=" + ViewId(view) + v, op, 1);
}

void RecordExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  const bool immediate = out == &g_entries;
  if (immediate) ++g_execute_command_lists;
  const std::string id = ListId(list);
  const auto it = g_list_entries.find(list);
  if (it == g_list_entries.end()) {
    if (immediate) ++g_unrecorded_lists;
    Push(*out, id + " (not recorded during capture)", "ExecuteCommandList", 1);
    return;
  }
  // Header and spliced entries get unique keys so they never collapse with
  // neighbouring calls.
  std::string header = id + " (" + std::to_string(it->second.size()) + " entries";
  if (const auto f = g_list_frames.find(list); f != g_list_frames.end())
    header += ", recorded frame " + std::to_string(f->second.first) + ".." +
              std::to_string(f->second.second);
  header += ")";
  Push(*out, header + "#" + std::to_string(out->size()), "ExecuteCommandList", 1);
  out->back().text = "ExecuteCommandList  " + header;
  for (const Entry& e : it->second) {
    Entry copy = e;
    copy.key = "|" + e.key + "#" + std::to_string(out->size());
    copy.text = "| " + e.text;
    out->push_back(std::move(copy));
  }
}

void RecordFinishCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list) {
  std::lock_guard lock(g_mutex);
  if (g_phase == Phase::Idle) return;
  auto node = g_ctx_entries.extract(ctx);
  g_list_entries[list] = node.empty() ? std::vector<Entry>{} : std::move(node.mapped());
  const auto start = g_ctx_start.find(ctx);
  g_list_frames[list] = {start != g_ctx_start.end() ? start->second : g_frame, g_frame};
  if (start != g_ctx_start.end()) g_ctx_start.erase(start);
}

void RecordMap(ID3D11DeviceContext* ctx, ID3D11Resource* r, D3D11_MAP type, void* data) {
  const UINT bytes = ConstantBufferBytes(r);
  if (!bytes || !data) return;
  std::lock_guard lock(g_mutex);
  if (!Target(ctx)) return;
  g_pending_maps[{ctx, r}] = {static_cast<const uint8_t*>(data), bytes, type};
}

void RecordUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* r) {
  std::lock_guard lock(g_mutex);
  const auto it = g_pending_maps.find({ctx, r});
  if (it == g_pending_maps.end()) return;
  const PendingMap m = it->second;
  g_pending_maps.erase(it);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  // The game has finished writing; the contents are what the GPU will see.
  const std::string id = Id(r);
  const uint32_t rec = AddCbRecord(std::stoi(id.substr(1)), 0, m.data, m.bytes);
  Push(*out, "dst=" + id + " bytes=" + std::to_string(m.bytes) + " type=" +
                 std::to_string(m.type) + " cb#" + std::to_string(rec),
       "Map/Unmap CB", 1);
}

void RecordUpdate(ID3D11DeviceContext* ctx, const char* op, ID3D11Resource* r,
                  const D3D11_BOX* box, const void* data) {
  const UINT bytes = ConstantBufferBytes(r);
  if (!bytes || !data) return;
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  const UINT offset = box ? box->left : 0;
  const UINT size = box ? box->right - box->left : bytes;
  const std::string id = Id(r);
  const uint32_t rec = AddCbRecord(std::stoi(id.substr(1)), offset, data, size);
  Push(*out, "dst=" + id + " offset=" + std::to_string(offset) + " bytes=" +
                 std::to_string(size) + " cb#" + std::to_string(rec),
       op, 1);
}

}  // namespace dlaa_core::frame_dump
