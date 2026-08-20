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

static std::string lower_(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// --- the map (same layout as eveterm: DOTLAN region SVG positions + the
// embedded New Eden gate graph) -------------------------------------------------
struct MapNode {
    int id = 0;
    std::string label, llabel;  // llabel: lowercased once at build time
    double nx = 0, ny = 0;
    int region = 0;  // cross-region gates draw dashed (0 everywhere = all solid)
};
struct MapView {
    std::string region, error;
    bool loading = false, ok = false;
    std::vector<MapNode> nodes;
    std::vector<std::pair<int, int>> gates;
    std::vector<std::pair<int, int>> jb;  // jump-bridge node-index pairs
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

// resolve the friendly-bridge id pairs to node indexes at build time so the
// draw loop never rebuilds an id lookup per frame
static void map_resolve_bridges(MapView& m, const std::map<int, int>& idx) {
    for (auto& jb : g_jbridges) {
        auto a = idx.find(jb.first), b = idx.find(jb.second);
        if (a != idx.end() && b != idx.end()) m.jb.push_back({a->second, b->second});
    }
}

// Called from BOTH the render thread (eve_logs_scan, banner, set destination)
// and spawn_bg map builders, so the whole load is mutexed; a failed parse is
// tried once, not per frame.
static void load_universe() {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    static bool tried = false;
    if (tried) return;
    tried = true;
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
            std::string nm = g_sysname.count(kv.first) ? g_sysname[kv.first]
                                                       : std::to_string(kv.first);
            m.nodes.push_back(
                {kv.first, nm, lower_(nm), kv.second.nx, kv.second.ny, kv.second.region});
        }
        for (auto& kv : idx)
            for (int nb : g_adj[kv.first])
                if (kv.first < nb && idx.count(nb))
                    m.gates.push_back({kv.second, idx.at(nb)});
        map_resolve_bridges(m, idx);
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
    live.zoom = std::clamp((float)(0.85 / (span > 0.001 ? span : 0.001)), 0.3f, 60.0f);
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
            std::string nm = g_sysname.count(id) ? g_sysname[id] : std::to_string(id);
            m.nodes.push_back({id, nm, lower_(nm), x, y});
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
            map_resolve_bridges(m, idx);
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
// BFS jump counts from a source system over stargates + the friendly Ansiblex
// network - the same graph eveterm routes on (a bridge is one jump). Cached
// until the source changes; the whole of k-space is ~5,400 nodes.
static const std::map<int, int>& jump_dists(int src) {
    static std::map<int, int> dists;
    static int cached_src = -1;
    if (cached_src == src) return dists;
    cached_src = src;
    dists.clear();
    std::map<int, std::vector<int>> bridge;
    for (auto& jb : g_jbridges) {
        bridge[jb.first].push_back(jb.second);
        bridge[jb.second].push_back(jb.first);
    }
    std::deque<int> q{src};
    dists[src] = 0;
    while (!q.empty()) {
        int s = q.front();
        q.pop_front();
        int d = dists[s];
        auto push = [&](int n) {
            if (!dists.count(n)) {
                dists[n] = d + 1;
                q.push_back(n);
            }
        };
        auto it = g_adj.find(s);
        if (it != g_adj.end())
            for (int n : it->second) push(n);
        auto bt = bridge.find(s);
        if (bt != bridge.end())
            for (int n : bt->second) push(n);
    }
    return dists;
}

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

static const ImU32 kGreyU32 = IM_COL32(128, 136, 150, 255);  // no-data placeholder

// stepped urgency colours: only the map dots still use these (the gauges
// moved to the continuous ramp_u32)
static ImU32 band_u32(int band) {
    switch (band) {
        case 0: return IM_COL32(255, 70, 70, 255);
        case 1: return IM_COL32(255, 140, 0, 255);
        case 2: return IM_COL32(250, 215, 70, 255);
        case 3: return IM_COL32(56, 216, 232, 255);
        default: return kGreyU32;
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
static ImTextureID g_ic_fuel, g_ic_gas, g_ic_ozone, g_ic_goo;
static std::map<std::string, ImTextureID> g_type_icons;  // display type -> portrait

// POS rows all share the display type "POS" but wear their race's tower art
static std::string icon_key(const Row& r) {
    if (!r.is_pos) return r.type;
    return "POS-" + (r.pos_race.empty() ? std::string("minmatar") : r.pos_race);
}

// one half-height meter strip (small text riding inside)
static void mini_bar(ImDrawList* dl, ImVec2 p, float w, float h, double frac,
                     const std::string& left, const std::string& right, bool secondary,
                     ImTextureID icon, bool invert = false, ImU32 solid = 0) {
    ImU32 col = kGreyU32;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(30, 32, 44, 255), 2.0f);
    float fillx = p.x;
    if (frac >= 0) {
        float f = frac > 1 ? 1.f : (float)frac;
        fillx = p.x + w * f;
        col = solid ? solid : ramp_u32(invert ? 1.0f - f : f, secondary);
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

// flame tongues licking up from a bar that has been full too long.
// intensity 0..1 scales height, glow and flicker; classic fire colours
// (red-orange tongues, yellow cores), per-x hash jitter so it looks alive.
static void draw_fire(ImDrawList* dl, ImVec2 p, float w, float intensity, unsigned seed) {
    if (intensity <= 0) return;
    double t = ImGui::GetTime();
    for (float x = 5; x < w - 5; x += 7) {
        unsigned hsh = seed * 2654435761u + (unsigned)(x * 97.0f);
        float ph = (hsh % 628) / 100.0f;
        float fl = 0.55f + 0.45f * (float)std::sin(t * (4.5 + (hsh % 5) * 0.8) + ph);
        float fh = (3.0f + 12.0f * intensity) * fl;
        float bx = p.x + x, by = p.y + 1;
        int a = (int)(140 + 115 * intensity);
        dl->AddTriangleFilled(ImVec2(bx - 3, by), ImVec2(bx + 3, by), ImVec2(bx, by - fh),
                              IM_COL32(255, 96, 26, a));
        dl->AddTriangleFilled(ImVec2(bx - 1.5f, by), ImVec2(bx + 1.5f, by),
                              ImVec2(bx, by - fh * 0.55f), IM_COL32(255, 214, 90, a));
    }
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
        // a feed without timer fields (the corp-mode backend) can't say NONE
        // honestly: show unknown instead of implying there is no clock
        txt = g_timers_ok ? "NONE" : "?";
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

// skyhook theft window: counts down toward the window opening (danger
// approaching, yellow -> red over the ~4h ESI announce lead); while the
// window is OPEN the remaining time shows in red and `open` is set so the
// caller can flash RAIDABLE. Grey "none" when no window is announced.
static void skyhook_timer_info(const Row& r, std::string& txt, ImU32& col, bool& open) {
    open = false;
    time_t nowt = time(nullptr);
    time_t ws = r.sky_wstart.empty() ? 0 : parse_iso(r.sky_wstart);
    time_t we = r.sky_wend.empty() ? 0 : parse_iso(r.sky_wend);
    if (ws && we && nowt >= ws && nowt < we) {
        open = true;
        txt = fmt_dur((double)(we - nowt)) + " left";
        col = IM_COL32(255, 70, 70, 255);
        return;
    }
    if (ws && nowt < ws) {
        double frac = 1.0 - std::min(1.0, (double)(ws - nowt) / (4 * 3600.0));
        txt = fmt_dur((double)(ws - nowt));
        col = mix_u32(IM_COL32(250, 215, 70, 255), IM_COL32(255, 70, 70, 255), (float)frac);
        return;
    }
    txt = "none";
    col = kGreyU32;
}

// Athanor/Tatara moon pull, following the real chunk lifecycle: countdown
// runs yellow -> green toward arrival (the pull landing is good news), then
// READY while the chunk waits for someone to fire the laser (the timer only
// starts the ~3h fire window - reaching 0 pops nothing by itself), then
// POPPED once a MoonminingLaserFired/AutomaticFracture notification lands
// (or natural_decay passes, the guaranteed auto-fracture), then RESET once
// the asteroid field has decayed. natural_decay comes rig-adjusted straight
// from ESI; the field lifetime is 48h base, stretched by the drilling rig.
static time_t moonpull_field_life(const Row& r) {
    double mult = r.drill_rig_tier == 2 ? 2.0 : r.drill_rig_tier == 1 ? 1.5 : 1.0;
    return (time_t)(48 * 3600 * mult);
}
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
        return;
    }
    time_t decay = r.natural_decay.empty() ? 0 : parse_iso(r.natural_decay);
    if (decay <= arr) decay = arr + 3 * 3600;  // old snapshots: unrigged window
    // a pop notification from before this chunk arrived belongs to a past cycle
    time_t popped = r.popped_at >= arr - 120 ? r.popped_at : 0;
    if (!popped && nowt < decay) {
        txt = "READY " + fmt_dur((double)(decay - nowt));  // time left to fire by hand
        col = IM_COL32(255, 176, 60, 255);
        return;
    }
    time_t pop_t = popped ? popped : decay;
    if (nowt - pop_t < moonpull_field_life(r)) {
        txt = "POPPED";
        col = IM_COL32(80, 230, 110, 255);
    } else {
        txt = "RESET";
        col = IM_COL32(255, 70, 70, 255);
    }
}

// Metenox monthly profitability (goo - taxes - fuel - gas): text + colour +
// flash tier shared by the fuel-panel NET label and the details equation.
// green above 200m, yellow under it, red in the loss zone; the fuel panel
// pulses yellow under 100m and red below zero.
static std::string net_str(double v) {
    return (v < 0 ? "-" : "") + isk_compact(std::fabs(v));
}
static ImU32 net_col(double v) {
    if (v < 0) return IM_COL32(255, 70, 70, 255);
    if (v < 200e6) return IM_COL32(250, 215, 70, 255);
    return IM_COL32(80, 230, 110, 255);
}

// --- user preferences (settings window) --------------------------------------------
// Fuel levels are per DISPLAY TYPE with optional per-structure overrides:
// "alert" (days) feeds the needs-fuel filter, "cap" (days) is the bar's full
// mark and the "[units] to [cap]d" haul target. Saved to prefs.json in the
// config dir on every change.
struct FuelPref {
    float alert = 7, cap = 30;
};
enum : unsigned {
    WF_ATTACK = 1, WF_ABANDONED = 2, WF_LOWPOWER = 4, WF_OFFLINE = 8,
    WF_LOWFUEL = 16, WF_ANCHORING = 32, WF_UNANCHORING = 64, WF_TIMER = 128,
    WF_MOONPULL = 256, WF_SKYWIN = 512,
};
static const struct { unsigned bit; const char* name; } kWarnDefs[] = {
    {WF_ATTACK, "under attack"},   {WF_ABANDONED, "abandoned"},
    {WF_LOWPOWER, "low power"},    {WF_OFFLINE, "services offline"},
    {WF_LOWFUEL, "low fuel"},      {WF_ANCHORING, "anchoring/onlining"},
    {WF_UNANCHORING, "unanchoring/unanchored"},
    {WF_TIMER, "reinforce timer"}, {WF_MOONPULL, "moon pull attention"},
    {WF_SKYWIN, "skyhook window"},
};
struct Prefs {
    float goo_max_m3 = 50000;  // metenox bay bar reads full at this many m3
    bool fuel_filter = false;  // toolbar: hide rows above their alert level
    int warn_filter = 0;       // 0 = off (everything), 1 = flagged only
    unsigned warn_mask = 0xFFFFFFFFu;  // which flags feed the warn filter
    std::map<std::string, FuelPref> type_fuel, type_gas;
    std::map<long long, FuelPref> sid_fuel, sid_gas;  // per-structure overrides
    std::set<long long> hidden;  // manually hidden structures (settings unhides)
};
static Prefs g_prefs;

// Slider edits mark this instead of writing prefs.json every frame of a drag;
// the frame loop flushes it once no widget is active (i.e. on release).
static bool g_prefs_dirty = false;

static void prefs_save() {
    json j;
    j["goo_max_m3"] = g_prefs.goo_max_m3;
    j["fuel_filter"] = g_prefs.fuel_filter;
    j["warn_filter"] = g_prefs.warn_filter;
    j["warn_mask"] = g_prefs.warn_mask;
    auto put_type = [&](const char* key, std::map<std::string, FuelPref>& m) {
        for (auto& kv : m)
            j[key][kv.first] = {{"alert", kv.second.alert}, {"cap", kv.second.cap}};
    };
    auto put_sid = [&](const char* key, std::map<long long, FuelPref>& m) {
        for (auto& kv : m)
            j[key][std::to_string(kv.first)] = {{"alert", kv.second.alert},
                                                {"cap", kv.second.cap}};
    };
    put_type("type_fuel", g_prefs.type_fuel);
    put_type("type_gas", g_prefs.type_gas);
    put_sid("sid_fuel", g_prefs.sid_fuel);
    put_sid("sid_gas", g_prefs.sid_gas);
    j["hidden"] = json::array();
    for (auto sid : g_prefs.hidden) j["hidden"].push_back(sid);
    standalone::save_json_file(standalone::config_dir() / "prefs.json", j);
}

static void prefs_load() {
    json j = standalone::load_json_file(standalone::config_dir() / "prefs.json");
    if (!j.is_object()) return;
    g_prefs.goo_max_m3 = j.value("goo_max_m3", g_prefs.goo_max_m3);
    g_prefs.fuel_filter = j.value("fuel_filter", g_prefs.fuel_filter);
    g_prefs.warn_filter = j.value("warn_filter", g_prefs.warn_filter) ? 1 : 0;
    g_prefs.warn_mask = j.value("warn_mask", g_prefs.warn_mask);
    auto get_pref = [](const json& v) {
        FuelPref p;
        p.alert = v.value("alert", p.alert);
        p.cap = v.value("cap", p.cap);
        if (p.alert > 14) p.alert = 14;  // alert slider range is 0..14d
        if (p.cap < 1) p.cap = 1;
        return p;
    };
    if (j.contains("type_fuel"))
        for (auto& [k, v] : j["type_fuel"].items()) g_prefs.type_fuel[k] = get_pref(v);
    if (j.contains("type_gas"))
        for (auto& [k, v] : j["type_gas"].items()) g_prefs.type_gas[k] = get_pref(v);
    if (j.contains("sid_fuel"))
        for (auto& [k, v] : j["sid_fuel"].items())
            g_prefs.sid_fuel[std::atoll(k.c_str())] = get_pref(v);
    if (j.contains("sid_gas"))
        for (auto& [k, v] : j["sid_gas"].items())
            g_prefs.sid_gas[std::atoll(k.c_str())] = get_pref(v);
    if (j.contains("hidden") && j["hidden"].is_array())
        for (auto& v : j["hidden"])
            if (v.is_number()) g_prefs.hidden.insert(v.get<long long>());
}

// effective levels: structure override > type setting > defaults
static FuelPref pref_fuel(const Row& r) {
    auto s = g_prefs.sid_fuel.find(r.sid);
    if (s != g_prefs.sid_fuel.end()) return s->second;
    auto t = g_prefs.type_fuel.find(r.type);
    if (t != g_prefs.type_fuel.end()) return t->second;
    return FuelPref{};
}
static FuelPref pref_gas(const Row& r) {
    auto s = g_prefs.sid_gas.find(r.sid);
    if (s != g_prefs.sid_gas.end()) return s->second;
    auto t = g_prefs.type_gas.find(r.type);
    if (t != g_prefs.type_gas.end()) return t->second;
    return FuelPref{};
}

// blocks needed to reach the cap; rate-based, falling back to the server's
// 30d figure when the burn rate is unknown (only exact at cap 30 then)
static double need_fuel_units(const Row& r, float cap) {
    if (r.bpd > 0 && r.has_fuel) return std::max(0.0, std::round((cap - r.days) * r.bpd));
    if ((int)cap == 30) return r.need;
    return -1;
}
static double need_gas_units(const Row& r, float cap) {
    if (r.gas_day <= 0 || r.fuel2 < 0) return -1;
    return std::max(0.0, std::round((cap - fuel2_days(r)) * r.gas_day));
}
static std::string need_str(double units) {
    return units < 0 ? "--" : units == 0 ? "ok" : commas(units);
}
// haul labels inside the bars read "[units] to [cap]d"
static std::string to_cap(const std::string& s, float cap) {
    if (s.empty() || s == "ok" || s == "--" || s == "?") return s;
    char b[16];
    std::snprintf(b, sizeof b, " to %.0fd", cap);
    return s + b;
}

// every warning the row currently shows, as a mask (mirrors the badge logic
// in the table renderer; feeds the warn filter)
static unsigned row_flags(const Row& r, const std::set<long long>& attacked, bool extr_ok) {
    if (r.is_skyhook) {
        // a theft window announced (or open) is the only skyhook alarm
        std::string tt;
        ImU32 tc;
        bool open = false;
        skyhook_timer_info(r, tt, tc, open);
        return (open || tt != "none") ? (unsigned)WF_SKYWIN : 0u;
    }
    unsigned fl = 0;
    time_t nowt = time(nullptr);
    time_t ua = r.unanchors_at.empty() ? 0 : parse_iso(r.unanchors_at);
    bool limbo = r.state == "anchoring" || r.state == "anchor_vulnerable" ||
                 r.state == "deploy_vulnerable" || r.state == "fitting_invulnerable" ||
                 r.state == "onlining_vulnerable" || r.state == "unanchored";
    if (attacked.count(r.sid) || r.state == "armor_reinforce" ||
        r.state == "hull_reinforce" || r.state == "armor_vulnerable" ||
        r.state == "hull_vulnerable" || r.state == "reinforced")
        fl |= WF_ATTACK;
    if (r.is_pos) {
        // towers have their own power rules: offline = deliberately dark (no
        // burn, no clock), online with an empty bay = the real emergency;
        // onlining/unanchoring/reinforced carry state badges instead
        if (r.state == "offline") fl |= WF_OFFLINE;
        else if (r.state == "online" && r.has_fuel && r.days <= 0) fl |= WF_LOWPOWER;
        else if (r.state == "online" && r.has_fuel && r.days < 7) fl |= WF_LOWFUEL;
    } else if (!limbo) {
        if (r.has_fuel && r.days <= -7) fl |= WF_ABANDONED;
        else if (!r.has_fuel || r.days <= 0) fl |= WF_LOWPOWER;
        else if (r.sv_on == 0 && r.sv_off > 0) fl |= WF_OFFLINE;
        else if (r.days < 7) fl |= WF_LOWFUEL;
    }
    if (r.state == "anchoring" || r.state == "anchor_vulnerable" ||
        r.state == "deploy_vulnerable" || r.state == "fitting_invulnerable" ||
        r.state == "onlining_vulnerable")
        fl |= WF_ANCHORING;
    if (r.state == "unanchored" || ua > 0) fl |= WF_UNANCHORING;
    {
        std::string tt;
        ImU32 tc;
        timer_info(r, tt, tc);
        if (tt != "NONE" && tt != "?") fl |= WF_TIMER;  // "NONE" is timer_info's idle text
    }
    if (has_moon_pull(r.type)) {
        std::string mt;
        ImU32 mc;
        moonpull_info(r, extr_ok, mt, mc);
        if (mt.rfind("READY", 0) == 0 || mt == "POPPED" || mt == "RESET")
            fl |= WF_MOONPULL;
    }
    (void)nowt;
    return fl;
}

// toolbar toggle: a real button with a bevel - popped out when off, pressed
// in (dark well, sunken shadow, lit text) when on
static bool toggle_btn(const char* label, bool on) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          on ? IM_COL32(16, 18, 28, 255) : IM_COL32(56, 58, 74, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          on ? IM_COL32(22, 26, 38, 255) : IM_COL32(68, 70, 88, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(14, 16, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          on ? IM_COL32(0, 230, 255, 255) : IM_COL32(200, 206, 222, 255));
    bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleColor(4);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 lite = IM_COL32(255, 255, 255, 42), dark = IM_COL32(0, 0, 0, 170);
    // bevel: light top/left = raised, dark top/left = sunken
    dl->AddLine(ImVec2(mn.x, mn.y), ImVec2(mx.x - 1, mn.y), on ? dark : lite);
    dl->AddLine(ImVec2(mn.x, mn.y), ImVec2(mn.x, mx.y - 1), on ? dark : lite);
    dl->AddLine(ImVec2(mn.x, mx.y - 1), ImVec2(mx.x - 1, mx.y - 1), on ? lite : dark);
    dl->AddLine(ImVec2(mx.x - 1, mn.y), ImVec2(mx.x - 1, mx.y - 1), on ? lite : dark);
    return clicked;
}

// slider + digital dial readout: a real grab to drag, and beside it the value
// in a dark well with glowing digits - click it and type to set exactly
static bool dial_slider(const char* id, float* v, float mn, float mx, const char* fmt,
                        float dial_w = 64.0f) {
    bool ch = false;
    ImGui::PushID(id);
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(40.0f, avail - dial_w - 6.0f));
    ch |= ImGui::SliderFloat("##s", v, mn, mx, "");
    {
        // lit track: a thin cyan strip running from the left edge to the grab
        ImVec2 smn = ImGui::GetItemRectMin(), smx = ImGui::GetItemRectMax();
        float t = (mx > mn) ? (*v - mn) / (mx - mn) : 0.0f;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float gw = ImGui::GetStyle().GrabMinSize;
        float gx = smn.x + 3 + gw * 0.5f + t * (smx.x - smn.x - 6 - gw);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(smn.x + 3, smx.y - 4),
                                                  ImVec2(gx, smx.y - 2),
                                                  IM_COL32(0, 200, 225, 140), 1.0f);
    }
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(dial_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(4, 8, 10, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(8, 16, 20, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(10, 22, 28, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 230, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 110, 125, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ch |= ImGui::InputFloat("##v", v, 0.0f, 0.0f, fmt);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    ImGui::PopID();
    if (*v < mn) *v = mn;
    if (*v > mx) *v = mx;
    return ch;
}

// both meters stacked in one cell: fuel blocks on top, gas/oz below
static void dual_gauge(const Row& r, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = gauge_cell_h(1);  // one strip; keep in lockstep with the row height
    bool has2 = !r.fuel2_name.empty();
    char b[32];
    std::snprintf(b, sizeof b, "%.1fd", r.days);
    ImTextureID ic2 = r.gas_day > 0 ? g_ic_gas : g_ic_ozone;
    if (r.is_skyhook) {
        // bar 1 = raidable surplus bay (what a thief gets, so fuller = redder),
        // bar 2 = reserve hold, bar 3 = NA (skyhooks burn nothing)
        ImVec2 p2(p.x, p.y + h + 2), p3(p.x, p.y + 2 * (h + 2));
        if (r.sky_unsec_m3 >= 0) {
            // live owner data, or the accrual estimate since the last raid
            // ("~" marks the estimate)
            const char* tilde = r.sky_est ? "~" : "";
            char pc[32];
            std::snprintf(pc, sizeof pc, "%s%.0f%%", tilde,
                          100.0 * std::min(1.0, r.sky_unsec_m3 / SKY_RAIDABLE_M3));
            mini_bar(dl, p, w, h, r.sky_unsec_m3 / SKY_RAIDABLE_M3, pc,
                     tilde + isk_compact(r.sky_unsec_isk < 0 ? 0 : r.sky_unsec_isk) + " isk",
                     false, g_ic_gas, true);
        } else {
            mini_bar(dl, p, w, h, -1, "NA", "", false, 0);
        }
        // second strip: the unraided-streak meter. 0..10 runs green (fresh)
        // to red (full); past 10 the bar catches fire, worsening to 20.
        if (r.sky_streak >= 0) {
            double f = std::min(1.0, r.sky_streak / 10.0);
            char sl[32];
            std::snprintf(sl, sizeof sl, "unraided x%d", r.sky_streak);
            mini_bar(dl, p2, w, h, f, sl, "", false, 0, true);
            if (r.sky_streak > 10) {
                draw_fire(dl, p2, w,
                          (float)std::min(1.0, (r.sky_streak - 10) / 10.0),
                          (unsigned)r.sid);
                g_flash_active = true;  // keep the flames animating
            }
        } else {
            mini_bar(dl, p2, w, h, -1, "?", "", false, 0);
        }
        mini_bar(dl, p3, w, h, -1, "NA", "", false, 0);
        ImGui::Dummy(ImVec2(w, gauge_cell_h(3)));
        return;
    }
    float fcap = pref_fuel(r).cap;
    if (!r.has_fuel && r.is_pos && r.blocks_now >= 0)
        // parked tower: no clock, but the bay contents are real - show them
        mini_bar(dl, p, w, h, -1, commas(r.blocks_now) + " blk stored", "", false,
                 g_ic_fuel);
    else if (!r.has_fuel)
        mini_bar(dl, p, w, h, -1, "--", "", false, g_ic_fuel);
    else
        mini_bar(dl, p, w, h, r.days / fcap, b,
                 to_cap(need_str(need_fuel_units(r, fcap)), fcap), false, g_ic_fuel);
    ImVec2 p2(p.x, p.y + h + 2);
    if (r.is_pos) {
        // POS second strip: the strontium bay, as hours of reinforcement it
        // buys, metered against the longest timer a tower can hold (~41.7h)
        if (r.stront_hours >= 0) {
            char sl[32];
            std::snprintf(sl, sizeof sl, "%.1fh", r.stront_hours);
            mini_bar(dl, p2, w, h, r.stront_hours / POS_STRONT_MAX_H, sl,
                     commas(r.stront) + " stront", true, 0);
        } else {
            mini_bar(dl, p2, w, h, -1, "?", "", true, 0);
        }
    } else if (!has2) {  // type has no secondary fuel: hatched NA placeholder
        mini_bar(dl, p2, w, h, -1, "NA", "", true, 0);
    } else if (r.fuel2 < 0) {
        mini_bar(dl, p2, w, h, -1, "?", "", true, ic2);
    } else if (r.gas_day > 0) {
        float gcap = pref_gas(r).cap;
        char g[32];
        std::snprintf(g, sizeof g, "%.1fd", fuel2_days(r));
        mini_bar(dl, p2, w, h, fuel2_days(r) / gcap, g,
                 to_cap(need_str(need_gas_units(r, gcap)), gcap), true, ic2);
    } else {
        mini_bar(dl, p2, w, h, r.lo_target > 0 ? r.fuel2 / r.lo_target : -1,
                 compact_units(r.fuel2), to30(gas30_raw(r)), true, ic2);
    }
    ImVec2 p3(p.x, p.y + 2 * (h + 2));
    if (r.goo_cap > 0) {
        // Metenox moongoo bay: flat m3 stored + haul value, solid teal fill
        // against the user's "bar max m3" setting (settings window)
        double gf = r.goo_m3 >= 0
                        ? std::min(1.0, r.goo_m3 / std::max(1.0f, g_prefs.goo_max_m3))
                        : -1;
        std::string gm3 = r.goo_m3 >= 0 ? commas(r.goo_m3) + " m3" : "?";
        mini_bar(dl, p3, w, h, gf, gm3,
                 isk_compact(r.goo_isk < 0 ? 0 : r.goo_isk) + " isk", false, g_ic_goo,
                 false, IM_COL32(0, 130, 148, 255));
    } else if (r.type == "Metenox") {  // bay exists but this data source can't see it
        mini_bar(dl, p3, w, h, -1, "?", "", false, g_ic_goo);
    } else {  // no moon material bay on this type: hatched NA placeholder
        mini_bar(dl, p3, w, h, -1, "NA", "", false, 0);
    }
    // profitability alarm: the whole fuel panel pulses red when the drill
    // runs at a loss, yellow when the monthly net falls under 100m
    if (r.has_econ && r.econ_net < 100e6) {
        ImU32 warn = r.econ_net < 0 ? IM_COL32(255, 70, 70, 0)
                                    : IM_COL32(250, 215, 70, 0);
        g_flash_active = true;
        int a = (int)(70 + 150 * (0.5 + 0.5 * std::sin(ImGui::GetTime() * 5.0)));
        float ch = gauge_cell_h(3);
        dl->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + w + 2, p.y + ch + 1),
                    warn | ((ImU32)a << 24), 4.0f, 0, 2.5f);
    }
    ImGui::Dummy(ImVec2(w, gauge_cell_h(3)));  // uniform rows: every type = Metenox height
}

// --- the gauge widget: fill fraction + text inside -----------------------------
static void gauge(const char* id, double frac, const std::string& left,
                  const std::string& right, float w, bool secondary = false,
                  ImTextureID icon = 0, bool invert = false, ImU32 solid = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight() + 4;
    ImU32 col = kGreyU32;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(30, 32, 44, 255), 3.0f);
    if (frac >= 0) {
        float f = frac > 1 ? 1.f : (float)frac;
        col = solid ? solid : ramp_u32(invert ? 1.0f - f : f, secondary);
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
    bool f0 = false;
    if (!g_busy.compare_exchange_strong(f0, true)) return;   // atomic claim, no TOCTOU
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

// the SSO login flow; the caller has already claimed g_busy
static void run_login_now() {
    std::string err, name;
    bool ok = standalone::login(g_client_id, err, &name, [](const std::string& s) {
        std::lock_guard<std::mutex> l(g_mtx);
        g_status = s;
        if (g_gui_wake) g_gui_wake();
    });
    {
        std::lock_guard<std::mutex> l(g_mtx);
        // a cancelled login already announced its successor; stay quiet
        if (ok || err != "cancelled") {
            g_note = ok ? "added " + name : "login failed: " + err;
            g_note_at = time(nullptr);
        }
        g_status.clear();
    }
    if (ok) standalone_cycle();
    g_busy = false;
    if (g_gui_wake) g_gui_wake();
}

static void act_add_character() {
    if (!g_standalone) return;
    bool f = false;
    if (g_busy.compare_exchange_strong(f, true)) {
        spawn_bg(run_login_now);
        return;
    }
    if (standalone::login_pending()) {
        // an abandoned browser login (tab closed, nothing picked) is holding
        // the port: cancel it and start a fresh one the moment it lets go
        static std::atomic<bool> s_takeover{false};
        if (s_takeover.exchange(true)) return;  // restart already queued
        {
            std::lock_guard<std::mutex> l(g_mtx);
            g_note = "previous login cancelled - opening a fresh EVE login...";
            g_note_at = time(nullptr);
            if (g_gui_wake) g_gui_wake();
        }
        standalone::cancel_pending_login();
        spawn_bg([]() {
            for (int i = 0; i < 100 && g_run; i++) {  // old listener exits within ~1s
                bool ff = false;
                if (g_busy.compare_exchange_strong(ff, true)) {
                    s_takeover = false;
                    run_login_now();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            s_takeover = false;
            std::lock_guard<std::mutex> l(g_mtx);
            g_note = "could not take over the login - try again";
            g_note_at = time(nullptr);
            if (g_gui_wake) g_gui_wake();
        });
        return;
    }
    // a data refresh (not a login) holds the busy flag; those finish on their own
    std::lock_guard<std::mutex> l(g_mtx);
    g_note = "still busy with a data refresh - try again in a few seconds";
    g_note_at = time(nullptr);
    if (g_gui_wake) g_gui_wake();
}

static void act_claim(long long sid) {
    if (sid == 0 || g_standalone) return;
    bool f0 = false;
    if (!g_busy.compare_exchange_strong(f0, true)) return;   // atomic claim, no TOCTOU
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

// on-demand re-check (startup checks once; long-running sessions can ask again)
static void act_check_update() {
    if (g_busy) return;
    spawn_bg([]() {
        check_update();
        std::lock_guard<std::mutex> l(g_mtx);
        if (g_update_tag.empty()) {
            g_note = std::string("up to date (") + STOKER_VERSION + ")";
            g_note_at = time(nullptr);
        }
        if (g_gui_wake) g_gui_wake();
    });
}

static void act_update(const std::string& tag, const std::string& url,
                       const std::string& sig_url) {
    bool f0 = false;
    if (!g_busy.compare_exchange_strong(f0, true)) return;   // atomic claim, no TOCTOU
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_status = "downloading " + tag + "...";
    }
    spawn_bg([tag, url, sig_url]() {
        std::string res = apply_update(tag, url, sig_url);
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

// --- custom window chrome --------------------------------------------------------
// The OS decorations are gone (GLFW_DECORATED false): STOKER draws its own Hot
// Neon title bar (drag strip, min/max/close) and a cyan border, and implements
// move + edge-resize by hand. Wayland forbids programmatic window moves, so it
// keeps native decorations there; config "native_titlebar": true opts out too.
static bool g_chrome = false;
static bool g_win_maxed = false;
static bool g_win_pinned = false;  // keep-on-top: floats over the EVE client
static int g_win_rest[4];  // windowed pos+size to restore from maximize

static const float CHROME_H = 30.0f;  // title-bar strip height
static const float CHROME_BTN = 46.0f;

static void chrome_toggle_max(GLFWwindow* win) {
    if (g_win_maxed) {
        // size FIRST: a window exactly filling the work area reads as
        // "maximized" to some WMs (mutter), which ignore moves until the
        // size shrinks away from that state
        glfwSetWindowSize(win, g_win_rest[2], g_win_rest[3]);
        glfwSetWindowPos(win, g_win_rest[0], g_win_rest[1]);
        g_win_maxed = false;
        return;
    }
    glfwGetWindowPos(win, &g_win_rest[0], &g_win_rest[1]);
    glfwGetWindowSize(win, &g_win_rest[2], &g_win_rest[3]);
    // maximize = fill the work area of the monitor holding the window centre
    // (manual, so the taskbar/dock is respected on every platform)
    int cx = g_win_rest[0] + g_win_rest[2] / 2, cy = g_win_rest[1] + g_win_rest[3] / 2;
    int n = 0, wx = 0, wy = 0, ww = 0, wh = 0;
    GLFWmonitor** mons = glfwGetMonitors(&n);
    for (int i = 0; i < n; i++) {
        int mx, my, mw, mh;
        glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
        if (i == 0 || (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh)) {
            wx = mx;
            wy = my;
            ww = mw;
            wh = mh;
            if (i > 0) break;  // exact hit beats the primary fallback
        }
    }
    if (ww > 0) {
        glfwSetWindowPos(win, wx, wy);
        glfwSetWindowSize(win, ww, wh);
        g_win_maxed = true;
    }
}

// "STOKER" as chunky 5x7 pixel letters with a pink->cyan sweep and a soft
// glow; the embedded font is a plain mono, so the wordmark is drawn, not set
static float chrome_wordmark(ImDrawList* dl, float x, float y) {
    static const unsigned char L[6][7] = {
        {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // S
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // T
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // O
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},  // K
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // E
        {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
    };
    const float px = 2.0f;
    for (int li = 0; li < 6; li++) {
        float f = li / 5.0f;  // pink -> cyan across the word
        ImU32 c = IM_COL32((int)(255 * (1 - f)), (int)(43 + (230 - 43) * f),
                           (int)(214 + (255 - 214) * f), 255);
        ImU32 glow = (c & 0xFFFFFF) | (46u << 24);
        float lx = x + li * (5 * px + 4);
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (L[li][row] & (0x10 >> col)) {
                    float qx = lx + col * px, qy = y + row * px;
                    dl->AddRectFilled(ImVec2(qx - 1, qy - 1), ImVec2(qx + px + 1, qy + px + 1),
                                      glow);
                    dl->AddRectFilled(ImVec2(qx, qy), ImVec2(qx + px, qy + px), c);
                }
    }
    return x + 6 * (5 * px + 4);
}

static ImGuiMouseCursor chrome_cursor_for(int m) {
    bool l = m & 1, r = m & 2, t = m & 4, b = m & 8;
    if ((l && t) || (r && b)) return ImGuiMouseCursor_ResizeNWSE;
    if ((r && t) || (l && b)) return ImGuiMouseCursor_ResizeNESW;
    if (l || r) return ImGuiMouseCursor_ResizeEW;
    return ImGuiMouseCursor_ResizeNS;
}

// title bar + border + resize. Called once per frame right after the main
// window Begin; returns the strip height so content starts below it.
static float chrome_begin(GLFWwindow* win) {
    if (!g_chrome) return 0;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float W = io.DisplaySize.x;

    // strip + brand
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, CHROME_H), IM_COL32(8, 8, 14, 255));
    dl->AddLine(ImVec2(0, CHROME_H - 1), ImVec2(W, CHROME_H - 1), IM_COL32(0, 110, 125, 255));
    dl->AddRectFilled(ImVec2(10, 7), ImVec2(14, CHROME_H - 7), IM_COL32(255, 43, 214, 255));
    float wm_end = chrome_wordmark(dl, 20, 8);
    dl->AddText(ImVec2(wm_end + 10, 6), IM_COL32(128, 136, 150, 255), STOKER_VERSION);

    // buttons: pin (keep on top) / minimize / maximize / close
    const char* ids[4] = {"##wpin", "##wmin", "##wmax", "##wclose"};
    for (int i = 0; i < 4; i++) {
        float bx = W - CHROME_BTN * (4 - i);
        ImGui::SetCursorScreenPos(ImVec2(bx, 0));
        ImGui::InvisibleButton(ids[i], ImVec2(CHROME_BTN, CHROME_H));
        bool hov = ImGui::IsItemHovered();
        if (hov)
            dl->AddRectFilled(ImVec2(bx, 0), ImVec2(bx + CHROME_BTN, CHROME_H),
                              i == 3 ? IM_COL32(200, 30, 55, 220) : IM_COL32(255, 255, 255, 22));
        ImU32 gc = (hov && i == 3) ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 206, 222, 255);
        float cx = bx + CHROME_BTN / 2, cy = CHROME_H / 2;
        if (i == 0) {
            // thumbtack: head + stem + point; cyan when pinned
            ImU32 pc = g_win_pinned ? IM_COL32(0, 230, 255, 255) : gc;
            if (g_win_pinned)
                dl->AddRectFilled(ImVec2(bx, 0), ImVec2(bx + CHROME_BTN, CHROME_H),
                                  IM_COL32(0, 140, 160, 40));
            dl->AddCircleFilled(ImVec2(cx + 2, cy - 3), 3.5f, pc);
            dl->AddLine(ImVec2(cx + 2, cy), ImVec2(cx + 2, cy + 2), pc, 2.0f);
            dl->AddLine(ImVec2(cx - 2, cy + 6), ImVec2(cx + 1, cy + 3), pc, 1.0f);
            if (hov)
                ImGui::SetTooltip(g_win_pinned ? "release from top" : "keep on top of EVE");
        } else if (i == 1) {
            dl->AddLine(ImVec2(cx - 5, cy + 3), ImVec2(cx + 5, cy + 3), gc, 1.0f);
        } else if (i == 2) {
            if (g_win_maxed) {  // restore: two offset squares
                dl->AddRect(ImVec2(cx - 2, cy - 5), ImVec2(cx + 5, cy + 2), gc);
                dl->AddRectFilled(ImVec2(cx - 6, cy - 2), ImVec2(cx + 2, cy + 6),
                                  IM_COL32(8, 8, 14, 255));
                dl->AddRect(ImVec2(cx - 5, cy - 2), ImVec2(cx + 2, cy + 5), gc);
            } else {
                dl->AddRect(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), gc);
            }
        } else {
            dl->AddLine(ImVec2(cx - 5, cy - 5), ImVec2(cx + 5, cy + 5), gc, 1.0f);
            dl->AddLine(ImVec2(cx - 5, cy + 5), ImVec2(cx + 5, cy - 5), gc, 1.0f);
        }
        if (ImGui::IsItemClicked()) {
            if (i == 0) {
                g_win_pinned = !g_win_pinned;
                glfwSetWindowAttrib(win, GLFW_FLOATING, g_win_pinned ? GLFW_TRUE : GLFW_FALSE);
            } else if (i == 1) {
                glfwIconifyWindow(win);
            } else if (i == 2) {
                chrome_toggle_max(win);
            } else {
                glfwSetWindowShouldClose(win, 1);
            }
        }
    }

    // drag strip: everything left of the buttons, below the 6px top resize zone
    static bool dragging = false;
    static double grab_x = 0, grab_y = 0;
    ImGui::SetCursorScreenPos(ImVec2(0, 6));
    ImGui::InvisibleButton("##wdrag", ImVec2(std::max(1.0f, W - CHROME_BTN * 4), CHROME_H - 6));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        dragging = false;
        chrome_toggle_max(win);
    } else {
        if (ImGui::IsItemActivated()) {
            if (g_win_maxed) {
                // dragging a maximized window: restore it under the cursor at
                // the same horizontal ratio, like the native titlebar does
                double cx2, cy2;
                glfwGetCursorPos(win, &cx2, &cy2);
                int wx, wy;
                glfwGetWindowPos(win, &wx, &wy);
                double sx = wx + cx2, sy = wy + cy2;  // screen cursor
                double ratio = W > 0 ? cx2 / W : 0.5;
                chrome_toggle_max(win);
                glfwSetWindowPos(win, (int)(sx - ratio * g_win_rest[2]), (int)(sy - 15));
            }
            glfwGetCursorPos(win, &grab_x, &grab_y);
            dragging = true;
        }
        if (dragging && ImGui::IsItemActive()) {
            double cx2, cy2;
            glfwGetCursorPos(win, &cx2, &cy2);
            int dx = (int)(cx2 - grab_x), dy = (int)(cy2 - grab_y);
            if (dx || dy) {
                int wx, wy;
                glfwGetWindowPos(win, &wx, &wy);
                glfwSetWindowPos(win, wx + dx, wy + dy);
            }
        } else if (!ImGui::IsItemActive()) {
            dragging = false;
        }
    }

    // version doubles as the update check; submitted after the drag strip so
    // it wins the overlapping hover
    ImGui::SetCursorScreenPos(ImVec2(wm_end + 8, 2));
    if (ImGui::InvisibleButton("##wver", ImVec2(64, CHROME_H - 4))) act_check_update();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("check for updates");

    // neon border (foreground list renders over everything)
    ImGui::GetForegroundDrawList()->AddRect(ImVec2(0, 0), io.DisplaySize,
                                            IM_COL32(0, 140, 160, 255));

    // edge-resize state machine (raw io: the 6px zones live in the window
    // padding where no widgets are; a started drag owns the gesture)
    if (!g_win_maxed) {
        static bool rz = false;
        static int rz_edges = 0, rz_x, rz_y, rz_w, rz_h;
        static double rz_cx, rz_cy;  // screen cursor at drag start
        const float B = 6;
        if (!rz) {
            int m = 0;
            ImVec2 mp = io.MousePos;
            if (mp.x >= 0 && mp.y >= 0 && mp.x < W && mp.y < io.DisplaySize.y) {
                if (mp.x < B) m |= 1;
                if (mp.x >= W - B) m |= 2;
                if (mp.y < B) m |= 4;
                if (mp.y >= io.DisplaySize.y - B) m |= 8;
            }
            // the window buttons own the top-right corner
            if ((m & 4) && mp.x > W - CHROME_BTN * 4) m = 0;
            if (m && !ImGui::IsAnyItemActive() && !ImGui::IsAnyItemHovered()) {
                ImGui::SetMouseCursor(chrome_cursor_for(m));
                if (ImGui::IsMouseClicked(0)) {
                    rz = true;
                    rz_edges = m;
                    glfwGetWindowPos(win, &rz_x, &rz_y);
                    glfwGetWindowSize(win, &rz_w, &rz_h);
                    double cx2, cy2;
                    glfwGetCursorPos(win, &cx2, &cy2);
                    rz_cx = rz_x + cx2;
                    rz_cy = rz_y + cy2;
                }
            }
        } else {
            ImGui::SetMouseCursor(chrome_cursor_for(rz_edges));
            if (!ImGui::IsMouseDown(0)) {
                rz = false;
            } else {
                int wx, wy;
                glfwGetWindowPos(win, &wx, &wy);
                double cx2, cy2;
                glfwGetCursorPos(win, &cx2, &cy2);
                double dx = (wx + cx2) - rz_cx, dy = (wy + cy2) - rz_cy;
                const int MINW = 780, MINH = 480;
                int nx = rz_x, ny = rz_y, nw = rz_w, nh = rz_h;
                if (rz_edges & 1) {
                    nw = std::max(MINW, (int)(rz_w - dx));
                    nx = rz_x + (rz_w - nw);
                }
                if (rz_edges & 2) nw = std::max(MINW, (int)(rz_w + dx));
                if (rz_edges & 4) {
                    nh = std::max(MINH, (int)(rz_h - dy));
                    ny = rz_y + (rz_h - nh);
                }
                if (rz_edges & 8) nh = std::max(MINH, (int)(rz_h + dy));
                if (nx != wx || ny != wy) glfwSetWindowPos(win, nx, ny);
                glfwSetWindowSize(win, nw, nh);
            }
        }
    }
    return CHROME_H;
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
    prefs_load();

    if (!glfwInit()) return 1;
    g_chrome = !g_native_titlebar;
#if defined(GLFW_PLATFORM_WAYLAND)
    // Wayland forbids programmatic window moves: keep native decorations there
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) g_chrome = false;
#endif
    if (g_chrome) glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(1280, 820, "STOKER", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwSetWindowSizeLimits(win, 780, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
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
    // slider/button dress-up, adapted from the "enemymouse" neon-cyan style
    // (dear-imgui-styles collection): chunky rounded grabs, translucent cyan
    // that goes hot pink when grabbed, teal-tinted buttons, neon checkmarks
    st.FrameRounding = 3.0f;
    st.GrabRounding = 2.0f;
    st.GrabMinSize = 16.0f;
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.17f, 1);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.13f, 0.22f, 1);
    st.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.15f, 0.26f, 1);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.00f, 0.90f, 1.00f, 0.45f);
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.17f, 0.84f, 0.90f);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(0.00f, 0.90f, 1.00f, 0.80f);
    st.Colors[ImGuiCol_Button] = ImVec4(0.00f, 0.35f, 0.40f, 0.55f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.00f, 0.55f, 0.62f, 0.70f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.00f, 0.75f, 0.85f, 0.85f);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    for (auto& e : kStructIcons)
        g_type_icons[e.type] = make_icon(e.rgba, kStructIconSize, true);
    g_ic_fuel = make_icon(kIcon_fuel);
    g_ic_gas = make_icon(kIcon_gas);
    g_ic_ozone = make_icon(kIcon_ozone);
    g_ic_goo = make_icon(kIcon_goo);

    std::thread th(worker);
    std::thread logs_th(eve_logs_worker);  // SMT-style chat-log watcher
    if (g_standalone && !standalone::have_login()) act_add_character();

    char filter[128] = {0};
    // moon-rental corp/rented filter: starts active; only shown when the
    // rentals feed is online for the current tab
    static bool g_corp_only = true;
    static std::string g_type_tab;  // station-type tab; "" = All
    // ctrl+click (toggle) / shift+click (range) haul tally: the detail panel
    // becomes the hauling plan while anything is selected; skyhooks excluded
    static std::set<long long> g_tally;
    static long long g_tally_anchor = 0;
    long long detail_sid = 0;
    int view_tab = 0;  // 0 detail, 1 refuel log

    while (!glfwWindowShouldClose(win) && g_run) {
        glfwWaitEventsTimeout(g_flash_active ? 0.06 : 0.25);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // heavy snapshots (rows/logs/notifs/tabs) are only re-copied when an
        // ingest bumped the generation - the flash-mode fast loop would
        // otherwise deep-copy the whole dataset 16 times a second; the cheap
        // strings still refresh every frame
        static std::vector<Row> rows;
        static std::vector<Refuel> refuels;
        static std::vector<std::string> tabs;
        static std::vector<Notif> notifs;
        static unsigned long long seen_gen = ~0ull;
        std::string status, note, corpname, upd, updurl, updsig, pulled, esiMod;
        int tabsel;
        bool rentals_online, extractions_ok;
        std::string pos_status;
        {
            std::lock_guard<std::mutex> l(g_mtx);
            if (seen_gen != g_data_gen) {
                seen_gen = g_data_gen;
                rows = g_rows;
                refuels = g_refuels;
                tabs = g_tab_labels;
                notifs = g_notifs;
            }
            tabsel = g_tab;
            rentals_online = g_rentals_online;
            extractions_ok = g_extractions_ok;
            pos_status = g_starbases_status;
            status = g_status;
            corpname = g_corp_name;
            upd = g_update_tag;
            updurl = g_update_url;
            updsig = g_update_sig_url;
            esiMod = g_esi_lastmod;
            if (g_note_at && time(nullptr) - g_note_at < 20) note = g_note;
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        float chrome_h = chrome_begin(win);
        if (chrome_h > 0) ImGui::SetCursorPosY(chrome_h + 6);

        // header row; with the custom chrome the brand + version live in the
        // title bar, so only repeat them when the OS is drawing the window
        if (!g_chrome) {
            ImGui::TextColored(PINK, "STOKER");
            ImGui::SameLine();
            if (ImGui::SmallButton(STOKER_VERSION)) act_check_update();
            ImGui::SetItemTooltip("check for updates");
            ImGui::SameLine();
        }
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
            if (ImGui::SmallButton(("update " + upd).c_str())) act_update(upd, updurl, updsig);
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        static bool show_settings = false;
        float rightw = (g_standalone ? 250.0f : 160.0f) + 80.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - rightw);
        if (ImGui::SmallButton("settings")) show_settings = !show_settings;
        ImGui::SameLine();
        if (ImGui::SmallButton("refresh")) act_refresh();
        if (g_standalone) {
            ImGui::SameLine();
            if (ImGui::SmallButton("+ add character")) act_add_character();
        }

        // settings: goo bar scale, per-type fuel levels, warning-flag set
        if (show_settings) {
            ImGui::SetNextWindowSize(ImVec2(740, 560), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("STOKER settings", &show_settings)) {
                bool ch = false;
                ImGui::TextColored(DIMCYAN, "metenox moon-material bar");
                ImGui::TextColored(GREY_, "bar reads full at");
                ImGui::SameLine();
                {
                    ImGui::PushID("goomax");
                    ImGui::SetNextItemWidth(220);
                    ch |= ImGui::SliderFloat("##s", &g_prefs.goo_max_m3, 1000.0f,
                                             500000.0f, "", ImGuiSliderFlags_Logarithmic);
                    ImGui::SameLine(0, 4);
                    ImGui::SetNextItemWidth(92);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(4, 8, 10, 255));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(8, 16, 20, 255));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(10, 22, 28, 255));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 230, 255, 255));
                    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 110, 125, 255));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ch |= ImGui::InputFloat("##v", &g_prefs.goo_max_m3, 0.0f, 0.0f,
                                            "%.0f m3");
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(5);
                    ImGui::PopID();
                    if (g_prefs.goo_max_m3 < 1000) g_prefs.goo_max_m3 = 1000;
                    if (g_prefs.goo_max_m3 > 500000) g_prefs.goo_max_m3 = 500000;
                }
                ImGui::Separator();
                ImGui::TextColored(DIMCYAN, "fuel levels per structure type (days)");
                ImGui::TextColored(GREY_,
                                   "alert feeds the needs-fuel filter; max sets the bar "
                                   "cap and the \"[units] to [max]d\" haul target");
                std::set<std::string> stypes;
                for (auto& r : rows)
                    if (!r.is_skyhook && !r.type.empty()) stypes.insert(r.type);
                for (auto& kv : g_prefs.type_fuel) stypes.insert(kv.first);
                // fixed column widths: stretch-prop sizing feeds off content
                // width while the sliders size off the column - a feedback loop
                // that visibly "walks" for a few frames after every resize
                if (ImGui::BeginTable("fuelprefs", 5, ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("fuel alert", ImGuiTableColumnFlags_WidthFixed, 128);
                    ImGui::TableSetupColumn("fuel max", ImGuiTableColumnFlags_WidthFixed, 128);
                    ImGui::TableSetupColumn("gas alert", ImGuiTableColumnFlags_WidthFixed, 128);
                    ImGui::TableSetupColumn("gas max", ImGuiTableColumnFlags_WidthFixed, 128);
                    ImGui::TableHeadersRow();
                    for (auto& t : stypes) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextColored(TEXTC, "%s", t.c_str());
                        ImGui::PushID(t.c_str());
                        FuelPref& fp = g_prefs.type_fuel[t];
                        ImGui::TableSetColumnIndex(1);
                        ch |= dial_slider("fa", &fp.alert, 0.0f, 14.0f, "%.0f d", 52.0f);
                        ImGui::TableSetColumnIndex(2);
                        ch |= dial_slider("fc", &fp.cap, 1.0f, 90.0f, "%.0f d", 52.0f);
                        if (t == "Metenox") {  // the only type with a days-based 2nd bar
                            FuelPref& gp = g_prefs.type_gas[t];
                            ImGui::TableSetColumnIndex(3);
                            ch |= dial_slider("ga", &gp.alert, 0.0f, 14.0f, "%.0f d", 52.0f);
                            ImGui::TableSetColumnIndex(4);
                            ch |= dial_slider("gc", &gp.cap, 1.0f, 90.0f, "%.0f d", 52.0f);
                        } else {
                            ImGui::TableSetColumnIndex(3);
                            ImGui::TextColored(GREY_, "-");
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::TextColored(GREY_,
                                   "each structure's detail panel can override these "
                                   "per structure");
                ImGui::Separator();
                ImGui::TextColored(DIMCYAN, "hidden structures");
                if (g_prefs.hidden.empty()) {
                    ImGui::TextColored(GREY_,
                                       "none - hide one from its detail settings popup");
                } else {
                    std::vector<long long> unhide;
                    for (auto sid : g_prefs.hidden) {
                        const Row* hr = nullptr;
                        for (auto& r : rows)
                            if (r.sid == sid) { hr = &r; break; }
                        ImGui::PushID((int)(sid & 0x7fffffff));
                        if (ImGui::SmallButton("unhide")) unhide.push_back(sid);
                        ImGui::PopID();
                        ImGui::SameLine();
                        if (hr)
                            ImGui::TextColored(TEXTC, "%s  (%s)", hr->name.c_str(),
                                               hr->system.c_str());
                        else
                            ImGui::TextColored(GREY_, "structure %lld (another corp tab)",
                                               sid);
                    }
                    if (g_prefs.hidden.size() > 1 && ImGui::SmallButton("unhide all"))
                        g_prefs.hidden.clear(), ch = true;
                    for (auto sid : unhide) {
                        g_prefs.hidden.erase(sid);
                        ch = true;
                    }
                }
                ImGui::Separator();
                ImGui::TextColored(DIMCYAN, "warning flags counted by the flags filter");
                int wi = 0;
                for (auto& wd : kWarnDefs) {
                    if (wi++ % 2) ImGui::SameLine(280.0f);
                    ch |= ImGui::CheckboxFlags(wd.name, &g_prefs.warn_mask, wd.bit);
                }
                if (ch) g_prefs_dirty = true;   // flushed on release, not per drag frame
            }
            ImGui::End();
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
            // autocenter upgrades as better anchors appear: 0 = not yet,
            // 1 = centred on a structure, 2 = centred on the pilot; a corp-tab
            // switch resets so the view follows the corp being looked at
            static int centered = 0;
            static int centered_tab = -1;
            if (centered_tab != tabsel) {
                centered_tab = tabsel;
                centered = 0;
            }
            MapView mv;
            {
                std::lock_guard<std::mutex> l(g_mtx);
                mv = g_map;
            }
            // default view: the whole universe, centred on the pilot's region
            if (!mv.ok && !mv.loading && mv.error.empty()) act_build_universe();
            if (mv.ok && mv.region == "New Eden") {
                int want = !g_pilots.empty() ? 2 : (!rows.empty() ? 1 : 0);
                std::string cs = !g_pilots.empty() ? g_pilots[0].system
                                 : !rows.empty()   ? rows[0].system
                                                   : "";
                if (want > centered && !cs.empty()) {
                    std::lock_guard<std::mutex> l(g_mtx);
                    auto sit = g_sysid.find(lower_(cs));
                    if (sit != g_sysid.end()) {
                        map_center_on_system(g_map, sit->second);
                        centered = want;
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
                centered = 0;  // re-centre once it rebuilds
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
            if (lower_(g_eve_logs_cfg) == "off")
                ImGui::TextColored(GREY_,
                                   "pilot/intel overlay disabled (\"eve_logs\": \"off\" "
                                   "in config.json)");
            else if (g_logs_found < 0)
                ImGui::TextColored(GREY_,
                                   "eve chat logs not found - set \"eve_logs\" to your "
                                   "EVE logs path in config.json");

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
                // eveterm's map renderer, cloned: isotropic zoom-to-cursor, tiny
                // faint dots that grow with zoom, hollow marker rings popping in
                // past 10x, names at the 14x cutoff, dashed cross-region gates,
                // glowing jump-bridge arcs with a travelling light, parallaxed
                // region names. STOKER's own overlays (fuel rings, chat-log
                // intel pulses, pilot rings) ride on top.
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImVec2 sz = ImGui::GetContentRegionAvail();
                ImVec2 mcenter(p0.x + sz.x * 0.5f, p0.y + sz.y * 0.5f);
                float mbase = std::max(40.0f, std::min(sz.x, sz.y));
                ImGui::InvisibleButton("mapcanvas", sz);
                bool hovered = ImGui::IsItemHovered();
                bool mactive = ImGui::IsItemActive();
                auto& io2 = ImGui::GetIO();
                {
                    std::lock_guard<std::mutex> l(g_mtx);
                    MapView* live = &g_map;
                    float scale0 = live->zoom * mbase;
                    if (hovered && io2.MouseWheel != 0) {
                        // zoom toward the cursor: the system under the mouse stays put
                        ImVec2 mpos = io2.MousePos;
                        float bx = live->pan.x + (mpos.x - mcenter.x) / scale0;
                        float by = live->pan.y + (mpos.y - mcenter.y) / scale0;
                        live->zoom = std::clamp(
                            live->zoom * (io2.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 0.3f,
                            60.0f);
                        scale0 = live->zoom * mbase;
                        live->pan.x = bx - (mpos.x - mcenter.x) / scale0;
                        live->pan.y = by - (mpos.y - mcenter.y) / scale0;
                    }
                    if (mactive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                        live->pan.x -= io2.MouseDelta.x / scale0;
                        live->pan.y -= io2.MouseDelta.y / scale0;
                    }
                    mv = *live;
                }
                const float scale = mv.zoom * mbase;
                auto at2 = [&](double nx, double ny) {
                    return ImVec2(mcenter.x + (float)(nx - mv.pan.x) * scale,
                                  mcenter.y + (float)(ny - mv.pan.y) * scale);
                };
                auto at = [&](const MapNode& n) { return at2(n.nx, n.ny); };
                dl->PushClipRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), true);
                auto inView = [&](const ImVec2& s) {
                    return s.x >= p0.x - 30 && s.x <= p0.x + sz.x + 30 && s.y >= p0.y - 30 &&
                           s.y <= p0.y + sz.y + 30;
                };
                auto dashed = [&](ImVec2 a, ImVec2 b, ImU32 col, float th) {
                    float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
                    if (len < 1.0f) return;
                    float ux = dx / len, uy = dy / len;
                    for (float t = 0.0f; t < len; t += 9.0f) {
                        float t2 = std::min(t + 5.0f, len);
                        dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t),
                                    ImVec2(a.x + ux * t2, a.y + uy * t2), col, th);
                    }
                };
                const ImU32 kGate = IM_COL32(70, 74, 92, 255);
                for (auto& g : mv.gates) {
                    const MapNode& na = mv.nodes[g.first];
                    const MapNode& nb = mv.nodes[g.second];
                    ImVec2 a = at(na), b = at(nb);
                    if (!inView(a) && !inView(b)) continue;
                    if (na.region && nb.region && na.region != nb.region)
                        dashed(a, b, kGate, 1.6f);  // cross-region jumps read as stitches
                    else
                        dl->AddLine(a, b, kGate, 1.6f);
                }
                // region names: solid when zoomed out, gone by 10x, drifting
                // slightly on zoom (parallax) exactly like eveterm
                if (mv.region == "New Eden") {
                    float rfs = std::clamp(13.0f + mv.zoom * 1.6f, 15.0f, 34.0f);
                    int ra = (int)(mv.zoom <= 4.0f
                                       ? 240.0f
                                       : std::clamp(240.0f * (10.0f - mv.zoom) / 6.0f, 0.0f,
                                                    240.0f));
                    if (ra > 2) {
                        ImU32 rcol = IM_COL32(208, 218, 240, ra);
                        ImFont* rfont = ImGui::GetFont();
                        for (auto& rl : g_region_labels) {
                            ImVec2 raw = at2(rl.nx, rl.ny);
                            ImVec2 s(raw.x - 0.05f * (float)(rl.nx - 0.5) * scale,
                                     raw.y - 0.05f * (float)(rl.ny - 0.5) * scale);
                            if (!inView(s)) continue;
                            float tw = rfont->CalcTextSizeA(rfs, 1e30f, 0.0f, rl.name.c_str()).x;
                            dl->AddText(rfont, rfs, ImVec2(s.x - tw * 0.5f, s.y - rfs * 0.5f),
                                        rcol, rl.name.c_str());
                        }
                    }
                }
                // dot sizing: tiny + faint zoomed out, stepping up to full
                // markers at the name cutoff (eveterm's numbers verbatim)
                const float kCutoff = 14.0f;
                const float kLabelFs = 19.0f;
                bool showNames = mv.region != "New Eden" || mv.zoom >= kCutoff;
                float dotR;
                int dotAlpha;
                if (mv.zoom < kCutoff) {
                    float t = std::clamp(mv.zoom / kCutoff, 0.0f, 1.0f);
                    dotR = 0.8f + t * 1.3f;
                    dotAlpha = (int)(85.0f + t * 120.0f);
                } else {
                    dotR = 14.0f;
                    float t = std::clamp((mv.zoom - kCutoff) / 2.0f, 0.0f, 1.0f);
                    dotAlpha = (int)(170.0f + 85.0f * t);
                }
                if (mv.region != "New Eden") dotR = std::max(dotR, 3.5f);  // small maps
                float hitR = dotR + 6.0f;
                ImU32 dotCol = IM_COL32(198, 208, 228, dotAlpha);
                ImFont* fnt2 = ImGui::GetFont();
                int hoverIdx = -1;
                float hbest = 1e18f;
                ImVec2 mp2 = io2.MousePos;
                time_t nowt2 = time(nullptr);
                for (int i = 0; i < (int)mv.nodes.size(); i++) {
                    const MapNode& n = mv.nodes[i];
                    ImVec2 q = at(n);
                    if (!inView(q)) continue;
                    auto bs = by_sys.find(n.label);
                    bool mine = bs != by_sys.end();
                    ImU32 dc = dotCol;
                    if (pilot_sys.count(n.llabel)) dc = IM_COL32(80, 255, 140, 255);
                    dl->AddCircleFilled(q, i == mv.selected ? dotR + 1.5f : dotR, dc, 0);
                    // past 10x every system gains a hollow marker ring, fading
                    // in over 10 -> 12, before full dots + names land at 14
                    if (mv.zoom > 10.0f && mv.zoom < kCutoff) {
                        int ha = (int)(std::clamp((mv.zoom - 10.0f) / 2.0f, 0.0f, 1.0f) * 200.0f);
                        if (ha > 2) dl->AddCircle(q, 7.0f, IM_COL32(198, 208, 228, ha), 0, 1.4f);
                    }
                    // STOKER overlay: worst fuel band of tracked structures here
                    if (mine) {
                        int worst = 3;
                        for (auto* r : bs->second)
                            if (r->has_fuel) worst = std::min(worst, urgency_band(r->days));
                        dl->AddCircle(q, dotR + 4.0f, band_u32(worst), 0, 2.0f);
                    }
                    // STOKER overlay: chat-log intel pulse
                    if (auto ia = intel_at.find(n.llabel); ia != intel_at.end()) {
                        double age = difftime(nowt2, ia->second);
                        float ir = dotR * 2.4f + 3.0f;
                        if (age < 300) {
                            float pulse = 0.55f + 0.45f * (float)std::sin(ImGui::GetTime() * 5.0);
                            dl->AddCircleFilled(q, ir, IM_COL32(255, 45, 45, (int)(28 + 42 * pulse)), 0);
                            dl->AddCircle(q, ir, IM_COL32(255, 45, 45, (int)(110 + 140 * pulse)), 0, 3.0f);
                            g_flash_active = true;  // keep the event loop fast
                        } else {
                            int alpha = (int)(255 * (1.0 - age / 1800.0));
                            if (alpha > 0)
                                dl->AddCircle(q, ir, IM_COL32(255, 45, 45, alpha), 0, 2.5f);
                        }
                    }
                    if (hovered) {
                        float dx = q.x - mp2.x, dy = q.y - mp2.y, d2 = dx * dx + dy * dy;
                        if (d2 < hbest && d2 <= hitR * hitR) {
                            hbest = d2;
                            hoverIdx = i;
                        }
                    }
                }
                // jump bridges after the dots so the arcs sit over the systems:
                // glow halo + bright core + a light travelling along each arc
                {
                    double tt = ImGui::GetTime();
                    int idx2 = 0;
                    for (auto& jb : mv.jb) {
                        ++idx2;
                        ImVec2 pa = at(mv.nodes[jb.first]), pb = at(mv.nodes[jb.second]);
                        if (!inView(pa) && !inView(pb)) continue;
                        float ddx = pb.x - pa.x, ddy = pb.y - pa.y;
                        float L = std::sqrt(ddx * ddx + ddy * ddy);
                        ImVec2 mid(0.5f * (pa.x + pb.x), 0.5f * (pa.y + pb.y));
                        ImVec2 c = mid;
                        if (L >= 1.0f) {
                            ImVec2 perp(-ddy / L, ddx / L);
                            if (perp.y > 0) { perp.x = -perp.x; perp.y = -perp.y; }
                            c = ImVec2(mid.x + perp.x * L * 0.18f, mid.y + perp.y * L * 0.18f);
                        }
                        dl->AddBezierQuadratic(pa, c, pb, IM_COL32(90, 175, 255, 45), 6.0f, 0);
                        dl->AddBezierQuadratic(pa, c, pb, IM_COL32(120, 195, 255, 90), 3.0f, 0);
                        dl->AddBezierQuadratic(pa, c, pb, IM_COL32(150, 210, 255, 255), 1.7f, 0);
                        float u = std::fmod((float)tt * 0.33f + idx2 * 0.17f, 1.0f), m1 = 1.0f - u;
                        ImVec2 tp(m1 * m1 * pa.x + 2 * m1 * u * c.x + u * u * pb.x,
                                  m1 * m1 * pa.y + 2 * m1 * u * c.y + u * u * pb.y);
                        dl->AddCircleFilled(tp, 2.0f, IM_COL32(205, 232, 255, 220), 8);
                        g_flash_active = true;  // the travelling light needs redraws
                    }
                }
                // selection + hover rings
                if (mv.selected >= 0 && mv.selected < (int)mv.nodes.size()) {
                    ImVec2 q = at(mv.nodes[mv.selected]);
                    if (inView(q))
                        dl->AddCircle(q, dotR * 1.8f + 2.0f, IM_COL32(0, 229, 255, 255), 0, 2.0f);
                }
                if (hoverIdx >= 0 && hoverIdx != mv.selected) {
                    ImVec2 q = at(mv.nodes[hoverIdx]);
                    dl->AddCircle(q, dotR * 1.8f + 2.0f, IM_COL32(255, 255, 255, 230), 0, 1.5f);
                }
                // labels below the dots, one per system: generic names appear
                // at the cutoff; structure/pilot/selected/hover always label
                auto labelBelow = [&](ImVec2 s, const std::string& txt, ImU32 col,
                                      float clearance, float fs) {
                    float w = fnt2->CalcTextSizeA(fs, 1e30f, 0.0f, txt.c_str()).x;
                    dl->AddText(fnt2, fs, ImVec2(s.x - w * 0.5f, s.y + clearance + 2.0f), col,
                                txt.c_str());
                };
                float smallFs = std::clamp(12.0f + mv.zoom * 0.5f, 12.0f, kLabelFs);
                for (int i = 0; i < (int)mv.nodes.size(); i++) {
                    const MapNode& n = mv.nodes[i];
                    ImVec2 q = at(n);
                    if (!inView(q)) continue;
                    bool mine = by_sys.count(n.label) > 0;
                    bool pil = pilot_sys.count(n.llabel) > 0;
                    bool special = i == mv.selected || i == hoverIdx || mine || pil;
                    if (!showNames && !special) continue;
                    ImU32 col = i == mv.selected ? IM_COL32(0, 229, 255, 255)
                                : i == hoverIdx  ? IM_COL32(255, 255, 255, 230)
                                : pil            ? IM_COL32(80, 255, 140, 255)
                                                 : IM_COL32(228, 234, 244, 255);
                    labelBelow(q, n.label, col, dotR + (special ? 3.0f : 0.0f),
                               showNames ? kLabelFs : smallFs);
                }
                dl->PopClipRect();
                char hud[160];
                std::snprintf(hud, sizeof hud,
                              "zoom %.2f   (names at %.0fx)   drag = pan  wheel = zoom  "
                              "click a system",
                              mv.zoom, kCutoff);
                dl->AddText(ImVec2(p0.x + 6, p0.y + 5), IM_COL32(140, 152, 172, 235), hud);
                if (hoverIdx >= 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    std::lock_guard<std::mutex> l(g_mtx);
                    g_map.selected = hoverIdx;
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
                              r->has_fuel ? r->days / pref_fuel(*r).cap : -1,
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

        // per-corp type-tab memory: switching corp tabs restores that corp's
        // last pick (first visit uses the config tab_type_filter default)
        std::string cur_label = (tabsel >= 0 && tabsel < (int)tabs.size()) ? tabs[tabsel] : "";
        {
            static std::map<std::string, std::string> type_mem;
            static int last_tab = -1;
            if (tabsel != last_tab) {
                last_tab = tabsel;
                auto mem = type_mem.find(cur_label);
                if (mem != type_mem.end()) {
                    g_type_tab = mem->second;
                } else {
                    auto dft = g_tab_default_filter.find(cur_label);
                    g_type_tab = dft != g_tab_default_filter.end() ? dft->second : "";
                }
            }
            type_mem[cur_label] = g_type_tab;
        }

        // filter
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##filter", "filter name/system/type", filter, sizeof filter);

        // recent structure notifications, folded into per-sid sets once per
        // frame (the render loop must not rescan the list per row)
        time_t nowt0 = time(nullptr);
        std::set<long long> destroyed_sids, attacked_sids;
        for (auto& n : notifs) {
            if (!n.at) continue;
            long age = (long)(nowt0 - n.at);
            if (n.type == "StructureDestroyed" && age < 48 * 3600)
                destroyed_sids.insert(n.sid);
            else if (age < 1800 &&
                     (n.type == "StructureUnderAttack" || n.type == "StructureLostShields" ||
                      n.type == "StructureLostArmor"))
                attacked_sids.insert(n.sid);
            else if (age < 1800 && n.moon_id && n.type == "TowerAlertMe") {
                // POS attacks name the moon, not the hull: map back to the row
                for (auto& r : rows)
                    if (r.is_pos && r.moon_id == n.moon_id) attacked_sids.insert(r.sid);
            }
        }
        auto row_gone = [&](const Row& r) {  // automated removal signals
            return r.state == "unanchored" || destroyed_sids.count(r.sid) > 0;
        };
        std::set<std::string> hot_systems;  // fresh intel, for the red system names
        for (auto& ih : g_intel)
            if (nowt0 - ih.at < 900) hot_systems.insert(ih.system);
        int suppressed = 0;
        for (auto& r : rows)
            if (row_gone(r)) suppressed++;
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
            if (toggle_btn("corp only", g_corp_only)) g_corp_only = !g_corp_only;
        }
        ImGui::SameLine();
        if (toggle_btn("needs fuel", g_prefs.fuel_filter)) {
            g_prefs.fuel_filter = !g_prefs.fuel_filter;
            prefs_save();
        }
        ImGui::SetItemTooltip(
            "hide structures above their fuel alert level\n(levels per type in "
            "settings; per structure in its detail panel)");
        ImGui::SameLine();
        if (toggle_btn("flags", g_prefs.warn_filter != 0)) {
            g_prefs.warn_filter = g_prefs.warn_filter ? 0 : 1;
            prefs_save();
        }
        ImGui::SetItemTooltip(
            "on = only structures with a warning flag\n(which flags count is set "
            "in settings; overrides needs fuel)");
        if (rentals_online && g_corp_only && rented) {
            // rides AFTER the buttons so toggling corp-only never shifts them
            ImGui::SameLine();
            ImGui::TextColored(GREY_, "(%d rented hidden)", rented);
        }

        // filtered view (built before the counters so the header numbers
        // describe the rows the user can actually see)
        std::string f = filter;
        for (auto& c : f) c = (char)tolower((unsigned char)c);
        std::vector<const Row*> view;
        for (auto& r : rows) {
            // automated removal: fully-unanchored hulls and freshly-destroyed
            // structures (notification beats the hourly list) drop on their own
            if (!g_show_hidden && row_gone(r)) continue;
            // manual hides (structure settings popup; unhide from settings)
            if (g_prefs.hidden.count(r.sid)) continue;
            if (rentals_online && g_corp_only && r.rental == "private") continue;
            // skyhooks live on their own tab only, never in All
            if (g_type_tab.empty() && r.is_skyhook) continue;
            if (!g_type_tab.empty() && r.type != g_type_tab) continue;
            if (!f.empty()) {
                std::string hay = r.name + " " + r.system + " " + r.type;
                for (auto& c : hay) c = (char)tolower((unsigned char)c);
                if (hay.find(f) == std::string::npos) continue;
            }
            // flags overrides needs-fuel: while it is pressed, every flagged
            // structure shows and nothing else does. Needs-fuel only applies
            // when the flags filter is off.
            if (g_prefs.warn_filter) {
                // skyhooks carry WF_SKYWIN while a theft window is announced,
                // so the flags filter surfaces exactly the hooks that matter
                unsigned fl = row_flags(r, attacked_sids, extractions_ok) &
                              g_prefs.warn_mask;
                if (!fl) continue;  // on = flagged only
            } else if (g_prefs.fuel_filter && !r.is_skyhook) {
                // keep rows at/below their alert level on any days-based bar
                // (no fuel data at all also counts as needy)
                bool needy = !r.has_fuel;
                if (r.has_fuel && r.days <= pref_fuel(r).alert) needy = true;
                if (r.gas_day > 0 && r.fuel2 >= 0 &&
                    fuel2_days(r) <= pref_gas(r).alert)
                    needy = true;
                if (!needy) continue;
            }
            view.push_back(&r);
        }
        ImGui::SameLine();
        int u14 = 0, u7 = 0;
        for (auto* r : view) {
            if (r->has_fuel && r->days < 14) u14++;
            if (r->has_fuel && r->days < 7) u7++;
        }
        ImGui::TextColored(GREY_, "  %d structures   under14d", (int)view.size());
        ImGui::SameLine();
        ImGui::TextColored(u14 ? ImVec4(0.98f, 0.84f, 0.27f, 1) : ImVec4(0.35f, 0.88f, 0.51f, 1), "%d", u14);
        ImGui::SameLine();
        ImGui::TextColored(GREY_, "  under7d");
        ImGui::SameLine();
        ImGui::TextColored(u7 ? ImVec4(1, 0.27f, 0.27f, 1) : ImVec4(0.35f, 0.88f, 0.51f, 1), "%d", u7);

        // brand-new structures the hourly list has not rolled in yet
        {
            load_universe();
            std::map<long long, bool> have;
            for (auto& r : rows) have[r.sid] = true;
            for (auto& n : notifs) {
                if (n.type != "StructureAnchoring" || !n.at || nowt0 - n.at > 24 * 3600)
                    continue;
                if (n.sid && have.count(n.sid)) continue;
                std::string sys = g_sysname.count((int)n.system_id)
                                      ? g_sysname[(int)n.system_id]
                                      : "?";
                ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.27f, 1),
                                   "new structure anchoring in %s - full data lands on the "
                                   "next ESI roll",
                                   sys.c_str());
            }
        }
        // why POS rows are missing, when they are fixably missing
        if (pos_status == "relogin")
            ImGui::TextColored(GREY_,
                               "POS towers hidden: re-login this character (alt+c) to grant "
                               "the starbases scope");
        else if (pos_status == "director")
            ImGui::TextColored(GREY_, "POS towers hidden: needs the in-game Director role");

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
                                  ImGuiTableFlags_BordersInnerH,
                              // leave one line under the table for the totals bar
                              ImVec2(0, -(ImGui::GetTextLineHeightWithSpacing() + 6)))) {
            ImGui::TableSetupColumn("Fuel", ImGuiTableColumnFlags_WidthFixed, 250);
            ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);

            // no header row to click: always most-urgent-first (worst of the
            // fuel and gas clocks)
            std::stable_sort(view.begin(), view.end(), [](const Row* a, const Row* b) {
                auto worst = [](const Row* r) {
                    if (r->is_skyhook) {
                        // own tab only: open windows first, then soonest to
                        // open, windowless hooks at the bottom
                        time_t nowt = time(nullptr);
                        time_t ws = r->sky_wstart.empty() ? 0 : parse_iso(r->sky_wstart);
                        time_t we = r->sky_wend.empty() ? 0 : parse_iso(r->sky_wend);
                        if (ws && we && nowt >= ws && nowt < we) return 0.0;
                        if (ws && nowt < ws) return (double)(ws - nowt) / 86400.0;
                        return 1e9;
                    }
                    double d = r->has_fuel ? r->days : 1e9;
                    double g = fuel2_days(*r);
                    if (g >= 0) d = std::min(d, g);
                    return d;
                };
                return worst(a) < worst(b);
            });

            for (size_t vi = 0; vi < view.size(); vi++) {
                const Row& r = *view[vi];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                dual_gauge(r, 240);
                ImGui::TableSetColumnIndex(1);
                {
                    bool hot = hot_systems.count(r.system) > 0;
                    ImGui::TextColored(hot ? ImVec4(1, 0.27f, 0.27f, 1) : CYAN_, "%s",
                                       r.system.c_str());
                    // power state under the system name, derived the way the
                    // game derives it: fuel gone = Low Power, 7+ days without
                    // fuel = Abandoned; all services off while fueled = Offline.
                    // Structures still anchoring/onlining (or already
                    // unanchored) have no fuel clock BY DESIGN: no false badge.
                    bool limbo = r.is_skyhook || r.state == "anchoring" ||
                                 r.state == "anchor_vulnerable" ||
                                 r.state == "deploy_vulnerable" ||
                                 r.state == "fitting_invulnerable" ||
                                 r.state == "onlining_vulnerable" || r.state == "unanchored";
                    const char* ftxt = nullptr;
                    ImU32 fcol = 0;
                    if (limbo) {
                        // no power badge while the hull isn't in service yet
                    } else if (r.is_pos) {
                        // tower power rules (see row_flags): offline is a
                        // parked state, not a fuel emergency
                        if (r.state == "offline") {
                            ftxt = "OFFLINE";
                            fcol = IM_COL32(128, 136, 150, 255);
                        } else if (r.state == "online" && r.has_fuel && r.days <= 0) {
                            ftxt = "LOW POWER";
                            fcol = IM_COL32(255, 140, 0, 255);
                        } else if (r.state == "online" && r.has_fuel && r.days < 7) {
                            ftxt = "LOW FUEL";
                            fcol = IM_COL32(250, 215, 70, 255);
                        }
                    } else if (r.has_fuel && r.days <= -7) {
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
                ImVec2 cp = ImGui::GetCursorScreenPos();
                bool tallied = g_tally.count(r.sid) > 0;
                if (tallied)  // amber wash + gold edge bar below: unmissable
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           IM_COL32(122, 86, 6, 140));
                if (ImGui::Selectable(("##row" + std::to_string(r.sid)).c_str(),
                                      r.sid == detail_sid,
                                      ImGuiSelectableFlags_SpanAllColumns,
                                      ImVec2(0, rowh))) {
                    ImGuiIO& cio = ImGui::GetIO();
                    // skyhooks are their own thing: never part of a haul batch
                    if (cio.KeyShift && !r.is_skyhook) {
                        // range from the last tally click, in the visible sort order
                        int ai = -1, bi = (int)vi;
                        for (int k = 0; k < (int)view.size(); k++)
                            if (view[k]->sid == g_tally_anchor) { ai = k; break; }
                        if (ai < 0) ai = bi;
                        for (int k = std::min(ai, bi); k <= std::max(ai, bi); k++)
                            if (!view[k]->is_skyhook) g_tally.insert(view[k]->sid);
                        g_tally_anchor = r.sid;
                        view_tab = 0;
                    } else if (cio.KeyCtrl) {
                        if (!r.is_skyhook) {
                            if (!g_tally.erase(r.sid)) {
                                g_tally.insert(r.sid);
                                g_tally_anchor = r.sid;
                            }
                            view_tab = 0;
                        }
                    } else {
                        detail_sid = r.sid;
                        view_tab = 0;
                    }
                }
                if (tallied) {
                    ImVec2 tmn = ImGui::GetItemRectMin(), tmx = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2(tmn.x, tmn.y), ImVec2(tmn.x + 4, tmx.y),
                        IM_COL32(255, 196, 0, 255));
                }
                ImDrawList* dl2 = ImGui::GetWindowDrawList();
                float lh = ImGui::GetTextLineHeight();
                ImFont* fnt = ImGui::GetFont();
                // structure-type portrait on the left edge; the text block
                // indents past it (space reserved even without art so rows align)
                float ix = rowh - 8 + 8;
                {
                    auto ti = g_type_icons.find(icon_key(r));
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
                    if (r.state == "armor_reinforce" || r.state == "armor_vulnerable" ||
                        r.state == "reinforced")  // a POS reinforces at 25% shield
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
                    // constant label widths, measured once per frame
                    static float w_armour = 0, w_moonpull = 0;
                    static float w_for_fs = -1;
                    if (w_for_fs != fs) {
                        w_for_fs = fs;
                        w_armour = fnt->CalcTextSizeA(fs, 1e30f, 0, "Armour").x;
                        w_moonpull = fnt->CalcTextSizeA(fs, 1e30f, 0, "Moon Pull:").x;
                    }
                    float tx = cp.x + ix + 13 + w_armour + 18;
                    if (r.is_skyhook) {
                        // Window: countdown, Income: figures, flashing RAIDABLE
                        std::string tt;
                        ImU32 tc;
                        bool open_ = false;
                        skyhook_timer_info(r, tt, tc, open_);
                        dl2->AddText(fnt, fs, ImVec2(tx, y0), IM_COL32(128, 136, 150, 255),
                                     "Window:");
                        dl2->AddText(fnt, fs, ImVec2(tx + w_moonpull + 6, y0), tc, tt.c_str());
                        dl2->AddText(fnt, fs, ImVec2(tx, y0 + sp), IM_COL32(128, 136, 150, 255),
                                     "Income:");
                        std::string inc = r.sky_hourly >= 0 ? commas(r.sky_hourly) + "/h" : "?";
                        dl2->AddText(fnt, fs, ImVec2(tx + w_moonpull + 6, y0 + sp),
                                     IM_COL32(200, 206, 222, 255), inc.c_str());
                        if (open_) {  // the streak meter carries the count now
                            g_flash_active = true;
                            int wa = (int)(90 +
                                           165 * (0.5 + 0.5 * std::sin(ImGui::GetTime() * 6.0)));
                            warn_tri(dl2, ImVec2(tx, y0 + 2 * sp + 1), fs - 2,
                                     IM_COL32(255, 70, 70, 255), wa);
                            dl2->AddText(fnt, fs, ImVec2(tx + fs + 4, y0 + 2 * sp),
                                         IM_COL32(255, 70, 70, wa), "RAIDABLE");
                        } else if (tt != "none") {
                            // window announced but not open yet: steady amber
                            // heads-up (the Window: line carries the countdown)
                            warn_tri(dl2, ImVec2(tx, y0 + 2 * sp + 1), fs - 2,
                                     IM_COL32(250, 215, 70, 255), 255);
                            dl2->AddText(fnt, fs, ImVec2(tx + fs + 4, y0 + 2 * sp),
                                         IM_COL32(250, 215, 70, 255), "WINDOW SOON");
                        }
                    } else {
                    {
                        dl2->AddText(fnt, fs, ImVec2(tx, y0), IM_COL32(128, 136, 150, 255),
                                     "Timer:");
                        std::string tt;
                        ImU32 tc;
                        timer_info(r, tt, tc);
                        dl2->AddText(fnt, fs, ImVec2(tx + w_moonpull + 6, y0), tc, tt.c_str());
                    }
                    if (has_moon_pull(r.type)) {
                        dl2->AddText(fnt, fs, ImVec2(tx, y0 + sp), IM_COL32(128, 136, 150, 255),
                                     "Moon Pull:");
                        std::string mt;
                        ImU32 mc;
                        moonpull_info(r, extractions_ok, mt, mc);
                        dl2->AddText(fnt, fs, ImVec2(tx + w_moonpull + 6, y0 + sp), mc,
                                     mt.c_str());
                    } else if (r.has_econ) {
                        // Metenox: the moon-pull slot carries the monthly net
                        dl2->AddText(fnt, fs, ImVec2(tx, y0 + sp), IM_COL32(128, 136, 150, 255),
                                     "Net:");
                        dl2->AddText(fnt, fs, ImVec2(tx + w_moonpull + 6, y0 + sp),
                                     net_col(r.econ_net),
                                     (net_str(r.econ_net) + "/mo").c_str());
                    }
                    // state warning under Moon Pull: no label, blank when calm,
                    // flashing triangle + text when something is happening.
                    // Mapped from the ESI state enum, unanchors_at, and the
                    // (much faster) notification feed.
                    {
                        const char* wtxt = nullptr;
                        ImU32 wcol = 0;
                        time_t ua = r.unanchors_at.empty() ? 0 : parse_iso(r.unanchors_at);
                        if (attacked_sids.count(r.sid)) {
                            // notification-fed: fires within ~10 min of the hit,
                            // long before the hourly structure state catches up
                            wtxt = "UNDER ATTACK";
                            wcol = IM_COL32(255, 70, 70, 255);
                        } else if (r.state == "armor_reinforce" || r.state == "hull_reinforce" ||
                            r.state == "armor_vulnerable" || r.state == "hull_vulnerable") {
                            wtxt = "UNDER ATTACK";
                            wcol = IM_COL32(255, 70, 70, 255);
                        } else if (r.state == "reinforced") {
                            // a tower only reinforces because someone shot it;
                            // the Timer slot counts down reinforced_until
                            wtxt = "REINFORCED";
                            wcol = IM_COL32(255, 70, 70, 255);
                        } else if (r.is_pos && r.state == "onlining") {
                            wtxt = "ONLINING...";
                            wcol = IM_COL32(250, 215, 70, 255);
                        } else if (r.state == "unanchored") {
                            wtxt = "UNANCHORED";
                            wcol = IM_COL32(250, 215, 70, 255);
                        } else if (ua > nowt0) {
                            wtxt = "UNANCHORING";
                            wcol = IM_COL32(250, 215, 70, 255);
                        } else if (ua && ua <= nowt0) {
                            // unanchor finished but the hourly state hasn't
                            // flipped yet: the hull is floating and scoopable
                            wtxt = "UNANCHORED";
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
                    }  // end non-skyhook branch
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        // totals bar: current stock across everything visible on this tab
        // (the ctrl+click haul tally lives in the detail panel). Fuel/gas
        // stock only exists where the data source can see the bay, so unseen
        // rows are counted and disclosed.
        {
            double blk = 0, gas = 0, ozone = 0, goo = 0, goo_isk = 0;
            bool any_isk = false;
            int blind = 0;
            for (auto* rp : view) {
                const Row& r = *rp;
                bool saw = false;
                if (r.blocks_now >= 0) { blk += r.blocks_now; saw = true; }
                if (r.fuel2 >= 0) {
                    if (r.fuel2_name == "Magmatic Gas") gas += r.fuel2;
                    else if (r.fuel2_name == "Liquid Ozone") ozone += r.fuel2;
                    saw = true;
                }
                if (r.goo_m3 >= 0) { goo += r.goo_m3; saw = true; }
                if (r.goo_isk >= 0) { goo_isk += r.goo_isk; any_isk = true; }
                if (!saw && !r.is_skyhook) blind++;
            }
            ImGui::TextColored(GREY_, "TOTAL shown %d", (int)view.size());
            ImGui::SameLine();
            ImGui::TextColored(GREY_, "  fuel");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.51f, 1), "%s blk / %s m3",
                               commas(blk).c_str(), commas(blk * 5).c_str());
            if (gas > 0) {
                ImGui::SameLine();
                ImGui::TextColored(GREY_, "  gas");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.98f, 0.55f, 0.85f, 1), "%s / %s m3",
                                   commas(gas).c_str(), commas(gas * 0.01).c_str());
            }
            if (ozone > 0) {
                ImGui::SameLine();
                ImGui::TextColored(GREY_, "  ozone");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s / %s m3",
                                   commas(ozone).c_str(), commas(ozone * 0.4).c_str());
            }
            if (goo > 0) {
                ImGui::SameLine();
                ImGui::TextColored(GREY_, "  moon hold");
                ImGui::SameLine();
                std::string gtxt = commas(goo) + " m3";
                if (any_isk) gtxt += " (" + isk_compact(goo_isk) + ")";
                ImGui::TextColored(CYAN_, "%s", gtxt.c_str());
            }
            if (blind > 0) {
                ImGui::SameLine();
                ImGui::TextColored(GREY_, "  (%d no bay data)", blind);
            }
            ImGui::SetItemTooltip("ctrl+click / shift+click rows for a hauling tally");
        }
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
                if (!g_tally.empty()) {
                    // ---- hauling tally: what to move, where, in jump order ----
                    load_universe();
                    struct TItem {
                        const Row* r;
                        double blk = 0;          // fuel blocks to 30d
                        double gas = -2;         // magmatic to 30d; -1 bay unseen, -2 n/a
                        double oz = -2;          // ozone to doctrine target; same codes
                        double goo = 0;          // metenox cargo pickup, m3
                        int jumps = -1;
                    };
                    std::vector<TItem> items;
                    int elsewhere = 0;
                    for (auto& sid : g_tally) {
                        const Row* rr = nullptr;
                        for (auto& r : rows)
                            if (r.sid == sid) { rr = &r; break; }
                        if (!rr || rr->is_skyhook) {
                            if (!rr) elsewhere++;
                            continue;
                        }
                        TItem t;
                        t.r = rr;
                        double bn = need_fuel_units(*rr, pref_fuel(*rr).cap);
                        if (bn > 0) t.blk = bn;
                        if (rr->fuel2_name == "Magmatic Gas")
                            t.gas = (rr->fuel2 >= 0 && rr->gas_day > 0)
                                        ? need_gas_units(*rr, pref_gas(*rr).cap)
                                        : -1;
                        if (rr->fuel2_name == "Liquid Ozone" && rr->lo_target > 0)
                            t.oz = rr->fuel2 >= 0 ? std::max(0.0, rr->lo_target - rr->fuel2)
                                                  : -1;
                        if (rr->goo_m3 > 0) t.goo = rr->goo_m3;
                        items.push_back(t);
                    }
                    // jump counts from the pilot over gates + jump bridges
                    int src = 0;
                    std::string pilot_sys;
                    if (!g_pilots.empty()) {
                        pilot_sys = g_pilots[0].system;
                        auto sit = g_sysid.find(lower_(pilot_sys));
                        if (sit != g_sysid.end()) src = sit->second;
                    }
                    if (src) {
                        auto& dist = jump_dists(src);
                        for (auto& t : items) {
                            auto sit = g_sysid.find(lower_(t.r->system));
                            if (sit != g_sysid.end()) {
                                auto dit = dist.find(sit->second);
                                if (dit != dist.end()) t.jumps = dit->second;
                            }
                        }
                    }
                    std::stable_sort(items.begin(), items.end(),
                                     [](const TItem& a, const TItem& b) {
                                         unsigned ja = a.jumps < 0 ? 9999u : (unsigned)a.jumps;
                                         unsigned jb2 = b.jumps < 0 ? 9999u : (unsigned)b.jumps;
                                         if (ja != jb2) return ja < jb2;
                                         return a.r->name < b.r->name;
                                     });

                    // totals; a category only exists if a selected type carries it
                    double s_blk = 0, s_gas = 0, s_oz = 0, s_goo = 0;
                    bool has_gas = false, has_oz = false, gas_blind = false, oz_blind = false;
                    for (auto& t : items) {
                        s_blk += t.blk;
                        if (t.gas >= 0) { s_gas += t.gas; has_gas = true; }
                        if (t.gas == -1) { has_gas = true; gas_blind = true; }
                        if (t.oz >= 0) { s_oz += t.oz; has_oz = true; }
                        if (t.oz == -1) { has_oz = true; oz_blind = true; }
                        s_goo += t.goo;
                    }
                    double total_m3 = s_blk * 5 + s_gas * 0.01 + s_oz * 0.4 + s_goo;

                    ImGui::TextColored(ImVec4(1.0f, 0.77f, 0.0f, 1), "HAUL TALLY - %d structures",
                                       (int)items.size());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("clear")) g_tally.clear();
                    if (elsewhere)
                        ImGui::TextColored(GREY_, "(%d tallied on another corp tab not counted)",
                                           elsewhere);
                    ImGui::Separator();
                    ImGui::TextColored(GREY_, "total to move");
                    ImGui::SameLine();
                    ImGui::TextColored(CYAN_, "%s m3", commas(total_m3).c_str());
                    if (s_blk > 0)
                        ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.51f, 1),
                                           "  fuel blocks to max      %s blocks   %s m3",
                                           commas(s_blk).c_str(), commas(s_blk * 5).c_str());
                    if (has_gas)
                        ImGui::TextColored(ImVec4(0.98f, 0.55f, 0.85f, 1),
                                           "  magmatic gas to max     %s units   %s m3%s",
                                           commas(s_gas).c_str(), commas(s_gas * 0.01).c_str(),
                                           gas_blind ? "  (+? unseen bays)" : "");
                    if (has_oz)
                        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1),
                                           "  ozone to doctrine       %s units   %s m3%s",
                                           commas(s_oz).c_str(), commas(s_oz * 0.4).c_str(),
                                           oz_blind ? "  (+? unseen bays)" : "");
                    if (s_goo > 0)
                        ImGui::TextColored(CYAN_, "  metenox cargo pickup    %s m3",
                                           commas(s_goo).c_str());
                    ImGui::Separator();
                    if (src)
                        ImGui::TextColored(GREY_, "route from %s (gates + jump bridges)",
                                           pilot_sys.c_str());
                    else
                        ImGui::TextColored(GREY_,
                                           "no pilot position (EVE chat logs) - unsorted, "
                                           "jumps unknown");
                    ImGui::Spacing();
                    for (auto& t : items) {
                        const Row& r = *t.r;
                        ImGui::PushID((int)(r.sid & 0x7fffffff));
                        if (ImGui::SmallButton("set dest")) {
                            auto sit = g_sysid.find(lower_(r.system));
                            if (sit != g_sysid.end())
                                act_set_destination(sit->second, r.system);
                        }
                        ImGui::PopID();
                        ImGui::SameLine();
                        if (t.jumps >= 0)
                            ImGui::TextColored(ImVec4(1.0f, 0.77f, 0.0f, 1), "%2dj", t.jumps);
                        else
                            ImGui::TextColored(GREY_, " ?j");
                        ImGui::SameLine();
                        ImGui::TextColored(TEXTC, "%s", r.name.c_str());
                        std::string in, out;
                        auto add = [](std::string& s, const std::string& piece) {
                            if (!s.empty()) s += ", ";
                            s += piece;
                        };
                        if (t.blk > 0) add(in, commas(t.blk) + " blocks");
                        if (t.gas > 0) add(in, commas(t.gas) + " gas");
                        if (t.gas == -1) add(in, "gas ?");
                        if (t.oz > 0) add(in, commas(t.oz) + " ozone");
                        if (t.oz == -1) add(in, "ozone ?");
                        if (t.goo > 0)
                            out = commas(t.goo) + " m3 goo" +
                                  (r.goo_isk > 0 ? " (" + isk_compact(r.goo_isk) + ")" : "");
                        std::string line;
                        if (!in.empty()) line += "in: " + in;
                        if (!out.empty()) line += (line.empty() ? "" : "   ") + ("out: " + out);
                        if (line.empty()) line = "nothing to haul";
                        ImGui::TextColored(GREY_, "        %s", line.c_str());
                        ImGui::Spacing();
                    }
                } else if (!d) {
                    ImGui::TextColored(GREY_, "select a structure");
                } else {
                    ImGui::PushTextWrapPos(0.0f);
                    {
                        auto ti = g_type_icons.find(icon_key(*d));
                        if (ti != g_type_icons.end())
                            ImGui::Image(ti->second, ImVec2(48, 48));
                        else
                            ImGui::Image(g_ic_fuel, ImVec2(24, 24));
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(CYAN_, "%s", d->name.c_str());
                    ImGui::TextColored(GREY_, "%s   %s   %s", d->system.c_str(),
                                       d->is_pos ? d->tower.c_str() : d->type.c_str(),
                                       d->state.c_str());
                    if (d->rental == "private")
                        ImGui::TextColored(PINK, "rented to %s",
                                           d->renter.empty() ? "?" : d->renter.c_str());
                    else if (d->rental == "corp")
                        ImGui::TextColored(GREY_, "corp moon");
                    {
                        std::string tt;
                        ImU32 tc;
                        if (d->is_skyhook) {
                            bool open_ = false;
                            skyhook_timer_info(*d, tt, tc, open_);
                            ImGui::TextColored(GREY_, "Theft window:");
                            ImGui::SameLine();
                            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(tc), "%s%s",
                                               tt.c_str(), open_ ? "  RAIDABLE NOW" : "");
                            if (d->sky_hourly >= 0) {
                                ImGui::TextColored(GREY_, "Income:");
                                ImGui::SameLine();
                                ImGui::TextColored(TEXTC, "%s ISK/h", commas(d->sky_hourly).c_str());
                            }
                            if (d->sky_unsec_m3 >= 0) {
                                const char* tld = d->sky_est ? "~" : "";
                                ImGui::TextColored(GREY_, "Raidable bay:");
                                ImGui::SameLine();
                                ImGui::TextColored(TEXTC, "%s%s m3 / %s  (%s%s isk exposed)",
                                                   tld, commas(d->sky_unsec_m3).c_str(),
                                                   commas(SKY_RAIDABLE_M3).c_str(), tld,
                                                   isk_compact(d->sky_unsec_isk).c_str());
                                if (d->sky_est)
                                    ImGui::TextColored(GREY_,
                                                       "estimated: income rate x 72h x "
                                                       "unraided streak (x%d)",
                                                       d->sky_streak < 0 ? 0 : d->sky_streak);
                            }
                            if (d->sky_bays && d->sky_sec_m3 >= 0) {
                                ImGui::TextColored(GREY_, "Reserve hold:");
                                ImGui::SameLine();
                                ImGui::TextColored(TEXTC, "%s m3 / %s  (%s isk banked)",
                                                   commas(d->sky_sec_m3).c_str(),
                                                   commas(SKY_RESERVE_M3).c_str(),
                                                   isk_compact(d->sky_sec_isk).c_str());
                            }
                            if (d->sky_unraided >= 0)
                                ImGui::TextColored(GREY_,
                                                   "record: %d unraided / %d raided, "
                                                   "unraided x%d in a row",
                                                   d->sky_unraided, d->sky_raided,
                                                   d->sky_streak < 0 ? 0 : d->sky_streak);
                        } else {
                        timer_info(*d, tt, tc);
                        ImGui::TextColored(GREY_, "Timer:");
                        ImGui::SameLine();
                        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(tc), "%s", tt.c_str());
                        if (has_moon_pull(d->type)) {
                            std::string mt;
                            ImU32 mc;
                            moonpull_info(*d, extractions_ok, mt, mc);
                            ImGui::TextColored(GREY_, "Moon Pull:");
                            ImGui::SameLine();
                            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(mc), "%s",
                                               mt.c_str());
                            // how this chunk actually popped (notification-fed)
                            time_t arr =
                                d->chunk_arrival.empty() ? 0 : parse_iso(d->chunk_arrival);
                            if (d->popped_at && arr > 0 && d->popped_at >= arr - 120) {
                                std::string ago =
                                    fmt_dur((double)(time(nullptr) - d->popped_at));
                                if (d->popped_manual)
                                    ImGui::TextColored(TEXTC, "fired by %s, %s ago",
                                                       d->popped_by.empty()
                                                           ? "?" : d->popped_by.c_str(),
                                                       ago.c_str());
                                else
                                    ImGui::TextColored(TEXTC, "auto-fractured %s ago",
                                                       ago.c_str());
                            }
                            if (d->drill_rig_tier > 0) {
                                ImGui::TextColored(GREY_, "Drill rig:");
                                ImGui::SameLine();
                                ImGui::TextColored(TEXTC, "%s", d->drill_rig.c_str());
                                ImGui::TextColored(GREY_,
                                                   "  +%d%% fire window, +%d%% field life",
                                                   d->drill_rig_tier == 2 ? 24 : 20,
                                                   d->drill_rig_tier == 2 ? 100 : 50);
                            }
                        }
                        }
                    }
                    ImGui::Separator();
                    ImGui::Spacing();
                    float gw = ImGui::GetContentRegionAvail().x - 4;
                    char b[64];
                    std::snprintf(b, sizeof b, "%.1f days", d->days);
                    if (d->has_fuel) {
                        float fcap = pref_fuel(*d).cap;
                        gauge("df", d->days / fcap, b,
                              to_cap(need_str(need_fuel_units(*d, fcap)), fcap), gw, false,
                              g_ic_fuel);
                        ImGui::Spacing();
                    }
                    if (d->is_pos) {
                        // the fuel bay is real block counts (starbase detail
                        // endpoint), unlike Upwell rows where it's clock-derived
                        if (d->blocks_now >= 0)
                            ImGui::TextColored(GREY_, "%s fuel blocks in the bay%s",
                                               commas(d->blocks_now).c_str(),
                                               d->state == "online" ? ""
                                                                    : " (not burning)");
                        if (d->bpd > 0)
                            ImGui::TextColored(GREY_, "burn %s blocks/day%s",
                                               commas(d->bpd).c_str(),
                                               d->pos_sov ? " (sov holder, -25%)" : "");
                        ImGui::Spacing();
                        if (d->stront_hours >= 0) {
                            char sh[32];
                            std::snprintf(sh, sizeof sh, "%.1fh reinforce", d->stront_hours);
                            gauge("dstr", d->stront_hours / POS_STRONT_MAX_H, sh,
                                  commas(d->stront) + " stront", gw, true);
                            ImGui::TextColored(GREY_,
                                               "%s Strontium Clathrates = how long it holds "
                                               "if reinforced",
                                               commas(d->stront).c_str());
                        } else {
                            ImGui::TextColored(GREY_, "stront bay unreadable this pull");
                        }
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
                            float gcap = pref_gas(*d).cap;
                            gauge("df2", fuel2_days(*d) / gcap, g2,
                                  to_cap(need_str(need_gas_units(*d, gcap)), gcap), gw, true,
                                  g_ic_gas);
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
                        double gf = d->goo_m3 >= 0
                                        ? std::min(1.0, d->goo_m3 /
                                                            std::max(1.0f, g_prefs.goo_max_m3))
                                        : -1;
                        std::string gl =
                            d->goo_m3 >= 0 ? commas(d->goo_m3) + " m3" : "?";
                        gauge("dgoo", gf, gl,
                              isk_compact(d->goo_isk < 0 ? 0 : d->goo_isk) + " isk", gw, false,
                              g_ic_goo, false, IM_COL32(0, 130, 148, 255));
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
                    if (d->has_econ) {
                        // monthly profitability: the drill's whole ledger as a
                        // stacked equation (taxes = the alliance moon rent)
                        ImGui::Separator();
                        ImGui::TextColored(GREY_, "monthly profitability");
                        auto eq = [](const char* pre, const char* label, double v,
                                     ImU32 vcol) {
                            char lbl[32];
                            std::snprintf(lbl, sizeof lbl, "%s %-13s", pre, label);
                            ImGui::TextColored(GREY_, "%s", lbl);
                            ImGui::SameLine();
                            char val[32];
                            std::snprintf(val, sizeof val, "%9s", net_str(v).c_str());
                            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(vcol),
                                               "%s", val);
                        };
                        ImU32 plain = IM_COL32(200, 206, 222, 255);
                        eq(" ", "Moon Goo", d->econ_goo, plain);
                        eq("-", "Taxes", d->econ_rent, plain);
                        eq("-", "Fuel Blocks", d->econ_fuel, plain);
                        eq("-", "Magmatic Gas", d->econ_gas, plain);
                        ImGui::TextColored(GREY_, "  ---------------------");
                        eq("=", "NET", d->econ_net, net_col(d->econ_net));
                        ImGui::SameLine();
                        ImGui::TextColored(GREY_, "/month");
                        ImGui::Spacing();
                    }
                    if (d->bpd > 0 && !d->is_pos)  // POS rows print their own burn line
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
                        if (sit != g_sysid.end()) {
                            if (ImGui::Button("set destination"))
                                act_set_destination(sit->second, d->system);
                            ImGui::SameLine();
                        }
                        // per-structure settings popup: alert/cap overrides
                        // (beat the type levels) + hide, all persisted
                        bool ovr = g_prefs.sid_fuel.count(d->sid) > 0 ||
                                   g_prefs.sid_gas.count(d->sid) > 0;
                        if (ImGui::Button(ovr ? "settings (custom)" : "settings"))
                            ImGui::OpenPopup("structprefs");
                        if (ImGui::BeginPopup("structprefs")) {
                            ImGui::Dummy(ImVec2(380, 0));  // popup min width
                            ImGui::TextColored(CYAN_, "%s", d->name.c_str());
                            ImGui::TextColored(GREY_,
                                               "alert feeds the needs-fuel filter; max "
                                               "sets the bar cap + haul target");
                            ImGui::Separator();
                            FuelPref fp = pref_fuel(*d);
                            bool ch = false;
                            ImGui::TextColored(GREY_, "fuel alert");
                            ImGui::SameLine(90);
                            ch |= dial_slider("sfa", &fp.alert, 0.0f, 14.0f, "%.0f d");
                            ImGui::TextColored(GREY_, "fuel max");
                            ImGui::SameLine(90);
                            ch |= dial_slider("sfc", &fp.cap, 1.0f, 90.0f, "%.0f d");
                            if (ch) {
                                g_prefs.sid_fuel[d->sid] = fp;
                                g_prefs_dirty = true;
                            }
                            if (d->gas_day > 0) {
                                FuelPref gp = pref_gas(*d);
                                bool gh = false;
                                ImGui::TextColored(GREY_, "gas alert");
                                ImGui::SameLine(90);
                                gh |= dial_slider("sga", &gp.alert, 0.0f, 14.0f, "%.0f d");
                                ImGui::TextColored(GREY_, "gas max");
                                ImGui::SameLine(90);
                                gh |= dial_slider("sgc", &gp.cap, 1.0f, 90.0f, "%.0f d");
                                if (gh) {
                                    g_prefs.sid_gas[d->sid] = gp;
                                    g_prefs_dirty = true;
                                }
                            }
                            if (ovr && ImGui::SmallButton("back to type defaults")) {
                                g_prefs.sid_fuel.erase(d->sid);
                                g_prefs.sid_gas.erase(d->sid);
                                prefs_save();
                            }
                            ImGui::Separator();
                            if (ImGui::SmallButton("hide this structure")) {
                                g_prefs.hidden.insert(d->sid);
                                prefs_save();
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SetItemTooltip(
                                "drops it from the table; unhide from the settings "
                                "window");
                            ImGui::EndPopup();
                        }
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

        // flush deferred pref edits once the drag ends (no active widget)
        if (g_prefs_dirty && !ImGui::IsAnyItemActive()) {
            prefs_save();
            g_prefs_dirty = false;
        }

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
    if (g_prefs_dirty) { prefs_save(); g_prefs_dirty = false; }   // don't lose a mid-drag edit on quit
    // a pending browser login would otherwise hold its join for up to 3 min,
    // which reads as a frozen window / crash on close
    standalone::cancel_pending_login();
    if (th.joinable()) th.join();
    if (logs_th.joinable()) logs_th.join();
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
