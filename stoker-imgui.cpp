// STOKER's full GUI: the same data core as the terminal app (bpos-dash.cpp is
// included below with the FTXUI main compiled out), presented with Dear ImGui
// in a native window. Hot Neon theme, pixel-art column icons, sortable table,
// gauges with the days and haul values drawn inside the bar.
#define STOKER_IMGUI
#include "bpos-dash.cpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "font_dejavu_mono.hpp"
#include "icons_data.hpp"
#include "struct_icons_data.hpp"
#include "window_icon_data.hpp"
#include "jumpmap_data.hpp"
#include "universe_pos.hpp"
#include <regex>
#include <set>

// --- the map (same layout as eveterm: DOTLAN region SVG positions + the
// embedded New Eden gate graph) -------------------------------------------------
struct MapNode { int id = 0; std::string label; double nx = 0, ny = 0; };
struct MapView {
    std::string region, error;
    bool loading = false, ok = false;
    std::vector<MapNode> nodes;
    std::vector<std::pair<int, int>> gates;
    float zoom = 1.0f;
    ImVec2 pan{0.5f, 0.5f};  // normalized center
    int selected = -1;
};
static MapView g_map;               // guarded by g_mtx for the built data
static std::map<int, std::vector<int>> g_adj;
static std::map<int, std::string> g_sysname;
static std::map<std::string, int> g_sysid;
struct SysPos { double nx = 0, ny = 0; int region = 0; };
static std::map<int, SysPos> g_syspos;  // whole-universe layout (eveterm's coords)
static std::vector<std::pair<int, int>> g_jbridges;  // friendly Ansiblex network
struct RegionLabel { std::string name; double nx = 0, ny = 0; };
static std::vector<RegionLabel> g_region_labels;

static void load_universe() {
    if (!g_adj.empty()) return;
    try {
        json j = json::parse(std::string((const char*)kJumpmapJson, kJumpmapSize));
        for (auto& [k, v] : j["names"].items()) {
            int id = std::atoi(k.c_str());
            std::string nm = v.get<std::string>();
            g_sysname[id] = nm;
            for (auto& c : nm) c = (char)tolower((unsigned char)c);
            g_sysid[nm] = id;
        }
        for (auto& [k, v] : j["adj"].items()) {
            int id = std::atoi(k.c_str());
            for (auto& n : v) g_adj[id].push_back(n.get<int>());
        }
    } catch (...) {}
    try {
        json p = json::parse(std::string(kUniversePosJson, kUniversePosSize));
        for (auto& [k, v] : p.items())
            g_syspos[std::atoi(k.c_str())] = {v[0].get<double>(), v[1].get<double>(),
                                              v[2].get<int>()};
    } catch (...) {}
    try {
        json b = json::parse(std::string(kJumpBridgesJson, kJumpBridgesSize));
        for (auto& e : b) g_jbridges.push_back({e[0].get<int>(), e[1].get<int>()});
    } catch (...) {}
    try {
        json r = json::parse(std::string(kRegionLabelsJson, kRegionLabelsSize));
        for (auto& e : r)
            g_region_labels.push_back(
                {e.value("name", ""), e.value("x", 0.0), e.value("y", 0.0)});
    } catch (...) {}
}

// the whole-universe map from the embedded SDE layout; gateless (unreachable)
// systems are skipped so wormhole space does not litter the view
static void act_build_universe() {
    {
        std::lock_guard<std::mutex> l(g_mtx);
        if (g_map.loading) return;
        g_map.loading = true;
        g_map.error.clear();
    }
    spawn_bg([]() {
        load_universe();
        MapView m;
        m.region = "New Eden";
        std::map<int, int> idx;
        for (auto& kv : g_syspos) {
            if (!g_adj.count(kv.first)) continue;
            idx[kv.first] = (int)m.nodes.size();
            m.nodes.push_back({kv.first,
                               g_sysname.count(kv.first) ? g_sysname[kv.first]
                                                         : std::to_string(kv.first),
                               kv.second.nx, kv.second.ny});
        }
        for (auto& kv : idx)
            for (int nb : g_adj[kv.first])
                if (kv.first < nb && idx.count(nb))
                    m.gates.push_back({kv.second, idx.at(nb)});
        m.ok = !m.nodes.empty();
        if (!m.ok) m.error = "no universe layout embedded in this build";
        std::lock_guard<std::mutex> l(g_mtx);
        m.loading = false;
        g_map = std::move(m);
        if (g_gui_wake) g_gui_wake();
    });
}

// centre the universe view on a system, zoomed so its region fills the view
static void map_center_on_system(MapView& live, int sysid) {
    auto it = g_syspos.find(sysid);
    if (it == g_syspos.end()) return;
    double x0 = 1, x1 = 0, y0 = 1, y1 = 0;
    for (auto& kv : g_syspos)
        if (kv.second.region == it->second.region) {
            x0 = std::min(x0, kv.second.nx);
            x1 = std::max(x1, kv.second.nx);
            y0 = std::min(y0, kv.second.ny);
            y1 = std::max(y1, kv.second.ny);
        }
    double span = std::max(x1 - x0, y1 - y0);
    live.pan = ImVec2((float)((x0 + x1) * 0.5), (float)((y0 + y1) * 0.5));
    live.zoom = std::clamp((float)(0.85 / (span > 0.001 ? span : 0.001)), 1.0f, 80.0f);
}

#include "eve_logs.hpp"

static void act_build_map(std::string region) {
    {
        std::lock_guard<std::mutex> l(g_mtx);
        if (g_map.loading) return;
        g_map.loading = true;
        g_map.error.clear();
    }
    spawn_bg([region]() {
        load_universe();
        std::string slug = region;
        for (auto& c : slug)
            if (c == ' ') c = '_';
        std::string svg = standalone::http_get_body("https://evemaps.dotlan.net/svg/" + slug + ".svg");
        MapView m;
        m.region = region;
        static const std::regex re(R"RE(id="sys([0-9]+)"\s+x="([0-9.]+)"\s+y="([0-9.]+)")RE");
        std::map<int, int> idx;
        double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
        for (auto it = std::sregex_iterator(svg.begin(), svg.end(), re);
             it != std::sregex_iterator(); ++it) {
            int id = std::stoi((*it)[1]);
            if (idx.count(id)) continue;
            double x = std::stod((*it)[2]) + 31.0, y = std::stod((*it)[3]) + 15.0;
            idx[id] = (int)m.nodes.size();
            m.nodes.push_back({id, g_sysname.count(id) ? g_sysname[id] : std::to_string(id), x, y});
            minx = std::min(minx, x); maxx = std::max(maxx, x);
            miny = std::min(miny, y); maxy = std::max(maxy, y);
        }
        if (m.nodes.empty()) {
            m.error = "no systems parsed for '" + region + "' (check the region name)";
        } else {
            double sx = maxx > minx ? maxx - minx : 1, sy = maxy > miny ? maxy - miny : 1;
            for (auto& n : m.nodes) { n.nx = (n.nx - minx) / sx; n.ny = (n.ny - miny) / sy; }
            for (auto& kv : idx)
                for (int nb : g_adj.count(kv.first) ? g_adj[kv.first] : std::vector<int>{})
                    if (kv.first < nb && idx.count(nb))
                        m.gates.push_back({kv.second, idx.at(nb)});
            m.ok = true;
        }
        std::lock_guard<std::mutex> l(g_mtx);
        std::string keep_err = m.error;
        m.loading = false;
        g_map = std::move(m);
        if (g_gui_wake) g_gui_wake();
    });
}

// EVE autopilot: set the in-game destination with the first login that has
// the waypoint scope (needs one re-login after v2.1 to grant it).
static void act_set_destination(int system_id, const std::string& sysname) {
    spawn_bg([system_id, sysname]() {
        std::string note = "set destination needs a standalone EVE login";
        if (g_standalone) {
            json chars = standalone::load_characters();
            note = "no login has the waypoint permission - press + add character to re-login";
            for (size_t i = 0; i < chars.size(); i++) {
                std::string tok = standalone::ensure_token_entry(g_client_id, chars, i);
                if (tok.empty() ||
                    !standalone::token_has_scope(tok, "esi-ui.write_waypoint.v1"))
                    continue;
                int st = 0;
                standalone::http_post_auth(
                    "https://esi.evetech.net/latest/ui/autopilot/waypoint/"
                    "?add_to_beginning=false&clear_other_waypoints=true&destination_id=" +
                        std::to_string(system_id) + "&datasource=tranquility",
                    tok, st);
                note = st == 204 ? "destination set: " + sysname + " (" +
                                       chars[i].value("character_name", std::string()) + ")"
                                 : "set destination failed (HTTP " + std::to_string(st) + ")";
                break;
            }
        }
        std::lock_guard<std::mutex> l(g_mtx);
        g_note = note;
        g_note_at = time(nullptr);
        if (g_gui_wake) g_gui_wake();
    });
}

// --- theme -------------------------------------------------------------------
static const ImVec4 PINK(1.00f, 0.17f, 0.84f, 1), CYAN_(0.00f, 0.90f, 1.00f, 1),
    DIMCYAN(0.00f, 0.51f, 0.59f, 1), GREY_(0.50f, 0.53f, 0.59f, 1),
    TEXTC(0.90f, 0.92f, 0.96f, 1);

static ImU32 band_u32(int band, bool secondary = false) {
    switch (band) {
        case 0: return IM_COL32(255, 70, 70, 255);
        case 1: return IM_COL32(255, 140, 0, 255);
        case 2: return IM_COL32(250, 215, 70, 255);
        case 3: return secondary ? IM_COL32(172, 128, 255, 255) : IM_COL32(56, 216, 232, 255);
        default: return IM_COL32(128, 136, 150, 255);
    }
}

static ImU32 mix_u32(ImU32 a, ImU32 b, float t) {
    ImVec4 fa = ImGui::ColorConvertU32ToFloat4(a), fb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(fa.x + (fb.x - fa.x) * t,
                                                 fa.y + (fb.y - fa.y) * t,
                                                 fa.z + (fb.z - fa.z) * t,
                                                 fa.w + (fb.w - fa.w) * t));
}

// gauge colour ramp over the fill fraction, blended steadily across the whole
// range: fuel blocks fade green (full) through yellow to red (empty); the
// secondary fuel (gas/ozone) fades purple (full) to yellow (empty)
static ImU32 ramp_u32(float t, bool secondary) {
    t = t < 0 ? 0.f : t > 1 ? 1.f : t;
    if (secondary)
        return mix_u32(IM_COL32(250, 215, 70, 255), IM_COL32(172, 128, 255, 255), t);
    return t < 0.5f
               ? mix_u32(IM_COL32(255, 70, 70, 255), IM_COL32(250, 215, 70, 255), t * 2)
               : mix_u32(IM_COL32(250, 215, 70, 255), IM_COL32(80, 230, 110, 255),
                         (t - 0.5f) * 2);
}

// --- gl textures for the pixel-art icons --------------------------------------
static ImTextureID make_icon(const unsigned char* rgba, int size = kIconSize,
                             bool smooth = false) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    return (ImTextureID)(intptr_t)tex;
}
static ImTextureID g_ic_fuel, g_ic_gas, g_ic_ozone;
static std::map<std::string, ImTextureID> g_type_icons;  // display type -> portrait

// one half-height meter strip (small text riding inside)
static void mini_bar(ImDrawList* dl, ImVec2 p, float w, float h, double frac,
                     const std::string& left, const std::string& right, bool secondary,
                     ImTextureID icon, bool invert = false) {
    ImU32 col = band_u32(-1);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(30, 32, 44, 255), 2.0f);
    float fillx = p.x;
    if (frac >= 0) {
        float f = frac > 1 ? 1.f : (float)frac;
        fillx = p.x + w * f;
        col = ramp_u32(invert ? 1.0f - f : f, secondary);
        dl->AddRectFilled(p, ImVec2(fillx, p.y + h), col, 2.0f);
    }
    if (frac < 0 && (left == "?" || left == "NA")) {  // unknown/absent: hatch the trough
        dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
        for (float x = p.x - h; x < p.x + w; x += 7)
            dl->AddLine(ImVec2(x, p.y + h), ImVec2(x + h, p.y), IM_COL32(70, 76, 94, 255), 1.5f);
        dl->PopClipRect();
    }
    float tx0 = p.x + 4;
    if (icon) {
        float s = h - 4;
        dl->AddImage(icon, ImVec2(p.x + 3, p.y + 2), ImVec2(p.x + 3 + s, p.y + 2 + s));
        tx0 = p.x + 3 + s + 4;
    }
    ImFont* fnt = ImGui::GetFont();
    float fs = std::min(h - 3.0f, 16.0f);
    // each label drawn twice, clipped at the fill edge: white over the fill,
    // ramp colour over the trough - splits cleanly mid-glyph
    auto put = [&](const std::string& s, bool rightside) {
        if (s.empty()) return;
        ImVec2 ts = fnt->CalcTextSizeA(fs, FLT_MAX, 0, s.c_str());
        float x = rightside ? p.x + w - ts.x - 4 : tx0;
        ImVec2 tp(x, p.y + (h - ts.y) * 0.5f);
        dl->PushClipRect(p, ImVec2(fillx, p.y + h), true);
        dl->AddText(fnt, fs, tp, IM_COL32(12, 12, 18, 255), s.c_str());
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(fillx, p.y), ImVec2(p.x + w, p.y + h), true);
        dl->AddText(fnt, fs, tp, col, s.c_str());
        dl->PopClipRect();
    };
    if (frac < 0) {
        ImVec2 ts = fnt->CalcTextSizeA(fs, FLT_MAX, 0, left.c_str());
        dl->AddText(fnt, fs, ImVec2(p.x + (w - ts.x) / 2, p.y + (h - ts.y) * 0.5f), col,
                    left.c_str());
    } else {
        put(left, false);
        // drop the right label instead of colliding when the bar is narrow
        ImVec2 lts = fnt->CalcTextSizeA(fs, FLT_MAX, 0, left.c_str());
        ImVec2 rts = fnt->CalcTextSizeA(fs, FLT_MAX, 0, right.c_str());
        if (tx0 + lts.x + 10 + rts.x <= p.x + w - 4) put(right, true);
    }
}

// haul labels inside the bars read "[units] to 30d"; placeholders stay bare
static std::string to30(const std::string& s) {
    return (s.empty() || s == "ok" || s == "--" || s == "?") ? s : s + " to 30d";
}

// cell height: one strip per meter (fuel / gas-oz / moongoo) with 2px gaps
static float gauge_cell_h(int meters) {
    float h = ((ImGui::GetTextLineHeight() + 6) * 2 - 2) / 2;
    return meters * h + (meters - 1) * 2;
}

// little warning triangle with an exclamation mark, chart-style
static void warn_tri(ImDrawList* dl, ImVec2 p, float s, ImU32 col, int alpha = 255) {
    ImU32 c = (col & 0xFFFFFF) | ((ImU32)alpha << 24);
    dl->AddTriangleFilled(ImVec2(p.x + s * 0.5f, p.y), ImVec2(p.x, p.y + s),
                          ImVec2(p.x + s, p.y + s), c);
    ImU32 dark = IM_COL32(20, 20, 26, alpha);
    dl->AddLine(ImVec2(p.x + s * 0.5f, p.y + s * 0.34f),
                ImVec2(p.x + s * 0.5f, p.y + s * 0.66f), dark, 1.8f);
    dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.82f), 1.1f, dark);
}

// something on screen is flashing: run the event loop fast enough to animate
static bool g_flash_active = false;

// fully-unanchored hulls auto-drop from the table (destroyed ones leave the
// ESI feed by themselves); this toggle reveals the auto-dropped rows
static bool g_show_hidden = false;

// countdown text: "1d 04:22:11" or "04:22:11"
static std::string fmt_dur(double secs) {
    long s = secs < 0 ? 0 : (long)secs;
    char b[48];
    if (s >= 86400)
        std::snprintf(b, sizeof b, "%ldd %02ld:%02ld:%02ld", s / 86400, (s % 86400) / 3600,
                      (s % 3600) / 60, s % 60);
    else
        std::snprintf(b, sizeof b, "%02ld:%02ld:%02ld", s / 3600, (s % 3600) / 60, s % 60);
    return b;
}

// structure clock: reinforcement / anchoring vulnerability timers come from
// state_timer_end, an unanchor-in-progress from unanchors_at. Counts down
// toward the dangerous moment, so the colour runs yellow -> red. NONE when
// nothing is ticking.
static void timer_info(const Row& r, std::string& txt, ImU32& col) {
    time_t nowt = time(nullptr);
    time_t te = r.state_timer_end.empty() ? 0 : parse_iso(r.state_timer_end);
    if (te <= nowt) {
        time_t ua = r.unanchors_at.empty() ? 0 : parse_iso(r.unanchors_at);
        if (ua > nowt) te = ua;  // the unanchor completion clock
    }
    if (te <= nowt) {
        txt = "NONE";
        col = IM_COL32(128, 136, 150, 255);
        return;
    }
    time_t ts = r.state_timer_start.empty() ? 0 : parse_iso(r.state_timer_start);
    if (ts <= 0 || ts >= te) ts = te - 24 * 3600;
    double frac = (double)(nowt - ts) / (double)(te - ts);
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    txt = fmt_dur((double)(te - nowt));
    col = mix_u32(IM_COL32(250, 215, 70, 255), IM_COL32(255, 70, 70, 255), (float)frac);
}

// Athanor/Tatara moon pull: countdown runs yellow -> green toward arrival
// (the pull landing is good news), then POPPED for 24h, then RESET.
static void moonpull_info(const Row& r, bool extr_ok, std::string& txt, ImU32& col) {
    if (!extr_ok) {
        txt = "?";
        col = IM_COL32(128, 136, 150, 255);
        return;
    }
    time_t nowt = time(nullptr);
    time_t arr = r.chunk_arrival.empty() ? 0 : parse_iso(r.chunk_arrival);
    if (arr <= 0) {
        txt = "RESET";
        col = IM_COL32(255, 70, 70, 255);
        return;
    }
    if (arr > nowt) {
        time_t st = r.extraction_start.empty() ? 0 : parse_iso(r.extraction_start);
        if (st <= 0 || st >= arr) st = arr - 7 * 86400;
        double frac = (double)(nowt - st) / (double)(arr - st);
        frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
        txt = fmt_dur((double)(arr - nowt));
        col = mix_u32(IM_COL32(250, 215, 70, 255), IM_COL32(80, 230, 110, 255), (float)frac);
    } else if (nowt - arr < 24 * 3600) {
        txt = "POPPED";
        col = IM_COL32(80, 230, 110, 255);
    } else {
        txt = "RESET";
        col = IM_COL32(255, 70, 70, 255);
    }
}

// both meters stacked in one cell: fuel blocks on top, gas/oz below
static void dual_gauge(const Row& r, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ((ImGui::GetTextLineHeight() + 6) * 2 - 2) / 2;
    bool has2 = !r.fuel2_name.empty();
    char b[32];
    std::snprintf(b, sizeof b, "%.1fd", r.days);
    ImTextureID ic2 = r.gas_day > 0 ? g_ic_gas : g_ic_ozone;
    if (!r.has_fuel)
        mini_bar(dl, p, w, h, -1, "--", "", false, g_ic_fuel);
    else
        mini_bar(dl, p, w, h, r.days / GAUGE_DAYS, b, to30(units_raw(r)), false, g_ic_fuel);
    ImVec2 p2(p.x, p.y + h + 2);
    if (!has2) {  // type has no secondary fuel: hatched NA placeholder
        mini_bar(dl, p2, w, h, -1, "NA", "", true, 0);
    } else if (r.fuel2 < 0) {
        mini_bar(dl, p2, w, h, -1, "?", "", true, ic2);
    } else if (r.gas_day > 0) {
        char g[32];
        std::snprintf(g, sizeof g, "%.1fd", fuel2_days(r));
        mini_bar(dl, p2, w, h, fuel2_days(r) / GAUGE_DAYS, g, to30(gas30_raw(r)), true, ic2);
    } else {
        mini_bar(dl, p2, w, h, r.lo_target > 0 ? r.fuel2 / r.lo_target : -1,
                 compact_units(r.fuel2), to30(gas30_raw(r)), true, ic2);
    }
    ImVec2 p3(p.x, p.y + 2 * (h + 2));
    if (r.goo_cap > 0) {  // Metenox moongoo bay: fill % + haul value (inverted
                          // ramp: a full bay is the one that needs emptying)
        double gf = r.goo_m3 >= 0 ? r.goo_m3 / r.goo_cap : -1;
        char pc[32];
        std::snprintf(pc, sizeof pc, "%.0f%%", 100.0 * (gf < 0 ? 0 : gf));
        mini_bar(dl, p3, w, h, gf, gf < 0 ? "?" : pc,
                 isk_compact(r.goo_isk < 0 ? 0 : r.goo_isk) + " isk", false, g_ic_ozone, true);
    } else if (r.type == "Metenox") {  // bay exists but this data source can't see it
        mini_bar(dl, p3, w, h, -1, "?", "", false, g_ic_ozone);
    } else {  // no moon material bay on this type: hatched NA placeholder
        mini_bar(dl, p3, w, h, -1, "NA", "", false, 0);
    }
    ImGui::Dummy(ImVec2(w, gauge_cell_h(3)));  // uniform rows: every type = Metenox height
}

// --- the gauge widget: fill fraction + text inside -----------------------------
static void gauge(const char* id, double frac, const std::string& left,
                  const std::string& right, float w, bool secondary = false,
                  ImTextureID icon = 0, bool invert = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight() + 4;
    ImU32 col = band_u32(-1);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(30, 32, 44, 255), 3.0f);
    if (frac >= 0) {
        float f = frac > 1 ? 1.f : (float)frac;
        col = ramp_u32(invert ? 1.0f - f : f, secondary);
        dl->AddRectFilled(p, ImVec2(p.x + w * f, p.y + h), col, 3.0f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), (col & 0xFFFFFF) | 0x60000000, 3.0f);
    }
    float tx0 = p.x + 5;
    if (icon) {
        float s = h - 4;
        dl->AddImage(icon, ImVec2(p.x + 3, p.y + 2), ImVec2(p.x + 3 + s, p.y + 2 + s));
        tx0 = p.x + 3 + s + 5;
    }
    float fillx = frac >= 0 ? p.x + w * (frac > 1 ? 1.f : (float)frac) : p.x;
    // clip-split labels: white over the fill, ramp colour over the trough
    auto put = [&](const std::string& s, bool rightside) {
        if (s.empty()) return;
        ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        float x = rightside ? p.x + w - ts.x - 5 : tx0;
        ImVec2 tp(x, p.y + 2);
        dl->PushClipRect(p, ImVec2(fillx, p.y + h), true);
        dl->AddText(tp, IM_COL32(12, 12, 18, 255), s.c_str());
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(fillx, p.y), ImVec2(p.x + w, p.y + h), true);
        dl->AddText(tp, col, s.c_str());
        dl->PopClipRect();
    };
    if (frac < 0) {  // no gauge: centered label
        ImVec2 ts = ImGui::CalcTextSize(left.c_str());
        dl->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + 2), col, left.c_str());
    } else {
        put(left, false);
        ImVec2 lts = ImGui::CalcTextSize(left.c_str());
        ImVec2 rts = ImGui::CalcTextSize(right.c_str());
        if (tx0 + lts.x + 10 + rts.x <= p.x + w - 5) put(right, true);
    }
    ImGui::Dummy(ImVec2(w, h));
    (void)id;
}

// --- background actions (mirrors of the TUI lambdas, on the shared core) ------
static void act_refresh() {
    if (g_busy) return;
    g_busy = true;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_status = g_standalone ? "refreshing from ESI..." : "kicking a live ESI pull on the box...";
    }
    spawn_bg([]() {
        if (g_standalone)
            standalone_cycle();
        else {
            ingest(standalone::http_get_body(g_refresh_url));
        }
        {
            std::lock_guard<std::mutex> l(g_mtx);
            g_status.clear();
        }
        g_busy = false;
        if (g_gui_wake) g_gui_wake();
    });
}

static void act_add_character() {
    if (!g_standalone) return;
    if (g_busy) {  // a login (or refresh) is still in flight: say so instead
                   // of silently eating the click - the SSO listener gives up
                   // after 3 minutes and frees the busy flag
        std::lock_guard<std::mutex> l(g_mtx);
        g_note = "still waiting on the previous login/refresh (browser logins time "
                 "out after 3 min) - try again shortly";
        g_note_at = time(nullptr);
        if (g_gui_wake) g_gui_wake();
        return;
    }
    g_busy = true;
    spawn_bg([]() {
        std::string err, name;
        bool ok = standalone::login(g_client_id, err, &name, [](const std::string& s) {
            std::lock_guard<std::mutex> l(g_mtx);
            g_status = s;
            if (g_gui_wake) g_gui_wake();
        });
        {
            std::lock_guard<std::mutex> l(g_mtx);
            g_note = ok ? "added " + name : "login failed: " + err;
            g_note_at = time(nullptr);
            g_status.clear();
        }
        if (ok) standalone_cycle();
        g_busy = false;
        if (g_gui_wake) g_gui_wake();
    });
}

static void act_claim(long long sid) {
    if (g_busy || sid == 0 || g_standalone) return;
    g_busy = true;
    const char* env = std::getenv("STOKER_NAME");
    std::string who = (env && *env) ? env : "";
    spawn_bg([sid, who]() {
        std::string url = g_claim_url + "&structure_id=" + std::to_string(sid) +
                          "&by=" + urlenc(who);
        std::string res = standalone::http_post_form(url, "");
        std::string note = "claim failed: no reply from the box";
        try {
            json j = json::parse(res);
            if (j.value("ok", false))
                note = (j.value("status", "") == "stamped" ? "stamped: " : "claim pending: ") +
                       j.value("structure", std::string("?"));
            else
                note = "claim rejected: " + j.value("error", std::string("?"));
        } catch (...) {}
        {
            std::lock_guard<std::mutex> l(g_mtx);
            g_note = note;
            g_note_at = time(nullptr);
        }
        ingest(standalone::http_get_body(g_fetch_url));
        g_busy = false;
        if (g_gui_wake) g_gui_wake();
    });
}

static void act_update(const std::string& tag, const std::string& url) {
    if (g_busy) return;
    g_busy = true;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_status = "downloading " + tag + "...";
    }
    spawn_bg([tag, url]() {
        std::string res = apply_update(tag, url);
        {
            std::lock_guard<std::mutex> l(g_mtx);
            g_note = res;
            g_note_at = time(nullptr);
            g_status.clear();
            if (res.rfind("updated", 0) == 0) g_update_tag.clear();
        }
        g_busy = false;
        if (g_gui_wake) g_gui_wake();
    });
}

// --- main ----------------------------------------------------------------------
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    g_defer_login = true;
    if (argc > 1 && std::string(argv[1]) == "--version") {
        std::printf("STOKER %s (gui)\n", STOKER_VERSION);
        return 0;
    }
    {
        std::error_code ec;
        auto old_exe = own_exe();
        if (!old_exe.empty()) { old_exe += ".old"; std::filesystem::remove(old_exe, ec); }
    }
    if (run_cmd("curl --version" QUIET).rfind("curl", 0) != 0) {
#if defined(_WIN32)
        MessageBoxA(nullptr, "STOKER needs curl.exe (ships with Windows 10 1803+).",
                    "STOKER", MB_ICONERROR);
#else
        std::fprintf(stderr, "STOKER needs curl installed.\n");
#endif
        return 1;
    }
    const bool force_corp = argc > 1 && std::string(argv[1]) == "--corp";
    if (!load_or_setup(force_corp)) return 1;

    if (!glfwInit()) return 1;
    GLFWwindow* win = glfwCreateWindow(1280, 820, "STOKER", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    {
        GLFWimage ims[3] = {{64, 64, (unsigned char*)kWinIcon64},
                            {32, 32, (unsigned char*)kWinIcon32},
                            {16, 16, (unsigned char*)kWinIcon16}};
        glfwSetWindowIcon(win, 3, ims);
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    g_gui_wake = [] { glfwPostEmptyEvent(); };

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF((void*)kFontData, (int)kFontDataSize, 17.0f, &fc);
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.055f, 0.09f, 1);
    st.Colors[ImGuiCol_Header] = ImVec4(0.35f, 0.05f, 0.28f, 1);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.08f, 0.36f, 1);
    st.Colors[ImGuiCol_Tab] = ImVec4(0.10f, 0.10f, 0.16f, 1);
    st.Colors[ImGuiCol_TabActive] = ImVec4(0.42f, 0.06f, 0.34f, 1);
    st.Colors[ImGuiCol_TabHovered] = ImVec4(0.55f, 0.10f, 0.44f, 1);
    st.Colors[ImGuiCol_TableRowBg] = ImVec4(0.075f, 0.07f, 0.115f, 1);
    st.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.105f, 0.075f, 0.15f, 1);
    st.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.00f, 0.55f, 0.62f, 1);
    st.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.00f, 0.70f, 0.78f, 1);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    for (auto& e : kStructIcons)
        g_type_icons[e.type] = make_icon(e.rgba, kStructIconSize, true);
    g_ic_fuel = make_icon(kIcon_fuel);
    g_ic_gas = make_icon(kIcon_gas);
    g_ic_ozone = make_icon(kIcon_ozone);

    std::thread th(worker);
    if (g_standalone && !standalone::have_login()) act_add_character();

    char filter[128] = {0};
    // moon-rental corp/rented filter: starts active; only shown when the
    // rentals feed is online for the current tab
    static bool g_corp_only = true;
    static std::string g_type_tab;  // station-type tab; "" = All
    long long detail_sid = 0;
    int view_tab = 0;  // 0 detail, 1 refuel log

    while (!glfwWindowShouldClose(win) && g_run) {
        glfwWaitEventsTimeout(g_flash_active ? 0.06 : 0.25);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        std::vector<Row> rows;
        std::vector<Refuel> refuels;
        std::vector<std::string> tabs;
        std::string status, note, corpname, upd, updurl, pulled, esiMod;
        int tabsel;
        bool rentals_online, extractions_ok;
        {
            std::lock_guard<std::mutex> l(g_mtx);
            rows = g_rows;
            refuels = g_refuels;
            tabs = g_tab_labels;
            tabsel = g_tab;
            rentals_online = g_rentals_online;
            extractions_ok = g_extractions_ok;
            status = g_status;
            corpname = g_corp_name;
            upd = g_update_tag;
            updurl = g_update_url;
            esiMod = g_esi_lastmod;
            if (g_note_at && time(nullptr) - g_note_at < 20) note = g_note;
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        // header row
        ImGui::TextColored(PINK, "STOKER");
        ImGui::SameLine();
        ImGui::TextColored(GREY_, "%s", STOKER_VERSION);
        ImGui::SameLine();
        ImGui::TextColored(DIMCYAN, " %s fuel watch",
                           corpname.empty() ? "..." : corpname.c_str());
        ImGui::SameLine();
        if (!esiMod.empty()) {
            ImGui::TextColored(GREY_, "  game data %s", rel_age(esiMod).c_str());
            ImGui::SameLine();
        }
        if (!upd.empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.42f, 0.05f, 1));
            if (ImGui::SmallButton(("update " + upd).c_str())) act_update(upd, updurl);
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        float rightw = g_standalone ? 250.0f : 160.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - rightw);
        if (ImGui::SmallButton("refresh")) act_refresh();
        if (g_standalone) {
            ImGui::SameLine();
            if (ImGui::SmallButton("+ add character")) act_add_character();
        }
        if (!note.empty()) {
            ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.51f, 1), "%s", note.c_str());
        } else if (!status.empty()) {
            ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.27f, 1), "%s", status.c_str());
        } else {
            ImGui::TextColored(GREY_, "%s", rows.empty() ? "loading..." : " ");
        }

        // corp tabs
        if (tabs.size() > 1 && ImGui::BeginTabBar("corps")) {
            for (int i = 0; i < (int)tabs.size(); i++) {
                if (ImGui::BeginTabItem(tabs[i].empty() ? "?" : tabs[i].c_str())) {
                    if (i != tabsel) {
                        std::string data;
                        {
                            std::lock_guard<std::mutex> l(g_mtx);
                            if (i < (int)g_tab_data.size()) {
                                g_tab = i;
                                data = g_tab_data[i];
                            }
                        }
                        if (!data.empty()) ingest(data);
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        static int mode = 0;
        ImGui::RadioButton("Fuel", &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Map", &mode, 1);
        ImGui::SameLine();
        ImGui::TextColored(GREY_, " ");
        ImGui::SameLine();

        eve_logs_scan();  // pilot position + intel from the client's chat logs
        g_flash_active = false;  // re-armed by any warning drawn this frame

        if (mode == 1) {
            static char region[64] = {0};
            static bool autocentered = false;
            MapView mv;
            {
                std::lock_guard<std::mutex> l(g_mtx);
                mv = g_map;
            }
            // default view: the whole universe, centred on the pilot's region
            if (!mv.ok && !mv.loading && mv.error.empty()) act_build_universe();
            if (mv.ok && mv.region == "New Eden" && !autocentered) {
                // pilot position when the client logs give us one, else the
                // first tracked structure's system
                std::string cs = !g_pilots.empty() ? g_pilots[0].system
                                 : !rows.empty()   ? rows[0].system
                                                   : "";
                if (!cs.empty()) {
                    std::lock_guard<std::mutex> l(g_mtx);
                    auto sit = g_sysid.find(lower_(cs));
                    if (sit != g_sysid.end()) {
                        map_center_on_system(g_map, sit->second);
                        autocentered = true;
                        mv = g_map;
                    }
                }
            }
            ImGui::SetNextItemWidth(220);
            bool go = ImGui::InputTextWithHint("##region", "region (e.g. Querious)", region,
                                               sizeof region, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("load map") || go) && region[0]) act_build_map(region);
            ImGui::SameLine();
            if (ImGui::Button("universe")) {
                act_build_universe();
                autocentered = false;  // re-centre on the pilot once it rebuilds
            }
            ImGui::SameLine();
            if (ImGui::Button("me") && !g_pilots.empty()) {
                std::lock_guard<std::mutex> l(g_mtx);
                auto sit = g_sysid.find(lower_(g_pilots[0].system));
                if (sit != g_sysid.end() && g_map.ok && g_map.region == "New Eden")
                    map_center_on_system(g_map, sit->second);
            }
            ImGui::SameLine();
            if (mv.loading) ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.27f, 1), "fetching DOTLAN layout...");
            else if (!mv.error.empty()) ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", mv.error.c_str());
            else if (mv.ok) ImGui::TextColored(GREY_, "%s: %d systems - wheel zooms, drag pans, click selects", mv.region.c_str(), (int)mv.nodes.size());
            if (g_logs_dir.empty())
                ImGui::TextColored(GREY_,
                                   "eve chat logs not found - set \"eve_logs\" in config.json "
                                   "for the pilot/intel overlay");

            // structures per system for highlights + the side panel
            std::map<std::string, std::vector<const Row*>> by_sys;
            for (auto& r : rows) by_sys[r.system].push_back(&r);

            // chat-log overlay lookups (lowercased system names)
            std::map<std::string, time_t> intel_at;
            std::map<std::string, bool> pilot_sys;
            for (auto& ih : g_intel)
                if (!intel_at.count(lower_(ih.system))) intel_at[lower_(ih.system)] = ih.at;
            for (auto& pl : g_pilots) pilot_sys[lower_(pl.system)] = true;

            ImGui::BeginChild("map", ImVec2(ImGui::GetContentRegionAvail().x * 0.75f, 0), true);
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImVec2 sz = ImGui::GetContentRegionAvail();
                ImGui::InvisibleButton("mapcanvas", sz);
                bool hovered = ImGui::IsItemHovered();
                auto& io2 = ImGui::GetIO();
                static MapView* live = nullptr;  // pan/zoom act on the shared state
                {
                    std::lock_guard<std::mutex> l(g_mtx);
                    live = &g_map;
                    if (hovered && io2.MouseWheel != 0) {
                        float f = io2.MouseWheel > 0 ? 1.25f : 0.8f;
                        live->zoom = std::clamp(live->zoom * f, 1.0f, 80.0f);
                    }
                    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                        ImVec2 d = io2.MouseDelta;
                        live->pan.x -= d.x / (live->zoom * sz.x);
                        live->pan.y -= d.y / (live->zoom * sz.y);
                    }
                    mv = *live;
                }
                auto at = [&](const MapNode& n) {
                    return ImVec2(p0.x + sz.x * 0.5f + (float)(n.nx - mv.pan.x) * mv.zoom * sz.x,
                                  p0.y + sz.y * 0.5f + (float)(n.ny - mv.pan.y) * mv.zoom * sz.y);
                };
                for (auto& g : mv.gates) {
                    ImVec2 a = at(mv.nodes[g.first]), b = at(mv.nodes[g.second]);
                    if ((a.x < p0.x && b.x < p0.x) || (a.y < p0.y && b.y < p0.y) ||
                        (a.x > p0.x + sz.x && b.x > p0.x + sz.x) ||
                        (a.y > p0.y + sz.y && b.y > p0.y + sz.y))
                        continue;
                    dl->AddLine(a, b, IM_COL32(70, 74, 92, 255));
                }
                // big region names over the universe map, fading out as the
                // zoom closes in (eveterm's behaviour)
                if (mv.region == "New Eden") {
                    int ra = (int)std::clamp(240.0f * (10.0f - mv.zoom) / 6.0f, 0.0f, 240.0f);
                    if (ra > 2) {
                        ImU32 rcol = IM_COL32(208, 218, 240, ra);
                        ImFont* rfont = ImGui::GetFont();
                        float rfs = 22.0f;
                        for (auto& rl : g_region_labels) {
                            MapNode fake;
                            fake.nx = rl.nx;
                            fake.ny = rl.ny;
                            ImVec2 s = at(fake);
                            if (s.x < p0.x - 200 || s.x > p0.x + sz.x + 200 || s.y < p0.y - 40 ||
                                s.y > p0.y + sz.y + 40)
                                continue;
                            float tw = rfont->CalcTextSizeA(rfs, 1e30f, 0.0f, rl.name.c_str()).x;
                            dl->AddText(rfont, rfs, ImVec2(s.x - tw * 0.5f, s.y - rfs * 0.5f),
                                        rcol, rl.name.c_str());
                        }
                    }
                }
                // friendly Ansiblex bridges: blue arcs over the gate lines,
                // matching eveterm's map
                if (!g_jbridges.empty()) {
                    std::map<int, int> byid;
                    for (int i = 0; i < (int)mv.nodes.size(); i++) byid[mv.nodes[i].id] = i;
                    for (auto& jb : g_jbridges) {
                        auto a = byid.find(jb.first), b = byid.find(jb.second);
                        if (a == byid.end() || b == byid.end()) continue;
                        ImVec2 pa = at(mv.nodes[a->second]), pb = at(mv.nodes[b->second]);
                        if ((pa.x < p0.x && pb.x < p0.x) || (pa.y < p0.y && pb.y < p0.y) ||
                            (pa.x > p0.x + sz.x && pb.x > p0.x + sz.x) ||
                            (pa.y > p0.y + sz.y && pb.y > p0.y + sz.y))
                            continue;
                        ImVec2 mid((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f);
                        ImVec2 d(pb.x - pa.x, pb.y - pa.y);
                        dl->AddBezierQuadratic(pa,
                                               ImVec2(mid.x - d.y * 0.18f, mid.y + d.x * 0.18f),
                                               pb, IM_COL32(90, 175, 255, 220), 1.6f);
                    }
                }
                int clicked = -1;
                // on the universe map, labels only appear once zoomed in enough
                // to read them; structure and selected systems always label
                bool labels = mv.nodes.size() < 600 || mv.zoom >= 8.0f;
                for (int i = 0; i < (int)mv.nodes.size(); i++) {
                    ImVec2 q = at(mv.nodes[i]);
                    if (q.x < p0.x - 30 || q.x > p0.x + sz.x + 30 || q.y < p0.y - 10 ||
                        q.y > p0.y + sz.y + 10)
                        continue;
                    auto bs = by_sys.find(mv.nodes[i].label);
                    ImU32 dot = IM_COL32(160, 168, 190, 255);
                    if (bs != by_sys.end()) {
                        int worst = 3;
                        for (auto* r : bs->second)
                            if (r->has_fuel) worst = std::min(worst, urgency_band(r->days));
                        dot = band_u32(worst);
                        dl->AddCircle(q, 7.0f, dot, 0, 2.0f);
                    }
                    std::string ll = lower_(mv.nodes[i].label);
                    if (auto ia = intel_at.find(ll); ia != intel_at.end()) {
                        double age = difftime(time(nullptr), ia->second);
                        int alpha = age < 300
                                        ? (int)(150 + 105 * std::sin(ImGui::GetTime() * 6.0))
                                        : (int)(255 * (1.0 - age / 1800.0));
                        if (alpha > 0)
                            dl->AddCircle(q, 12.0f, IM_COL32(255, 60, 60, alpha), 0, 2.5f);
                    }
                    if (pilot_sys.count(ll))
                        dl->AddCircle(q, 9.0f, IM_COL32(80, 255, 140, 255), 0, 2.5f);
                    bool selq = mv.selected == i;
                    dl->AddCircleFilled(q, selq ? 4.5f : 3.0f, selq ? IM_COL32(0, 229, 255, 255) : dot);
                    if (labels || selq || bs != by_sys.end())
                        dl->AddText(ImVec2(q.x + 6, q.y - 7),
                                    selq ? IM_COL32(0, 229, 255, 255)
                                         : IM_COL32(200, 206, 222, 255),
                                    mv.nodes[i].label.c_str());
                    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        ImVec2 mp = io2.MousePos;
                        float dx = mp.x - q.x, dy = mp.y - q.y;
                        if (dx * dx + dy * dy < 12 * 12) clicked = i;
                    }
                }
                if (clicked >= 0) {
                    std::lock_guard<std::mutex> l(g_mtx);
                    g_map.selected = clicked;
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("mapside", ImVec2(0, 0), true);
            ImGui::PushTextWrapPos(0.0f);
            if (!g_pilots.empty()) {
                for (auto& pl : g_pilots) {
                    ImGui::TextColored(ImVec4(0.31f, 1.0f, 0.55f, 1), "%s", pl.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(TEXTC, " %s", pl.system.c_str());
                }
                ImGui::Separator();
            }
            if (!g_intel.empty()) {
                ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "intel (last 30m)");
                time_t nowt = time(nullptr);
                int shown = 0;
                for (auto& ih : g_intel) {
                    if (shown++ >= 8) break;
                    long age = (long)difftime(nowt, ih.at);
                    ImGui::TextColored(GREY_, "[%ldm]", age / 60);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", ih.system.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(TEXTC, " %s", ih.text.c_str());
                }
                ImGui::Separator();
            }
            ImGui::PopTextWrapPos();
            if (mv.selected >= 0 && mv.selected < (int)mv.nodes.size()) {
                const MapNode& n = mv.nodes[mv.selected];
                ImGui::TextColored(CYAN_, "%s", n.label.c_str());
                if (ImGui::Button("set destination")) act_set_destination(n.id, n.label);
                ImGui::Separator();
                auto bs = by_sys.find(n.label);
                if (bs == by_sys.end()) {
                    ImGui::TextColored(GREY_, "no tracked structures here");
                } else {
                    for (auto* r : bs->second) {
                        char db2[32];
                        std::snprintf(db2, sizeof db2, "%.1fd", r->days);
                        gauge(("m" + std::to_string(r->sid)).c_str(),
                              r->has_fuel ? r->days / GAUGE_DAYS : -1,
                              r->has_fuel ? db2 : "--", "",
                              ImGui::GetContentRegionAvail().x - 4, false, g_ic_fuel);
                        ImGui::TextColored(GREY_, "%s", r->name.c_str());
                        ImGui::Spacing();
                    }
                }
            } else {
                ImGui::TextColored(GREY_, "click a system");
            }
            ImGui::EndChild();
            ImGui::End();
            ImGui::Render();
            int w2, h2;
            glfwGetFramebufferSize(win, &w2, &h2);
            glViewport(0, 0, w2, h2);
            glClearColor(0.055f, 0.055f, 0.09f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(win);
            continue;
        }

        // filter
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##filter", "filter name/system/type", filter, sizeof filter);
        int suppressed = 0;
        for (auto& r : rows)
            if (r.state == "unanchored") suppressed++;
        if (suppressed) {
            ImGui::SameLine();
            char hl[48];
            std::snprintf(hl, sizeof hl, "show hidden (%d)", suppressed);
            ImGui::Checkbox(hl, &g_show_hidden);
        }
        int rented = 0;
        if (rentals_online) {
            for (auto& r : rows)
                if (r.rental == "private") rented++;
            ImGui::SameLine();
            ImGui::Checkbox("corp only", &g_corp_only);
            if (g_corp_only && rented) {
                ImGui::SameLine();
                ImGui::TextColored(GREY_, "(%d rented hidden)", rented);
            }
        }
        ImGui::SameLine();
        int u14 = 0, u7 = 0;
        for (auto& r : rows) {
            if (r.has_fuel && r.days < 14) u14++;
            if (r.has_fuel && r.days < 7) u7++;
        }
        ImGui::TextColored(GREY_, "  %d structures   under14d", (int)rows.size());
        ImGui::SameLine();
        ImGui::TextColored(u14 ? ImVec4(0.98f, 0.84f, 0.27f, 1) : ImVec4(0.35f, 0.88f, 0.51f, 1), "%d", u14);
        ImGui::SameLine();
        ImGui::TextColored(GREY_, "  under7d");
        ImGui::SameLine();
        ImGui::TextColored(u7 ? ImVec4(1, 0.27f, 0.27f, 1) : ImVec4(0.35f, 0.88f, 0.51f, 1), "%d", u7);

        // filtered view
        std::string f = filter;
        for (auto& c : f) c = (char)tolower((unsigned char)c);
        std::vector<const Row*> view;
        for (auto& r : rows) {
            // automated removal: fully-unanchored hulls drop out on their own
            // (destroyed ones leave the ESI feed by themselves)
            if (!g_show_hidden && r.state == "unanchored") continue;
            if (rentals_online && g_corp_only && r.rental == "private") continue;
            if (!g_type_tab.empty() && r.type != g_type_tab) continue;
            if (!f.empty()) {
                std::string hay = r.name + " " + r.system + " " + r.type;
                for (auto& c : hay) c = (char)tolower((unsigned char)c);
                if (hay.find(f) == std::string::npos) continue;
            }
            view.push_back(&r);
        }

        // split: table left, detail/log right
        float leftw = ImGui::GetContentRegionAvail().x * 0.62f;
        ImGui::BeginChild("left", ImVec2(leftw, 0), true);
        // station-type tabs sit where the column header used to be, built from
        // whatever types this corp actually has
        {
            std::vector<std::string> types;
            for (auto& r : rows)
                if (!r.type.empty() &&
                    std::find(types.begin(), types.end(), r.type) == types.end())
                    types.push_back(r.type);
            std::sort(types.begin(), types.end());
            if (ImGui::BeginTabBar("stypes")) {
                if (ImGui::BeginTabItem("All")) {
                    g_type_tab.clear();
                    ImGui::EndTabItem();
                }
                for (auto& t : types)
                    if (ImGui::BeginTabItem(t.c_str())) {
                        g_type_tab = t;
                        ImGui::EndTabItem();
                    }
                ImGui::EndTabBar();
            }
        }
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 6));
        if (ImGui::BeginTable("structs", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Fuel", ImGuiTableColumnFlags_WidthFixed, 250);
            ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);

            // no header row to click: always most-urgent-first (worst of the
            // fuel and gas clocks)
            std::stable_sort(view.begin(), view.end(), [](const Row* a, const Row* b) {
                auto worst = [](const Row* r) {
                    double d = r->has_fuel ? r->days : 1e9;
                    double g = fuel2_days(*r);
                    if (g >= 0) d = std::min(d, g);
                    return d;
                };
                return worst(a) < worst(b);
            });

            for (auto* rp : view) {
                const Row& r = *rp;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                dual_gauge(r, 240);
                ImGui::TableSetColumnIndex(1);
                {
                    bool hot = false;
                    time_t nowt = time(nullptr);
                    for (auto& ih : g_intel)
                        if (difftime(nowt, ih.at) < 900 && ih.system == r.system) {
                            hot = true;
                            break;
                        }
                    ImGui::TextColored(hot ? ImVec4(1, 0.27f, 0.27f, 1) : CYAN_, "%s",
                                       r.system.c_str());
                    // power state under the system name, derived the way the
                    // game derives it: fuel gone = Low Power, 7+ days without
                    // fuel = Abandoned; all services off while fueled = Offline
                    const char* ftxt = nullptr;
                    ImU32 fcol = 0;
                    if (r.has_fuel && r.days <= -7) {
                        ftxt = "ABANDONED";
                        fcol = IM_COL32(255, 70, 70, 255);
                    } else if (!r.has_fuel || r.days <= 0) {
                        ftxt = "LOW POWER";
                        fcol = IM_COL32(255, 140, 0, 255);
                    } else if (r.sv_on == 0 && r.sv_off > 0) {
                        ftxt = "OFFLINE";
                        fcol = IM_COL32(128, 136, 150, 255);
                    } else if (r.days < 7) {
                        ftxt = "LOW FUEL";
                        fcol = IM_COL32(250, 215, 70, 255);
                    }
                    if (ftxt) {
                        ImDrawList* dls = ImGui::GetWindowDrawList();
                        ImVec2 wp = ImGui::GetCursorScreenPos();
                        float fs2 = std::min(ImGui::GetTextLineHeight() - 3.0f, 13.0f);
                        warn_tri(dls, ImVec2(wp.x - 5, wp.y + 2), fs2, fcol);
                        dls->AddText(ImGui::GetFont(), fs2, ImVec2(wp.x - 5 + fs2 + 2, wp.y + 1),
                                     fcol, ftxt);
                    }
                }
                ImGui::TableSetColumnIndex(2);
                float rowh = gauge_cell_h(3);  // uniform: every row Metenox-sized
                float cw = ImGui::GetContentRegionAvail().x;
                ImVec2 cp = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable(("##row" + std::to_string(r.sid)).c_str(),
                                      r.sid == detail_sid,
                                      ImGuiSelectableFlags_SpanAllColumns,
                                      ImVec2(0, rowh))) {
                    detail_sid = r.sid;
                    view_tab = 0;
                }
                ImDrawList* dl2 = ImGui::GetWindowDrawList();
                float lh = ImGui::GetTextLineHeight();
                ImFont* fnt = ImGui::GetFont();
                // structure-type portrait on the left edge; the text block
                // indents past it (space reserved even without art so rows align)
                float ix = rowh - 8 + 8;
                {
                    auto ti = g_type_icons.find(r.type);
                    if (ti != g_type_icons.end())
                        dl2->AddImageRounded(ti->second, ImVec2(cp.x, cp.y + 1),
                                             ImVec2(cp.x + rowh - 8, cp.y + rowh - 7),
                                             ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32(255, 255, 255, 255), 5.0f);
                }
                dl2->AddText(ImVec2(cp.x + ix, cp.y), ImGui::ColorConvertFloat4ToU32(TEXTC),
                             r.name.c_str());
                // under the name: the layer checklist (green = intact, red =
                // stripped), with Timer and Moon Pull in a column next to it
                {
                    float fs = std::min(lh - 2.0f, 15.0f);  // compact sub-rows
                    float y0 = cp.y + lh + 3, sp = fs + 3;
                    int layers = 3;
                    if (r.state == "armor_reinforce" || r.state == "armor_vulnerable")
                        layers = 2;
                    else if (r.state == "hull_reinforce" || r.state == "hull_vulnerable")
                        layers = 1;
                    const char* lbl[3] = {"Shield", "Armour", "Hull"};
                    for (int li = 0; li < 3; li++) {
                        float y = y0 + li * sp;
                        dl2->AddCircleFilled(ImVec2(cp.x + ix + 6, y + fs * 0.55f), 3.0f,
                                             layers >= 3 - li ? IM_COL32(80, 230, 110, 255)
                                                              : IM_COL32(255, 70, 70, 255));
                        dl2->AddText(fnt, fs, ImVec2(cp.x + ix + 13, y),
                                     IM_COL32(200, 206, 222, 255), lbl[li]);
                    }
                    float tx =
                        cp.x + ix + 13 + fnt->CalcTextSizeA(fs, 1e30f, 0, "Armour").x + 18;
                    (void)cw;
                    {
                        dl2->AddText(fnt, fs, ImVec2(tx, y0), IM_COL32(128, 136, 150, 255),
                                     "Timer:");
                        std::string tt;
                        ImU32 tc;
                        timer_info(r, tt, tc);
                        dl2->AddText(fnt, fs,
                                     ImVec2(tx + fnt->CalcTextSizeA(fs, 1e30f, 0, "Moon Pull:").x +
                                                6,
                                            y0),
                                     tc, tt.c_str());
                    }
                    if (r.type == "Athanor" || r.type == "Tatara") {
                        dl2->AddText(fnt, fs, ImVec2(tx, y0 + sp), IM_COL32(128, 136, 150, 255),
                                     "Moon Pull:");
                        std::string mt;
                        ImU32 mc;
                        moonpull_info(r, extractions_ok, mt, mc);
                        dl2->AddText(fnt, fs,
                                     ImVec2(tx + fnt->CalcTextSizeA(fs, 1e30f, 0, "Moon Pull:").x +
                                                6,
                                            y0 + sp),
                                     mc, mt.c_str());
                    }
                    // state warning under Moon Pull: no label, blank when calm,
                    // flashing triangle + text when something is happening.
                    // Mapped from the ESI state enum + unanchors_at.
                    {
                        const char* wtxt = nullptr;
                        ImU32 wcol = 0;
                        time_t ua = r.unanchors_at.empty() ? 0 : parse_iso(r.unanchors_at);
                        if (r.state == "armor_reinforce" || r.state == "hull_reinforce" ||
                            r.state == "armor_vulnerable" || r.state == "hull_vulnerable") {
                            wtxt = "UNDER ATTACK";
                            wcol = IM_COL32(255, 70, 70, 255);
                        } else if (r.state == "unanchored") {
                            wtxt = "UNANCHORED";
                            wcol = IM_COL32(250, 215, 70, 255);
                        } else if (ua > time(nullptr)) {
                            wtxt = "UNANCHORING";
                            wcol = IM_COL32(250, 215, 70, 255);
                        } else if (r.state == "onlining_vulnerable") {
                            wtxt = "ONLINING...";
                            wcol = IM_COL32(250, 215, 70, 255);
                        } else if (r.state == "anchoring" || r.state == "anchor_vulnerable" ||
                                   r.state == "deploy_vulnerable" ||
                                   r.state == "fitting_invulnerable") {
                            wtxt = "ANCHORING";
                            wcol = IM_COL32(250, 215, 70, 255);
                        }
                        if (wtxt) {
                            g_flash_active = true;
                            int wa = (int)(90 +
                                           165 * (0.5 + 0.5 * std::sin(ImGui::GetTime() * 6.0)));
                            warn_tri(dl2, ImVec2(tx, y0 + 2 * sp + 1), fs - 2, wcol, wa);
                            dl2->AddText(fnt, fs, ImVec2(tx + fs + 4, y0 + 2 * sp),
                                         (wcol & 0xFFFFFF) | ((ImU32)wa << 24), wtxt);
                        }
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(0, 0), true);
        if (ImGui::BeginTabBar("views")) {
            ImGuiTabItemFlags df = view_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Detail", nullptr, df)) {
                view_tab = -1;
                const Row* d = nullptr;
                for (auto& r : rows)
                    if (r.sid == detail_sid) { d = &r; break; }
                if (!d) {
                    ImGui::TextColored(GREY_, "select a structure");
                } else {
                    ImGui::PushTextWrapPos(0.0f);
                    {
                        auto ti = g_type_icons.find(d->type);
                        if (ti != g_type_icons.end())
                            ImGui::Image(ti->second, ImVec2(48, 48));
                        else
                            ImGui::Image(g_ic_fuel, ImVec2(24, 24));
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(CYAN_, "%s", d->name.c_str());
                    ImGui::TextColored(GREY_, "%s   %s   %s", d->system.c_str(),
                                       d->type.c_str(), d->state.c_str());
                    if (d->rental == "private")
                        ImGui::TextColored(PINK, "rented to %s",
                                           d->renter.empty() ? "?" : d->renter.c_str());
                    else if (d->rental == "corp")
                        ImGui::TextColored(GREY_, "corp moon");
                    {
                        std::string tt;
                        ImU32 tc;
                        timer_info(*d, tt, tc);
                        ImGui::TextColored(GREY_, "Timer:");
                        ImGui::SameLine();
                        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(tc), "%s", tt.c_str());
                        if (d->type == "Athanor" || d->type == "Tatara") {
                            std::string mt;
                            ImU32 mc;
                            moonpull_info(*d, extractions_ok, mt, mc);
                            ImGui::TextColored(GREY_, "Moon Pull:");
                            ImGui::SameLine();
                            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(mc), "%s",
                                               mt.c_str());
                        }
                    }
                    ImGui::Separator();
                    ImGui::Spacing();
                    float gw = ImGui::GetContentRegionAvail().x - 4;
                    char b[64];
                    std::snprintf(b, sizeof b, "%.1f days", d->days);
                    if (d->has_fuel) {
                        gauge("df", d->days / GAUGE_DAYS, b,
                              to30(units_raw(*d)), gw, false, g_ic_fuel);
                        ImGui::Spacing();
                    }
                    if (!d->fuel2_name.empty()) {
                        if (d->fuel2 < 0) {
                            std::string f2s;
                            {
                                std::lock_guard<std::mutex> l(g_mtx);
                                f2s = g_fuel2_status;
                            }
                            ImGui::Image(d->gas_day > 0 ? g_ic_gas : g_ic_ozone, ImVec2(20, 20));
                            ImGui::SameLine();
                            ImGui::TextColored(GREY_, "%s unknown - %s", d->fuel2_name.c_str(),
                                               f2s == "relogin" ? "re-login to grant corp-assets (+ add character)"
                                               : f2s == "director" ? "needs the in-game Director role"
                                                                   : "no Director-role data source");
                        } else if (d->gas_day > 0) {
                            char g2[64];
                            std::snprintf(g2, sizeof g2, "%.1f days", fuel2_days(*d));
                            gauge("df2", fuel2_days(*d) / GAUGE_DAYS, g2,
                                  to30(gas30_raw(*d)), gw, true, g_ic_gas);
                            ImGui::TextColored(GREY_, "%s %s in the bay",
                                               commas(d->fuel2).c_str(), d->fuel2_name.c_str());
                        } else {
                            gauge("df2", d->lo_target > 0 ? d->fuel2 / d->lo_target : -1,
                                  compact_units(d->fuel2), to30(gas30_raw(*d)), gw, true,
                                  g_ic_ozone);
                            ImGui::TextColored(GREY_, "%s %s in the bay",
                                               commas(d->fuel2).c_str(), d->fuel2_name.c_str());
                        }
                        ImGui::Spacing();
                    }
                    if (d->goo_cap > 0) {
                        double gf = d->goo_m3 >= 0 ? d->goo_m3 / d->goo_cap : -1;
                        char gl[32];
                        std::snprintf(gl, sizeof gl, "%.0f%% full", 100.0 * (gf < 0 ? 0 : gf));
                        gauge("dgoo", gf, gf < 0 ? "?" : gl,
                              isk_compact(d->goo_isk < 0 ? 0 : d->goo_isk) + " isk", gw, false,
                              g_ic_ozone, true);
                        ImGui::TextColored(GREY_, "moongoo %s of %s m3",
                                           commas(d->goo_m3 < 0 ? 0 : d->goo_m3).c_str(),
                                           commas(d->goo_cap).c_str());
                        for (auto& g : d->goo) {
                            ImGui::TextColored(TEXTC, "%s", g.name.c_str());
                            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.5f);
                            ImGui::TextColored(GREY_, "x%s", commas(g.qty).c_str());
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.27f, 1), " %s",
                                               isk_compact(g.isk).c_str());
                        }
                        if (!d->goo.empty())
                            ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.27f, 1),
                                               "total %s isk at market average",
                                               isk_compact(d->goo_isk).c_str());
                        ImGui::Spacing();
                    }
                    if (d->bpd > 0)
                        ImGui::TextColored(GREY_, "rate %s blocks/day from online services",
                                           commas(d->bpd).c_str());
                    if (d->burn7 >= 0 || d->burn30 >= 0) {
                        std::string bt = "burn";
                        char t2[48];
                        if (d->burn7 >= 0) { std::snprintf(t2, sizeof t2, " x%.2f (7d)", d->burn7); bt += t2; }
                        if (d->burn30 >= 0) { std::snprintf(t2, sizeof t2, " x%.2f (30d)", d->burn30); bt += t2; }
                        if (d->est >= 0) { std::snprintf(t2, sizeof t2, "  est %.1fd", d->est); bt += t2; }
                        ImGui::TextColored(TEXTC, "%s", bt.c_str());
                    }
                    if (d->lo_min >= 0)
                        ImGui::TextColored(GREY_, "ozone doctrine: min %s / fill to %s",
                                           commas(d->lo_min).c_str(), commas(d->lo_target).c_str());
                    if (d->has_refuel)
                        ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.51f, 1), "last fueled %s (+%.1fd)",
                                           rel_age(d->last_refuel).c_str(), d->refuel_added);
                    {
                        std::string ls = d->system;
                        for (auto& c : ls) c = (char)tolower((unsigned char)c);
                        load_universe();
                        auto sit = g_sysid.find(ls);
                        if (sit != g_sysid.end() && ImGui::Button("set destination"))
                            act_set_destination(sit->second, d->system);
                    }
                    if (!g_standalone) {
                        ImGui::Separator();
                        if (ImGui::Button("I fueled this structure")) act_claim(d->sid);
                    }
                    if (!d->log.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored(GREY_, "refuel history");
                        for (auto& e : d->log)
                            ImGui::Text("%s  +%.1fd  %s", rel_age(e.seen_at).c_str(),
                                        e.days_added, e.by.c_str());
                    }
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Refuel log")) {
                if (refuels.empty())
                    ImGui::TextColored(GREY_, "no refuels seen yet - the log fills in as polls catch fuel jumps");
                for (auto& v : refuels) {
                    if (v.pending) {
                        ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.27f, 1), "%s  pending  %s %s",
                                           rel_age(v.seen_at).c_str(), v.system.c_str(),
                                           v.name.c_str());
                        continue;
                    }
                    ImGui::Text("%s  +%.1fd", rel_age(v.seen_at).c_str(), v.days_added);
                    ImGui::SameLine();
                    if (v.blocks >= 0) {
                        ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.51f, 1), " %s blk",
                                           commas(v.blocks).c_str());
                        ImGui::SameLine();
                    }
                    if (!v.by.empty()) {
                        ImGui::TextColored(PINK, " %s", v.by.c_str());
                        ImGui::SameLine();
                    }
                    ImGui::TextColored(CYAN_, " %s", v.system.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(GREY_, " %s", v.name.c_str());
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.055f, 0.055f, 0.09f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    g_run = false;
    if (th.joinable()) th.join();
    {
        std::lock_guard<std::mutex> l(g_bg_mtx);
        for (auto& t : g_bg_threads)
            if (t.joinable()) t.join();
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
