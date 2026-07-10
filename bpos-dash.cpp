// STOKER: live FTXUI fuel-watch dashboard for Brave Pos Boys' Upwell
// structures. (A stoker keeps the furnaces fed; so does this.)
//
// Corp mode fetches a hosted STOKER backend (key-gated JSON endpoint): a cached JSON snapshot, refreshed
// server-side by cron every 30 min. A background thread re-fetches every 30s;
// 'r' kicks an ASYNC live ESI pull on the box (returns instantly, fresh data
// lands on the next poll ~30-90s later). Fuel-sorted by default so whatever
// runs dry first is on top.
//
// Keys: up/down/pgup/pgdn/wheel scroll, [tab] cycle type filter,
//       [s] cycle sort, [/] text filter (Enter apply, Esc clear), [f] refuel
//       log, [->] detail page for the selected structure ([<-]/esc back),
//       [1] "I fueled this" on the selected refuel event (or on a
//       structure you just fueled from the main view), [r] refresh, [q] quit.
//
// Refuel log BLOCKS column is the fuel-log estimate (days added x the
// structure's service burn rate). ESI exposes neither the fueler nor the bay
// contents for Upwell structures, so BY comes from the [1] claim; block-type
// tracking was tried and dropped (no data source short of a Director token).
// Build: cmake -B build && cmake --build build   (see README)
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/terminal.hpp>

#include "json.hpp"

// Windows: same design (shell out to curl, which Windows 10+ ships in
// System32), different spellings. Command strings use double quotes so both
// sh and cmd.exe parse them.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef RGB  // wingdi macro; we want ftxui::Color::RGB
#undef OUT  // windef.h annotation macros; OUT collides with a palette const
#undef IN
#define popen _popen
#define pclose _pclose
#define timegm _mkgmtime
#define QUIET " 2>NUL"
#else
#define QUIET " 2>/dev/null"
#endif

using json = nlohmann::json;
using namespace ftxui;

// Hot Neon palette (matches the desktop rice + app icon).
static const Color NEON_PINK = Color::RGB(255, 43, 214);
static const Color NEON_CYAN = Color::RGB(0, 229, 255);
static const Color NEON_DIM_CYAN = Color::RGB(0, 130, 150);
static const Color INK_GRAY = Color::RGB(110, 118, 138);

// Data source, resolved at startup by load_or_setup(): corp mode talks to the
// key-gated market-bot endpoint (full features); standalone mode pulls ESI
// directly with the user's own EVE SSO login (no burn rates / refuel log /
// claims, those are server-side).
static bool g_standalone = false;
static std::string g_fetch_url, g_refresh_url, g_claim_url, g_client_id;
static const int REFRESH_SECONDS = 30;       // corp endpoint poll
static const int ESI_REFRESH_SECONDS = 300;  // ESI caches corp structures ~1h

struct RefuelEvent {
    std::string seen_at, by;
    double days_added = 0;
};

struct GooItem {
    std::string name;
    double qty = 0, m3 = 0, isk = 0;
};

struct Row {
    long long sid = 0;
    std::string name, system, type, state, services, fuel_expires, last_refuel;
    bool has_fuel = false, has_refuel = false;
    double days = 0, refuel_added = 0;
    double burn7 = -1, burn30 = -1;   // fuel-days/day; -1 = not enough history
    double est = -1;                  // usage-adjusted days left; -1 = unknown
    double need = -1, m3 = -1;        // blocks + m3 to top up to 30d; -1 = rate unknown
    double bpd = -1;                  // blocks/day service burn; -1 = unknown
    double blocks_now = -1, m3_now = -1;      // estimated bay contents
    double lo_min = -1, lo_target = -1;       // Ansiblex ozone doctrine tier
    double gas_day = -1, gas_month = -1, gas_m3 = -1;  // Metenox magmatic
    std::string fuel2_name;           // secondary fuel kind (gas/ozone), "" = none
    double fuel2 = -1;                // stock units; -1 = source can't see the bay
    std::string rental, renter;       // moon-rental class: corp|private|unknown, "" = n/a
    double goo_m3 = -1, goo_cap = -1, goo_isk = -1;  // Metenox moon material bay
    std::vector<GooItem> goo;         // bay contents by value, priced at market avg
    std::string state_timer_start, state_timer_end;  // reinforcement clock (ISO)
    std::string extraction_start, chunk_arrival;     // Athanor/Tatara moon pull (ISO)
    std::string unanchors_at;         // set while unanchoring (ISO)
    int sv_on = -1, sv_off = -1;      // service counts; -1 = feed doesn't say
    std::vector<RefuelEvent> log;     // this structure's last refuel events
};

// ISK shorthand for inside the gauges: 1.24b / 830.5m / 12k
static std::string isk_compact(double v) {
    char b[32];
    if (v >= 1e9) std::snprintf(b, sizeof b, "%.2fb", v / 1e9);
    else if (v >= 1e6) std::snprintf(b, sizeof b, "%.1fm", v / 1e6);
    else if (v >= 1e3) std::snprintf(b, sizeof b, "%.0fk", v / 1e3);
    else std::snprintf(b, sizeof b, "%.0f", v);
    return b;
}

// Types that carry a secondary fuel besides blocks: Metenox drink magmatic
// gas, gates and cyno beacons burn liquid ozone per jump.
// Display names: the game's mouthfuls shortened for the table (raw names
// still drive the fuel2/scope logic and the wire format).
static std::string display_type(const std::string& t) {
    if (t == "Metenox Moon Drill") return "Metenox";
    if (t == "Orbital Skyhook") return "Skyhook";
    if (t.rfind("Ansiblex", 0) == 0) return "Jump-Bridge";
    if (t.rfind("Pharolux", 0) == 0) return "Cyno Bacon";  // yes, bacon
    return t;
}

// refineries with a moon drill get the Moon Pull line
static bool has_moon_pull(const std::string& display) {
    return display == "Athanor" || display == "Tatara";
}

static double fuel2_days(const Row& r) {
    return r.gas_day > 0 && r.fuel2 >= 0 ? r.fuel2 / r.gas_day : -1;
}

static const char* fuel2_kind(const std::string& type) {
    if (type == "Metenox Moon Drill") return "Magmatic Gas";
    if (type.rfind("Ansiblex", 0) == 0) return "Liquid Ozone";   // "... Jump Bridge"
    if (type.rfind("Pharolux", 0) == 0) return "Liquid Ozone";
    return "";
}

struct Refuel {
    long long sid = 0;
    std::string seen_at, name, system, new_expires, by;
    double days_added = 0;
    double blocks = -1;    // fuel-log estimate of the deposit; -1 = rate unknown
    bool pending = false;  // a filed claim ESI has not shown the jump for yet
};

static std::mutex g_mtx;
static std::vector<Row> g_rows;

// structure notifications (per active tab): a ~10-minute signal vs the hourly
// structures dataset; drives early attack flashes, instant destroyed removal
// and the "new structure anchoring" banner
struct Notif {
    std::string type;
    time_t at = 0;
    long long sid = 0;
    long long system_id = 0;
};
static std::vector<Notif> g_notifs;
static std::vector<Refuel> g_refuels;
// standalone multi-corp: one cached snapshot per corp the logins can read;
// the active one is what ingest() has parsed into the globals above. The tab
// bar only appears when there is more than one.
static std::vector<std::string> g_tab_labels, g_tab_data;
static int g_tab = 0;
// config "tab_type_filter": {"SOUSN": "Metenox Moon Drill", ...}; a corp tab
// starts with that type filter active instead of All
static std::map<std::string, std::string> g_tab_default_filter;

// EVE client chat-log awareness (GUI map overlay): config "eve_logs" points
// at the client's logs dir (auto-detected when empty), "intel_channels" lists
// channel names to tail (empty = any channel with "intel" in the name).
static std::string g_eve_logs_cfg;
static std::vector<std::string> g_intel_channels_cfg;
static std::string g_pulled_at;
static std::string g_corp_name;  // whose structures the active feed shows
static std::string g_fuel2_status;  // ok|relogin|director|error|off - why F² has data or not
static bool g_rentals_online = false;  // moon-rental classification feed reachable this pull
static bool g_extractions_ok = false;  // corp mining extractions readable this pull
static bool g_timers_ok = false;       // feed carries reinforcement timers (corp feed doesn't)
static bool g_showing_cache = false;   // instant-boot snapshot on screen, live sweep pending
static unsigned long long g_data_gen = 0;  // bumped per ingest; GUI re-copies only on change
static std::string g_esi_lastmod, g_esi_expires;  // CCP regenerates hourly
static std::string g_status = "connecting to the box...";
static std::string g_note;         // last claim result, shown for a few seconds
static time_t g_note_at = 0;
static std::atomic<bool> g_run{true};
// GUI build: poked after PostEvent so the GLFW loop redraws; the console
// login prompt is also deferred into the window (see main).
static void (*g_gui_wake)() = nullptr;
static bool g_defer_login = false;
static std::atomic<bool> g_busy{false};

// short-lived background workers (refresh / claim / login). Joined before
// main returns so none of them can touch destroyed locals (the FTXUI screen)
// after the UI exits.
static std::vector<std::thread> g_bg_threads;
static std::mutex g_bg_mtx;
template <typename F>
static void spawn_bg(F&& fn) {
    std::lock_guard<std::mutex> l(g_bg_mtx);
    g_bg_threads.emplace_back(std::forward<F>(fn));
}

// --- helpers ----------------------------------------------------------------
#ifdef _WIN32
// _popen from a -mwindows GUI app pops a visible console window for every
// child, so each curl call flashed a terminal on screen. Spawn through
// CreateProcess with CREATE_NO_WINDOW and read stdout over a pipe instead;
// "cmd /C" keeps _popen's shell semantics (quoting, 2>NUL, redirects), and
// stderr goes to NUL like the old invisible console unless the command
// redirects it itself. The spawn section is mutexed so concurrent fetch
// threads can't leak each other's inheritable pipe ends into their children
// (a leaked write end would hold the pipe open and hang the read forever).
static std::string run_cmd(const char* cmd) {
    std::string out;
    static std::mutex spawn_mtx;
    HANDLE rd = nullptr;
    PROCESS_INFORMATION pi{};
    {
        std::lock_guard<std::mutex> lk(spawn_mtx);
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return out;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr);
        STARTUPINFOA si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = nul;
        si.hStdInput = nullptr;
        std::string cl = std::string("cmd /C \"") + cmd + "\"";
        BOOL ok = CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) {
            CloseHandle(rd);
            return out;
        }
    }
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}
#else
static std::string run_cmd(const char* cmd) {
    std::string out;
    FILE* p = popen(cmd, "r");
    if (!p) return out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}
#endif

static std::string urlenc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

static std::string pad(const std::string& s, int w, bool right = false) {
    // width in display cells, counting UTF-8 code points (good enough here).
    int cells = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) cells++;
    if (cells >= w) {
        // trim to w code points
        std::string o;
        int k = 0;
        for (size_t i = 0; i < s.size();) {
            unsigned char c = s[i];
            int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            if (k >= w) break;
            o.append(s, i, len);
            i += len;
            k++;
        }
        return o;
    }
    std::string p(w - cells, ' ');
    return right ? p + s : s + p;
}


// 0 red, 1 orange, 2 yellow, 3 green, -1 grey/no-data; both UIs map these
static int urgency_band(double days) {
    if (days < 3) return 0;
    if (days < 7) return 1;
    if (days < 14) return 2;
    return 3;
}

// Danger colors are universal; the HEALTHY hue tells the columns apart:
// fuel blocks glow cyan-teal (like the blocks), gas/ozone glows violet.
static Color band_color(int band, bool secondary = false) {
    switch (band) {
        case 0: return Color::RGB(255, 70, 70);
        case 1: return Color::RGB(255, 140, 0);
        case 2: return Color::RGB(250, 215, 70);
        case 3: return secondary ? Color::RGB(172, 128, 255) : Color::RGB(56, 216, 232);
        default: return Color::RGB(128, 136, 150);
    }
}

static Color days_color(double days) { return band_color(urgency_band(days)); }

// -1 grey (no data), else an urgency band for the secondary fuel
static int fuel2_band(const Row& r) {
    if (r.fuel2_name.empty() || r.fuel2 < 0) return -1;
    if (r.fuel2 <= 0) return 0;
    if (double d = fuel2_days(r); d >= 0) return urgency_band(d);
    if (r.lo_min > 0 && r.fuel2 < r.lo_min) return 1;
    if (r.lo_target > 0 && r.fuel2 < r.lo_target) return 2;
    return 3;
}

static Color fuel_color(const Row& r) {
    if (!r.has_fuel) return Color::RGB(128, 136, 150);
    return days_color(r.days);
}

// Usage-adjusted days left when burn history exists, else ESI's flat number.
static double effective_days(const Row& r) { return r.est >= 0 ? r.est : r.days; }

static std::string commas(double v) {
    char raw[32];
    std::snprintf(raw, sizeof raw, "%.0f", v);
    std::string s = raw, o;
    int lead = (int)s.size() % 3;
    for (int i = 0; i < (int)s.size(); i++) {
        if (i && (i - lead) % 3 == 0) o += ',';
        o += s[i];
    }
    return o;
}

// Haul to 30d: UNITS = fuel blocks, M3 = their volume (blocks are 5 m3).
// Only gates/beacons have a verified flat rate; everything else shows --.
static std::string units_raw(const Row& r) {
    if (r.need < 0) return "--";
    if (r.need == 0) return "ok";
    return commas(r.need);
}



static Color fuel2_color(const Row& r) { return band_color(fuel2_band(r), true); }

static Color need_color(const Row& r) {
    if (r.need < 0) return Color::RGB(128, 136, 150);
    if (r.need == 0) return Color::RGB(90, 225, 130);
    return days_color(r.has_fuel ? r.days : -1);
}

// Effective burn: prefer the 7d window, fall back to 30d (detail line only).
static double burn_of(const Row& r) { return r.burn7 >= 0 ? r.burn7 : r.burn30; }

// A gauge with its value written INSIDE: label chars ride the bar, showing
// inverted on the filled part and colored on the empty part. frac < 0 means
// "no gauge": the label renders centered and grey.
static const double GAUGE_DAYS = 30.0;  // every days gauge renders on this scale

static int cells(const std::string& s) {  // display cells = UTF-8 codepoints here
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

static std::string cells_prefix(const std::string& s, int n) {
    int seen = 0;
    size_t i = 0;
    while (i < s.size()) {
        if ((s[i] & 0xC0) != 0x80) {
            if (seen == n) break;
            seen++;
        }
        i++;
    }
    while (i < s.size() && (s[i] & 0xC0) == 0x80) i++;
    return s.substr(0, i);
}

// left-anchored + right-anchored text composed to exactly `width` cells
static std::string two_sided(const std::string& l, const std::string& r, int width) {
    int cl = cells(l), cr = cells(r);
    if (cl + cr + 1 > width) return pad(l, width);  // no room: keep the left value
    return l + std::string((size_t)(width - cl - cr), ' ') + r;
}

static Element bar_with_text(std::string label, double frac, Color col, int width) {
    if (cells(label) > width) label = cells_prefix(label, width);
    if (frac < 0) {  // no gauge: just the label, centered, in the caller's color
        int lp = std::max(0, (width - cells(label)) / 2);
        return text(pad(std::string((size_t)lp, ' ') + label, width)) | color(col);
    }
    if (frac > 1) frac = 1;
    // at most four style runs: label-on-fill, label-past-fill, fill, empty tail
    int fill = (int)std::lround(frac * width);
    int lab = cells(label);
    int on_fill = std::min(lab, fill);
    int past_fill = lab - on_fill;
    int blank_fill = std::max(0, fill - lab);
    int tail = width - std::max(fill, lab);
    std::string head = cells_prefix(label, on_fill);
    Elements runs;
    if (on_fill)
        runs.push_back(text(head) | bgcolor(col) | color(Color::RGB(10, 10, 16)) | bold);
    if (past_fill) runs.push_back(text(label.substr(head.size())) | color(col) | bold);
    if (blank_fill) runs.push_back(text(std::string((size_t)blank_fill, ' ')) | bgcolor(col));
    if (tail > 0) {
        std::string t;
        for (int i = 0; i < tail; i++) t += "░";  // 3 UTF-8 bytes per cell
        runs.push_back(text(t) | color(col) | dim);
    }
    return hbox(runs);
}

// 1,234,567 -> "1.2M"; 14,900 -> "14.9k"; keeps tiny numbers plain
static std::string compact_units(double v) {
    char b[32];
    if (v >= 1e6) std::snprintf(b, sizeof b, "%.1fM", v / 1e6);
    else if (v >= 1e4) std::snprintf(b, sizeof b, "%.1fk", v / 1e3);
    else std::snprintf(b, sizeof b, "%.0f", v);
    return b;
}

// GAS-2-30D column: secondary fuel to haul. Metenox: gas to reach 30 days at
// the drill rate; gates/beacons: liquid ozone to reach the doctrine fill-to.
static std::string gas30_raw(const Row& r) {
    double need = -1;
    if (r.gas_day > 0)
        need = std::max(0.0, std::round((GAUGE_DAYS - fuel2_days(r)) * r.gas_day));
    else if (r.lo_target > 0)
        need = std::max(0.0, r.lo_target - r.fuel2);
    if (need < 0) return "";
    if (need == 0) return "ok";
    std::string s = commas(need);
    if ((int)s.size() > 7) s = compact_units(need);
    return s;
}

// One 30-day gauge scale for every days-based bar on the desk; `right` rides
// the far end of the bar (the haul needed to reach 30 days)
static Element days_gauge(double days, const std::string& right, Color col, int width) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1fd", days);
    return bar_with_text(two_sided(b, right, width), std::max(0.0, days / GAUGE_DAYS),
                         col, width);
}

// F cell: fuel gauge, days inside on the left, blocks-to-30d on the right
static Element fuel_cell(const Row& r, int width) {
    if (!r.has_fuel) return bar_with_text("--", -1, fuel_color(r), width);
    return days_gauge(r.days, units_raw(r), fuel_color(r), width);
}

// F² cell: secondary fuel gauge. Metenox gas has a burn rate, so it shows
// days remaining on the same 30d scale; ozone has no flat rate, so the bar
// fills against the doctrine fill-to target with the stock inside.
static Element fuel2_cell(const Row& r, int width) {
    if (r.fuel2_name.empty()) return bar_with_text("-", -1, fuel2_color(r), width);
    if (r.fuel2 < 0) return bar_with_text("?", -1, fuel2_color(r), width);
    if (r.gas_day > 0) return days_gauge(fuel2_days(r), gas30_raw(r), fuel2_color(r), width);
    double frac = r.lo_target > 0 ? r.fuel2 / r.lo_target : -1;
    return bar_with_text(two_sided(compact_units(r.fuel2), gas30_raw(r), width), frac,
                         fuel2_color(r), width);
}


// Parse "2026-07-08T05:00:23..." to time_t (UTC), ignoring fractional/offset.
static time_t parse_iso(const std::string& s) {
    if (s.size() < 19) return 0;
    std::tm tm{};
    try {
        tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(s.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(s.substr(8, 2));
        tm.tm_hour = std::stoi(s.substr(11, 2));
        tm.tm_min = std::stoi(s.substr(14, 2));
        tm.tm_sec = std::stoi(s.substr(17, 2));
    } catch (...) {
        return 0;  // not a date; callers already treat 0 as unknown
    }
    return timegm(&tm);
}

#include "standalone.hpp"  // needs run_cmd/urlenc/parse_iso above
#ifdef STOKER_GUI
#include "gui_host.hpp"  // native-window host for the same component
#endif

// --- self-update against GitHub releases --------------------------------------
// Startup checks the latest release once (config "update_check": false skips);
// when a newer tag exists the header offers [u], which downloads the matching
// platform asset and swaps it over the running binary (Windows: the running
// exe is renamed aside first, and the leftover .old is removed on next start).
static const char* STOKER_VERSION = "v2.6.2";
static const char* UPDATE_REPO = "niko-aubaris/stoker";
static bool g_update_check = true;
static std::string g_update_tag, g_update_url;  // set once by the worker (g_mtx)

static bool ver_newer(const std::string& a, const std::string& b) {
    // dotted-number compare, "v1.0.10" style; true when a > b
    auto nums = [](const std::string& s) {
        std::vector<long> v;
        long cur = -1;
        for (char c : s) {
            if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
            else if (cur >= 0) { v.push_back(cur); cur = -1; }
        }
        if (cur >= 0) v.push_back(cur);
        return v;
    };
    auto va = nums(a), vb = nums(b);
    for (size_t i = 0; i < std::max(va.size(), vb.size()); i++) {
        long x = i < va.size() ? va[i] : 0, y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

static void check_update() {
    if (!g_update_check) return;
    int st = 0;
    json j;
    try {
        j = json::parse(standalone::http_get(
            "https://api.github.com/repos/" + std::string(UPDATE_REPO) +
                "/releases/latest", "", st));
    } catch (...) { return; }
    if (st != 200 || !j.is_object()) return;
    std::string tag = j.value("tag_name", "");
    if (tag.empty() || !ver_newer(tag, STOKER_VERSION)) return;
#ifdef _WIN32
    const char* want = "windows-x64.zip";
#else
    const char* want = "linux-x86_64.tar.gz";
#endif
    for (auto& a : j.value("assets", json::array())) {
        if (a.value("name", std::string()).find(want) == std::string::npos) continue;
        std::lock_guard<std::mutex> l(g_mtx);
        g_update_tag = tag;
        g_update_url = a.value("browser_download_url", "");
        return;
    }
}

static std::filesystem::path own_exe() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return n ? std::filesystem::path(buf) : std::filesystem::path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p;
#endif
}

// Download + swap. Returns the note-line text; never throws.
static std::string apply_update(const std::string& tag, const std::string& url) try {
    auto exe = own_exe();
    if (exe.empty()) return "update failed: cannot locate own executable";
    auto dir = standalone::config_dir() / "update";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
#ifdef _WIN32
    auto pkg = dir / "pkg.zip";
#else
    auto pkg = dir / "pkg.tar.gz";
#endif
    auto fresh = dir / exe.filename();  // stoker or stoker-gui, whichever we are
if (!standalone::download_file(url, pkg, 100000))
        return "update failed: download incomplete";
    // Windows 10+ ships bsdtar as tar.exe, which also reads zip
    run_cmd(("tar -xf \"" + pkg.string() + "\" -C \"" + dir.string() + "\"" QUIET).c_str());
    if (!std::filesystem::exists(fresh))
        return "update failed: archive did not contain the binary";
#ifndef _WIN32
    std::filesystem::permissions(fresh,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
        std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
        std::filesystem::perms::others_exec, ec);
#endif
    // stage beside the running binary so the final rename is same-filesystem
    auto staged = exe;
    staged += ".new";
    std::filesystem::copy_file(fresh, staged,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return "update failed: cannot write in " + exe.parent_path().string();
#ifdef _WIN32
    auto old = exe;
    old += ".old";
    std::filesystem::remove(old, ec);
    if (!MoveFileExA(exe.string().c_str(), old.string().c_str(), MOVEFILE_REPLACE_EXISTING))
        return "update failed: cannot move the running exe aside";
    if (!MoveFileExA(staged.string().c_str(), exe.string().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExA(old.string().c_str(), exe.string().c_str(), MOVEFILE_REPLACE_EXISTING);
        return "update failed: cannot move the new exe in place";
    }
#else
    std::filesystem::rename(staged, exe, ec);
    if (ec) return "update failed: cannot replace the binary";
#endif
    std::filesystem::remove_all(dir, ec);
    return "updated to " + tag + " - restart STOKER to run it";
} catch (const std::exception& e) {
    return std::string("update failed: ") + e.what();
}
#include "splash_frames.hpp"

static std::string rel_age(const std::string& iso) {
    time_t t = parse_iso(iso);
    if (!t) return "?";
    long secs = (long)(time(nullptr) - t);
    if (secs < 0) secs = 0;
    if (secs < 90) return std::to_string(secs) + "s ago";
    if (secs < 3600) return std::to_string(secs / 60) + "m ago";
    if (secs < 86400) {
        long h = secs / 3600, m = (secs % 3600) / 60;
        return std::to_string(h) + "h" + (m ? std::to_string(m) + "m" : "") + " ago";
    }
    long d = secs / 86400, h = (secs % 86400) / 3600;
    return std::to_string(d) + "d" + (h ? std::to_string(h) + "h" : "") + " ago";
}

// --- intro splash: fuel blocks off the shovel, reactor lights up ------------
// Two embedded pixel-art frames (art/splash-load.png, art/splash-ignite.png,
// regenerate via tools/make_splash_frames.py) rendered as half-blocks: each
// character cell is two vertical "pixels" (▀ with fg = top, bg = bottom), so
// the 96x72 canvas fits in 96x36 characters. Storyboard: loaded shovel with
// falling blocks -> the drop lands and ignition dissolves up out of the pit ->
// the reactor column pulses with arc light until data arrives.
static uint32_t splash_hash(int x, int y, int f) {
    uint32_t h = (uint32_t)(x * 374761393u) ^ (uint32_t)(y * 668265263u)
               ^ (uint32_t)(f * 2246822519u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static Element splash_scene(long ms, const std::string& status) {
    constexpr int PW = SPLASH_W, PH = SPLASH_H;
    int fframe = (int)(ms / 100);
    double t = ms / 1000.0;

    // 0-1.9s   frame A: blocks shimmer, loose chips rain into the pit
    // 1.9-2.7s A -> B pixel dissolve, biased so ignition climbs out of the pit
    // 2.7s+    frame B: blue arc energy pulses, sparks pop
    double mix = t < 1.9 ? 0.0 : t > 2.7 ? 1.0 : (t - 1.9) / 0.8;
    mix = mix * mix * (3 - 2 * mix);

    auto clamp8 = [](double v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); };
    Elements rows;
    for (int ty = 0; ty < PH / 2; ty++) {
        Elements segs;
        Color cur_fg, cur_bg;
        std::string run;
        auto flush = [&]() {
            if (!run.empty()) segs.push_back(text(run) | color(cur_fg) | bgcolor(cur_bg));
            run.clear();
        };
        for (int x = 0; x < PW; x++) {
            Color two[2];
            for (int half = 0; half < 2; half++) {
                int y = ty * 2 + half;
                // dissolve threshold leans on depth so the bottom flips first
                double th = (splash_hash(x, y, 7) % 1000) / 1000.0 * 0.75 +
                            (1.0 - (double)y / PH) * 0.25;
                bool lit = mix >= 1.0 || (mix > 0.0 && th < mix);
                const unsigned char* p = (lit ? SPLASH_IGNITE : SPLASH_LOAD) +
                                         ((size_t)y * PW + x) * 3;
                double r = p[0], g = p[1], b = p[2];
                if (!lit) {
                    // cyan fuel blocks shimmer while they tumble
                    if (b > 150 && b > r + 30) {
                        double k = 1.0 + 0.12 * (((splash_hash(x, y, fframe / 2) % 100) / 100.0) - 0.5);
                        r *= k; g *= k; b *= k;
                    }
                } else {
                    // arc light breathes; occasional white spark on bright energy
                    if (b > 150 && b > r + 40) {
                        double k = 1.0 + 0.18 * std::sin(ms / 170.0 + (splash_hash(x, y, 9) % 628) / 100.0);
                        r *= k; g *= k; b *= k;
                        if (splash_hash(x, y, fframe) % 1499 < 2) { r = 255; g = 255; b = 255; }
                    }
                }
                two[half] = Color::RGB(clamp8(r), clamp8(g), clamp8(b));
            }
            // loose cyan chips raining from the shovel lip into the pit (frame A)
            if (mix < 1.0)
                for (int i = 0; i < 7; i++) {
                    int cx = 38 + (int)(splash_hash(i, 41, 0) % 30);
                    int cy = 36 + (int)((fframe * 2 + splash_hash(i, 43, 0)) % 26);
                    for (int half = 0; half < 2; half++)
                        if (cx == x && cy == ty * 2 + half && mix == 0.0)
                            two[half] = (i & 1) ? Color::RGB(0, 229, 255)
                                                : Color::RGB(140, 220, 255);
                }
            if (run.empty()) { cur_fg = two[0]; cur_bg = two[1]; }
            else if (!(two[0] == cur_fg && two[1] == cur_bg)) { flush(); cur_fg = two[0]; cur_bg = two[1]; }
            run += "▀";
        }
        flush();
        rows.push_back(hbox(segs));
    }
    rows.push_back(text(""));
    rows.push_back(hbox({filler(),
                         text("stoking the furnace" + std::string((ms / 400) % 4, '.'))
                             | color(NEON_DIM_CYAN),
                         filler()}));
    if (!status.empty())
        rows.push_back(hbox({filler(), text(status) | color(Color::RGB(255, 70, 70)), filler()}));
    return vbox({filler(), hbox({filler(), vbox(rows), filler()}), filler()}) | borderRounded;
}

// --- fetch / parse ----------------------------------------------------------
static void ingest(const std::string& raw) {
    if (raw.empty()) { std::lock_guard<std::mutex> l(g_mtx); g_status = "no data (network? auth?)"; return; }
    json d;
    try { d = json::parse(raw); }
    catch (...) { std::lock_guard<std::mutex> l(g_mtx); g_status = "bad JSON from box"; return; }
    std::vector<Row> rows;
    for (auto& s : d.value("structures", json::array())) try {
        Row r;
        if (s.contains("structure_id") && !s["structure_id"].is_null())
            r.sid = s["structure_id"].get<long long>();
        r.name = s.value("name", "");
        r.system = s.value("system", "");
        r.type = s.value("type", "");
        r.state = s.value("state", "");
        r.state_timer_start = s.value("state_timer_start", "");
        r.state_timer_end = s.value("state_timer_end", "");
        r.unanchors_at = s.value("unanchors_at", "");
        if (s.contains("services_online") && !s["services_online"].is_null())
            r.sv_on = s["services_online"].get<int>();
        if (s.contains("services_offline") && !s["services_offline"].is_null())
            r.sv_off = s["services_offline"].get<int>();
        r.extraction_start = s.value("extraction_start", "");
        r.chunk_arrival = s.value("chunk_arrival", "");
        r.services = s.value("services", "");
        r.fuel_expires = s.value("fuel_expires", "");
        if (s.contains("fuel_days_left") && !s["fuel_days_left"].is_null()) {
            r.has_fuel = true;
            r.days = s["fuel_days_left"].get<double>();
        }
        // recompute the clock locally: feeds bake fuel_days_left at fetch time,
        // and the instant-boot snapshot cache can replay hours-old data
        if (!r.fuel_expires.empty()) {
            if (time_t fe = parse_iso(r.fuel_expires)) {
                r.has_fuel = true;
                r.days = (double)(fe - time(nullptr)) / 86400.0;
            }
        }
        if (s.contains("burn_7d") && !s["burn_7d"].is_null()) r.burn7 = s["burn_7d"].get<double>();
        if (s.contains("burn_30d") && !s["burn_30d"].is_null()) r.burn30 = s["burn_30d"].get<double>();
        if (s.contains("est_fuel_days") && !s["est_fuel_days"].is_null()) r.est = s["est_fuel_days"].get<double>();
        if (s.contains("blocks_to_30d") && !s["blocks_to_30d"].is_null()) r.need = s["blocks_to_30d"].get<double>();
        if (s.contains("m3_to_30d") && !s["m3_to_30d"].is_null()) r.m3 = s["m3_to_30d"].get<double>();
        r.fuel2_name = fuel2_kind(r.type);
        r.type = display_type(r.type);
        if (s.contains("fuel2_units") && !s["fuel2_units"].is_null())
            r.fuel2 = s["fuel2_units"].get<double>();
        r.rental = s.value("rental", "");
        r.renter = s.value("renter", "");
        auto numf = [&s](const char* k) -> double {
            return (s.contains(k) && !s[k].is_null()) ? s[k].get<double>() : -1.0;
        };
        r.goo_m3 = numf("goo_m3");
        r.goo_cap = numf("goo_capacity");
        r.goo_isk = numf("goo_isk");
        if (s.contains("goo") && s["goo"].is_array())
            for (auto& g : s["goo"]) {
                GooItem gi;
                gi.name = g.value("name", "");
                gi.qty = g.value("qty", 0.0);
                gi.m3 = g.value("m3", 0.0);
                gi.isk = g.value("isk", 0.0);
                r.goo.push_back(std::move(gi));
            }
        r.bpd = numf("blocks_per_day");
        r.blocks_now = numf("blocks_now");
        r.m3_now = numf("m3_now");
        r.lo_min = numf("lo_min");
        r.lo_target = numf("lo_target");
        r.gas_day = numf("gas_per_day");
        r.gas_month = numf("gas_month_units");
        r.gas_m3 = numf("gas_month_m3");
        if (s.contains("refuel_log") && s["refuel_log"].is_array())
            for (auto& e : s["refuel_log"]) {
                RefuelEvent ev;
                ev.seen_at = e.value("seen_at", "");
                if (e.contains("days_added") && !e["days_added"].is_null())
                    ev.days_added = e["days_added"].get<double>();
                if (e.contains("by") && !e["by"].is_null()) ev.by = e["by"].get<std::string>();
                r.log.push_back(std::move(ev));
            }
        if (s.contains("last_refuel") && !s["last_refuel"].is_null()) {
            r.has_refuel = true;
            r.last_refuel = s["last_refuel"].get<std::string>();
            if (s.contains("last_refuel_days_added") && !s["last_refuel_days_added"].is_null())
                r.refuel_added = s["last_refuel_days_added"].get<double>();
        }
        rows.push_back(std::move(r));
    } catch (const std::exception&) { /* skip malformed entry */ }
    std::vector<Refuel> refuels;
    // filed claims still waiting for ESI to show the jump sit on top of the log
    for (auto& e : d.value("pending_claims", json::array())) try {
        Refuel v;
        v.pending = true;
        v.seen_at = e.value("created_at", "");
        v.name = e.value("name", "?");
        v.system = e.value("system", "?");
        if (e.contains("structure_id") && !e["structure_id"].is_null())
            v.sid = e["structure_id"].get<long long>();
        if (e.contains("by") && !e["by"].is_null()) v.by = e["by"].get<std::string>();
        refuels.push_back(std::move(v));
    } catch (const std::exception&) {}
    for (auto& e : d.value("refuels", json::array())) try {
        Refuel v;
        v.seen_at = e.value("seen_at", "");
        v.name = e.value("name", "?");
        v.system = e.value("system", "?");
        v.new_expires = e.value("new_expires", "");
        if (e.contains("structure_id") && !e["structure_id"].is_null())
            v.sid = e["structure_id"].get<long long>();
        if (e.contains("by") && !e["by"].is_null()) v.by = e["by"].get<std::string>();
        if (e.contains("blocks") && !e["blocks"].is_null())
            v.blocks = e["blocks"].get<double>();
        if (e.contains("days_added") && !e["days_added"].is_null())
            v.days_added = e["days_added"].get<double>();
        refuels.push_back(std::move(v));
    } catch (const std::exception&) {}
    for (auto& v : refuels)
        if (v.blocks < 0 && v.days_added > 0 && v.sid)
            for (auto& r : rows)
                if (r.sid == v.sid) {
                    if (r.bpd > 0) v.blocks = std::round(v.days_added * r.bpd);
                    break;
                }
    std::vector<Notif> notifs;
    for (auto& e : d.value("notifications", json::array())) try {
        Notif n;
        n.type = e.value("type", "");
        n.at = parse_iso(e.value("timestamp", ""));
        if (e.contains("structure_id") && !e["structure_id"].is_null())
            n.sid = e["structure_id"].get<long long>();
        if (e.contains("system_id") && !e["system_id"].is_null())
            n.system_id = e["system_id"].get<long long>();
        notifs.push_back(std::move(n));
    } catch (const std::exception&) {}
    std::lock_guard<std::mutex> l(g_mtx);
    g_rows = std::move(rows);
    g_refuels = std::move(refuels);
    g_notifs = std::move(notifs);
    g_pulled_at = d.value("pulled_at", "");
    g_corp_name = d.value("corp", "");
    g_fuel2_status = d.value("fuel2_status", "");
    g_rentals_online = d.value("rentals_online", false);
    g_extractions_ok = d.value("extractions_ok", false);
    g_timers_ok = d.value("timers_ok", false);
    g_data_gen++;
    g_esi_lastmod = d.contains("esi_last_modified") && !d["esi_last_modified"].is_null()
                        ? d["esi_last_modified"].get<std::string>() : "";
    g_esi_expires = d.contains("esi_expires") && !d["esi_expires"].is_null()
                        ? d["esi_expires"].get<std::string>() : "";
    if (d.contains("error"))
        g_status = "box error: " + d.value("error", "");
    else if (d.value("refreshing", false))
        g_status = "server pulling fresh ESI - data lands within ~1 min";
    else if (g_showing_cache)  // tab switches re-ingest cached data; keep the notice
        g_status = "showing cached data - refreshing from ESI...";
    else
        g_status = "";
}

// instant boot: the last good snapshot set is kept on disk and painted right
// away while the real ESI sweep runs; the sweep replaces it when it lands
static void save_snap_cache(const std::vector<standalone::Snap>& snaps) {
    bool any = false;
    for (auto& s : snaps) any = any || !s.label.empty();
    if (!any) return;  // never cache the no-logins error placeholder
    try {
        namespace fs = std::filesystem;
        json j = json::array();
        // a corp that failed THIS sweep keeps its previous cached snapshot:
        // stale beats vanished on the next instant boot
        std::vector<std::string> have;
        for (auto& s : snaps) {
            j.push_back({{"label", s.label}, {"data", s.data}});
            have.push_back(s.label);
        }
        try {
            json old = standalone::load_json_file(standalone::config_dir() / "snap-cache.json");
            if (old.is_array())
                for (auto& e : old) {
                    std::string lbl = e.value("label", "");
                    if (!lbl.empty() &&
                        std::find(have.begin(), have.end(), lbl) == have.end())
                        j.push_back(e);
                }
        } catch (...) {}
        // atomic + private: tmp in the same dir, chmod, rename over the old
        fs::path p = standalone::config_dir() / "snap-cache.json";
        fs::path tmp = standalone::config_dir() / "snap-cache.json.tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << j.dump();
        }
        std::error_code ec;
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
        fs::rename(tmp, p, ec);
    } catch (...) {}
}

static bool load_snap_cache() {
    try {
        json j = standalone::load_json_file(standalone::config_dir() / "snap-cache.json");
        if (!j.is_array() || j.empty()) return false;
        std::string active;
        {
            std::lock_guard<std::mutex> l(g_mtx);
            g_tab_labels.clear();
            g_tab_data.clear();
            for (auto& e : j) {
                g_tab_labels.push_back(e.value("label", ""));
                g_tab_data.push_back(e.value("data", ""));
            }
            if (g_tab >= (int)g_tab_data.size()) g_tab = 0;
            if (!g_tab_data.empty()) active = g_tab_data[g_tab];
        }
        if (active.empty()) return false;
        {
            std::lock_guard<std::mutex> l(g_mtx);
            g_showing_cache = true;
        }
        ingest(active);  // ingest's tail keeps the cached-data notice up
        return true;
    } catch (...) {
        return false;
    }
}

// Pull every reachable corp, cache the snapshots for the tab bar, ingest the
// active tab's. Used by both the poll loop and the manual refresh.
static void standalone_cycle() {
    auto snaps = standalone::fetch_snapshots(g_client_id);
    bool ok = false;
    for (auto& s : snaps) ok = ok || !s.label.empty();
    if (!ok) {
        // total failure (offline boot, expired logins): keep whatever is on
        // screen - especially the instant-boot cache - instead of wiping it
        std::string err;
        try { err = json::parse(snaps[0].data).value("error", ""); } catch (...) {}
        std::lock_guard<std::mutex> l(g_mtx);
        if (!g_tab_data.empty()) {
            g_status = (err.empty() ? std::string("refresh failed") : err) +
                       " - showing previous data";
            return;
        }
    }
    if (ok) {
        save_snap_cache(snaps);
        std::lock_guard<std::mutex> l(g_mtx);
        g_showing_cache = false;  // live data is about to land
    }
    std::string active;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_tab_labels.clear();
        g_tab_data.clear();
        for (auto& s : snaps) {
            g_tab_labels.push_back(s.label);
            g_tab_data.push_back(std::move(s.data));
        }
        if (g_tab >= (int)g_tab_data.size()) g_tab = 0;
        if (!g_tab_data.empty()) active = g_tab_data[g_tab];
    }
    ingest(active);
}

static void worker() {
    bool update_checked = false;
    if (g_standalone) load_snap_cache();  // paint the last session's data now
    while (g_run) {
        if (g_standalone) {
            // a manual refresh (g_busy) may already be mid-cycle: don't run a
            // second concurrent sweep against the same tokens and cache file
            if (!g_busy) {
                g_busy = true;
                standalone_cycle();
                g_busy = false;
            }
        } else
            ingest(standalone::http_get_body(g_fetch_url));
        if (!update_checked) {
            update_checked = true;
            check_update();
        }
        const int secs = g_standalone ? ESI_REFRESH_SECONDS : REFRESH_SECONDS;
        for (int i = 0; i < secs * 4 && g_run; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// One shared EVE dev app for every STOKER user (PKCE public client, so the
// id is safe to publish; each user still logs in as their own character).
// Empty = no default; the first-run wizard asks for one.
static const char* DEFAULT_CLIENT_ID = "1f06202c894448dd81c7f05eaeec63a5";

// Interactive corp-endpoint entry (run with --corp, or first run of a fork
// built without a default client id).
static bool corp_setup(json& cfg, const std::filesystem::path& cfg_path,
                       std::string& endpoint, std::string& key) {
    std::printf("corp-endpoint setup\nendpoint URL: ");
    if (!std::getline(std::cin, endpoint) || endpoint.empty()) return false;
    std::printf("access key: ");
    if (!std::getline(std::cin, key) || key.empty()) return false;
    cfg["endpoint"] = endpoint;
    cfg["key"] = key;
    standalone::save_json_file(cfg_path, cfg);
    std::printf("saved to %s\n", cfg_path.string().c_str());
    return true;
}

// --- config -----------------------------------------------------------------
// Resolve the data source: env vars > config file > baked-in shared dev app.
// Zero questions on a normal first run: straight to the EVE browser login.
// Corp mode wins when both a key and a client id are present.
static bool load_or_setup(bool force_corp) {
    auto dir = standalone::config_dir();
    auto cfg_path = dir / "config.json";
    json cfg = standalone::load_json_file(cfg_path);
    const char* e;
    if ((e = std::getenv("STOKER_ENDPOINT")) && *e) cfg["endpoint"] = e;
    if ((e = std::getenv("STOKER_KEY")) && *e) cfg["key"] = e;
    if ((e = std::getenv("STOKER_CLIENT_ID")) && *e) cfg["client_id"] = e;
    if (cfg.contains("scopes") && cfg["scopes"].is_string())
        standalone::g_scopes = cfg["scopes"].get<std::string>();
    if (cfg.contains("history_api") && cfg["history_api"].is_string())
        standalone::g_history_api = cfg["history_api"].get<std::string>();
    if (cfg.contains("rentals_api") && cfg["rentals_api"].is_string())
        standalone::g_rentals_api = cfg["rentals_api"].get<std::string>();
    if (cfg.contains("rentals_token") && cfg["rentals_token"].is_string())
        standalone::g_rentals_token = cfg["rentals_token"].get<std::string>();
    if (cfg.contains("rentals_corp_id") && cfg["rentals_corp_id"].is_number())
        standalone::g_rentals_corp = cfg["rentals_corp_id"].get<long long>();
    if (cfg.contains("eve_logs") && cfg["eve_logs"].is_string())
        g_eve_logs_cfg = cfg["eve_logs"].get<std::string>();
    if (cfg.contains("intel_channels") && cfg["intel_channels"].is_array())
        for (auto& v : cfg["intel_channels"])
            if (v.is_string()) g_intel_channels_cfg.push_back(v.get<std::string>());
    if (cfg.contains("update_check") && cfg["update_check"].is_boolean())
        g_update_check = cfg["update_check"].get<bool>();
    if (cfg.contains("tab_type_filter") && cfg["tab_type_filter"].is_object())
        for (auto& [k, v] : cfg["tab_type_filter"].items())
            if (v.is_string()) g_tab_default_filter[k] = v.get<std::string>();
    std::string endpoint = cfg.value("endpoint", "");
    std::string key = cfg.value("key", "");
    g_client_id = cfg.value("client_id", "");

    if (force_corp && !corp_setup(cfg, cfg_path, endpoint, key)) return false;

    if (key.empty() && g_client_id.empty()) {
        if (*DEFAULT_CLIENT_ID) {
            // shared dev app; not saved to config so new builds can rotate it
            g_client_id = DEFAULT_CLIENT_ID;
        } else {
            std::printf(
                "STOKER first-run setup (config: %s)\n\n"
                "  [1] corp mode: you were given an access key for a hosted BPOS endpoint\n"
                "  [2] standalone: log in with your own EVE character.\n"
                "      Needs a free developer app from https://developers.eveonline.com\n"
                "      (callback URL http://localhost:8420/callback, scope\n"
                "      esi-corporations.read_structures.v1) and the Station_Manager\n"
                "      in-game role on your character.\n\n"
                "mode [1/2]: ",
                cfg_path.string().c_str());
            std::string mode, val;
            if (!std::getline(std::cin, mode)) return false;
            if (mode == "1") {
                if (!corp_setup(cfg, cfg_path, endpoint, key)) return false;
            } else if (mode == "2") {
                std::printf("dev-app client id: ");
                if (!std::getline(std::cin, val) || val.empty()) return false;
                g_client_id = val;
                cfg["client_id"] = val;
                standalone::save_json_file(cfg_path, cfg);
                std::printf("saved.\n");
            } else {
                return false;
            }
        }
    }

    if (!key.empty()) {
        if (endpoint.empty()) {
            std::fprintf(stderr, "corp mode needs an \"endpoint\" in %s\n",
                         cfg_path.string().c_str());
            return false;
        }
        g_standalone = false;
        // strip characters that would break out of the double-quoted curl
        // command; keys and endpoints are plain URL material anyway
        for (auto* s : {&endpoint, &key})
            s->erase(std::remove_if(s->begin(), s->end(),
                     [](char c) { return c == '"' || c == '\\' || c == '$' || c == '`'; }),
                     s->end());
        std::string url = endpoint + "?k=" + urlenc(key);
        g_fetch_url = url;
        g_refresh_url = url + "&refresh=1";
        g_claim_url = endpoint + "/claim?k=" + urlenc(key);
        return true;
    }

    g_standalone = true;
    if (!standalone::have_login()) {
        if (g_defer_login) return true;  // GUI runs the login inside the window
        std::string err;
        if (!standalone::login(g_client_id, err)) {
            std::fprintf(stderr, "EVE login failed: %s\n", err.c_str());
            return false;
        }
    }
    return true;
}

#ifndef STOKER_IMGUI
// --- main -------------------------------------------------------------------
int main(int argc, char** argv) {
#ifdef _WIN32
    // Box-drawing glyphs come out as mojibake without a UTF-8 console.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
#ifdef STOKER_GUI
    g_defer_login = true;                      // SSO runs inside the window
    g_gui_wake = [] { glfwPostEmptyEvent(); };  // benign before glfwInit
#endif
    if (argc > 1 && std::string(argv[1]) == "--nettest") {
        std::printf("STOKER %s network self-test\n\n", STOKER_VERSION);
        auto probe = [](const char* label, const std::string& url) {
            int st = 0;
            standalone::http_get(url, "", st);
            std::printf("  %-36s %s (HTTP %d)\n", label, st > 0 ? "OK" : "FAIL", st);
            if (st <= 0)
                std::printf("      -> %s\n", standalone::curl_error("\"" + url + "\"").c_str());
        };
        probe("EVE login (login.eveonline.com)",
              "https://login.eveonline.com/.well-known/oauth-authorization-server");
        probe("EVE ESI (esi.evetech.net)",
              std::string(standalone::ESI) + "/status/?datasource=tranquility");
        probe("updates (api.github.com)",
              "https://api.github.com/repos/" + std::string(UPDATE_REPO) + "/releases/latest");
        if (!standalone::g_history_api.empty())
            probe("refuel history service", standalone::g_history_api + "/refuels?corp_id=1");
        std::printf("\nOK means the connection works (any HTTP status is a real reply).\n"
                    "[press Enter to close]");
        std::string pause;
        std::getline(std::cin, pause);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--version") {
        std::printf("STOKER %s\n", STOKER_VERSION);
        return 0;
    }
    // curl is the only external dependency (HTTP for the feed, ESI, SSO, and
    // updates). Naked Windows ships it since Windows 10 1803; say something
    // readable instead of a silently empty dashboard when it's missing.
    if (run_cmd("curl --version" QUIET).rfind("curl", 0) != 0) {
        std::printf(
            "STOKER needs curl, which was not found.\n\n"
#ifdef _WIN32
            "curl.exe ships with Windows 10 version 1803 (April 2018) and later.\n"
            "Update Windows, or install curl from https://curl.se/windows/\n"
#else
            "Install it with your package manager (e.g. sudo apt install curl).\n"
#endif
            "\n[press Enter to close]");
#if defined(_WIN32) && defined(STOKER_GUI)
        MessageBoxA(nullptr, "STOKER needs curl.exe, which ships with Windows 10 "
                    "version 1803 and later. Update Windows or install curl from "
                    "https://curl.se/windows/", "STOKER", MB_ICONERROR);
#else
        std::string pause;
        std::getline(std::cin, pause);
#endif
        return 1;
    }
    {   // clear the renamed-aside exe a Windows self-update leaves behind
        std::error_code ec;
        auto old_exe = own_exe();
        if (!old_exe.empty()) {
            old_exe += ".old";
            std::filesystem::remove(old_exe, ec);
        }
    }
    const bool force_corp = argc > 1 && std::string(argv[1]) == "--corp";
    const bool add_char = argc > 1 && std::string(argv[1]) == "--add";
    if (!load_or_setup(force_corp)) {
        // launched from a desktop icon the terminal vanishes on exit; let the
        // error be read first
        std::printf("\n[press Enter to close]");
        std::string pause;
        std::getline(std::cin, pause);
        return 1;
    }
    // --add: log in one more character (each character's corp becomes a tab)
    if (add_char) {
        if (!g_standalone) {
            std::printf("--add is for standalone mode; corp mode has no character logins\n");
        } else {
            std::string err;
            if (!standalone::login(g_client_id, err)) {
                std::fprintf(stderr, "EVE login failed: %s\n[press Enter to close]", err.c_str());
                std::string pause;
                std::getline(std::cin, pause);
                return 1;
            }
        }
    }
    // --dump: one fetch cycle to stdout, no TUI (debugging / scripting)
    if (argc > 1 && std::string(argv[1]) == "--dump") {
        if (g_standalone) {
            auto snaps = standalone::fetch_snapshots(g_client_id);
            for (auto& s : snaps) {
                json d;
                try { d = json::parse(s.data); } catch (...) {}
                std::string line = (s.label.empty() ? std::string("(no corp)") : s.label) + ": " +
                    std::to_string(d.value("structures", json::array()).size()) + " structures" +
                    "  F2: " + d.value("fuel2_status", "?") +
                    "  moon-pull: " + (d.value("extractions_ok", false) ? "ok" : "off");
                if (d.value("rentals_online", false)) {
                    int rented = 0, corpm = 0;
                    for (auto& st : d.value("structures", json::array())) {
                        std::string ty = st.value("rental", "");
                        if (ty == "private") rented++;
                        else if (ty == "corp") corpm++;
                    }
                    line += "  rentals: " + std::to_string(rented) + " rented / " +
                            std::to_string(corpm) + " corp";
                }
                {
                    double gm3 = 0, gisk = 0;
                    int drills = 0;
                    for (auto& st : d.value("structures", json::array()))
                        if (st.contains("goo_capacity")) {
                            drills++;
                            gm3 += st.value("goo_m3", 0.0);
                            gisk += st.value("goo_isk", 0.0);
                        }
                    if (drills)
                        line += "  goo: " + std::to_string(drills) + " drills, " +
                                std::to_string((long long)gm3) + " m3, " + isk_compact(gisk);
                }
                if (d.contains("error")) line += "  error: " + d.value("error", "");
                std::printf("%s\n", line.c_str());
            }
        } else {
            json d = json::object();
            try { d = json::parse(standalone::http_get_body(g_fetch_url)); }
            catch (...) { d = json::object(); }
            if (!d.is_object()) d = json::object();
            std::printf("corp endpoint: %d structures\n",
                        (int)d.value("structures", json::array()).size());
        }
        return 0;
    }
    const auto app_start = std::chrono::steady_clock::now();
    std::thread th(worker);
    auto screen = ScreenInteractive::Fullscreen();
    // FTXUI's Cursor::shape is uninitialized by default; force Hidden or the
    // Windows console cursor flickers at the frame edge on every redraw
    screen.SetCursor(ftxui::Screen::Cursor{0, 0, ftxui::Screen::Cursor::Hidden});

    // view state (main thread)
    int sort_mode = 0;          // 0 fuel, 1 type, 2 system, 3 name, 4 need
    const char* SORTN[] = {"fuel", "type", "system", "name", "need"};
    int type_idx = 0;           // 0 = All
    std::map<int, std::string> tab_filter_mem;  // corp tab -> chosen type ("" = All)
    bool tab_filter_inited = false;             // config default applied once at boot
    std::string text_filter;
    bool filter_mode = false;
    bool log_mode = false;      // [f]: refuel-event log instead of structures
    bool detail_mode = false;   // [->]: single-structure detail page
    long long detail_sid = 0;
    int offset = 0, sel = 0;

    // background force-refresh
    auto force_refresh = [&]() {
        if (g_busy) return;
        g_busy = true;
        { std::lock_guard<std::mutex> l(g_mtx); g_status = "kicking a live ESI pull on the box..."; }
        spawn_bg([&]() {
            if (g_standalone)
                standalone_cycle();
            else
                ingest(standalone::http_get_body(g_refresh_url));
            g_busy = false;
            if (g_run) if (g_gui_wake) g_gui_wake();
            else screen.PostEvent(Event::Custom);
        });
    };

    // [alt+c] standalone: add another character via the browser SSO flow.
    // Runs detached; progress lands in the status line, the tab bar picks the
    // new corp up on the refresh that follows. g_busy doubles as the guard so
    // only one login (or refresh) runs at a time.
    auto add_character = [&]() {
        if (!g_standalone) return;
        if (g_busy) {
            std::lock_guard<std::mutex> l(g_mtx);
            g_note = "busy with a refresh or login - try again in a moment";
            g_note_at = time(nullptr);
            return;
        }
        g_busy = true;
        spawn_bg([&]() {
            std::string err, name;
            bool ok = standalone::login(g_client_id, err, &name, [&](const std::string& s) {
                std::lock_guard<std::mutex> l(g_mtx);
                g_status = s;
                if (g_gui_wake) g_gui_wake();
            else screen.PostEvent(Event::Custom);
            });
            {
                std::lock_guard<std::mutex> l(g_mtx);
                g_note = ok ? "added " + name : "login failed: " + err;
                g_note_at = time(nullptr);
                g_status = "";
            }
            if (ok) standalone_cycle();  // new corp becomes a tab right away
            g_busy = false;
            if (g_run) if (g_gui_wake) g_gui_wake();
            else screen.PostEvent(Event::Custom);
        });
    };

    // [1] claim: "I fueled this". With seen_at it stamps that exact logged
    // event; without, the server stamps the newest recent refuel on the
    // structure or parks a pending claim until ESI shows the jump. The block
    // type comes from the station's sticky setting, not the claim.
    auto send_claim = [&](long long sid, std::string seen_at) {
        if (g_busy || sid == 0) return;
        if (g_standalone) {
            std::lock_guard<std::mutex> l(g_mtx);
            g_note = "refuel claims live on the corp endpoint (standalone mode is read-only)";
            g_note_at = time(nullptr);
            return;
        }
        g_busy = true;
        const char* env = std::getenv("STOKER_NAME");
        std::string who = (env && *env) ? env : "";  // backend fills its default
        { std::lock_guard<std::mutex> l(g_mtx); g_status = "filing claim..."; }
        spawn_bg([&, sid, seen_at, who]() {
            std::string url = g_claim_url + "&structure_id=" + std::to_string(sid) +
                              "&by=" + urlenc(who);
            if (!seen_at.empty()) url += "&seen_at=" + urlenc(seen_at);
            std::string res = standalone::http_post_form(url, "");
            std::string note = "claim failed: no reply from the box";
            try {
                json j = json::parse(res);
                if (j.value("ok", false)) {
                    bool st = j.value("status", "") == "stamped";
                    note = std::string(st ? "stamped: " : "claim pending: ") +
                           j.value("structure", "?") + "  fueled by " + who +
                           (st ? "" : "  (lands when ESI shows the jump, up to ~2h)");
                } else {
                    note = "claim rejected: " + j.value("error", std::string("?"));
                }
            } catch (...) {}
            {
                std::lock_guard<std::mutex> l(g_mtx);
                g_note = note;
                g_note_at = time(nullptr);
                g_status.clear();
            }
            ingest(standalone::http_get_body(g_fetch_url));
            g_busy = false;
            if (g_gui_wake) g_gui_wake();
            else screen.PostEvent(Event::Custom);
        });
    };


    auto view = [&]() -> std::vector<Row> {
        std::vector<Row> rows;
        std::vector<std::string> types;
        {
            std::lock_guard<std::mutex> l(g_mtx);
            rows = g_rows;
        }
        // distinct types for the tab filter
        for (auto& r : rows)
            if (std::find(types.begin(), types.end(), r.type) == types.end()) types.push_back(r.type);
        std::sort(types.begin(), types.end());
        std::string want = (type_idx > 0 && type_idx <= (int)types.size()) ? types[type_idx - 1] : "";
        std::vector<Row> f;
        for (auto& r : rows) {
            if (!want.empty() && r.type != want) continue;
            if (!text_filter.empty()) {
                std::string hay = r.name + " " + r.system;
                std::string nd, td = text_filter;
                for (char c : hay) nd += (char)std::tolower(c);
                for (auto& c : td) c = (char)std::tolower(c);
                if (nd.find(td) == std::string::npos) continue;
            }
            f.push_back(r);
        }
        auto cmp = [&](const Row& a, const Row& b) {
            switch (sort_mode) {
                case 1: return a.type < b.type;
                case 2: return a.system < b.system;
                case 3: return a.name < b.name;
                case 4: return a.need > b.need;  // biggest haul first (unknowns last)
                default: {
                    // soonest usage-adjusted runout first
                    double da = a.has_fuel ? effective_days(a) : 1e9;
                    double db = b.has_fuel ? effective_days(b) : 1e9;
                    return da < db;
                }
            }
        };
        std::stable_sort(f.begin(), f.end(), cmp);
        return f;
    };

    // distinct type list (for label + cycling bound), recomputed each frame
    auto type_list = [&]() {
        std::vector<std::string> types;
        std::lock_guard<std::mutex> l(g_mtx);
        for (auto& r : g_rows)
            if (std::find(types.begin(), types.end(), r.type) == types.end()) types.push_back(r.type);
        std::sort(types.begin(), types.end());
        return types;
    };

    // per-tab type filters: each corp tab remembers its own selection, and a
    // tab's FIRST visit starts on its configured default (config
    // "tab_type_filter", e.g. SOUSN opening on Metenox Moon Drill)
    auto set_type_filter = [&](const std::string& name) {
        type_idx = 0;
        if (name.empty()) return;
        auto types = type_list();
        for (size_t i = 0; i < types.size(); i++)
            if (types[i] == name) { type_idx = (int)i + 1; break; }
    };
    auto tab_default_filter = [&](int tab) -> std::string {
        std::lock_guard<std::mutex> l(g_mtx);
        if (tab >= 0 && tab < (int)g_tab_labels.size()) {
            auto it = g_tab_default_filter.find(g_tab_labels[tab]);
            if (it != g_tab_default_filter.end()) return it->second;
        }
        return "";
    };

    auto renderer = Renderer([&] {
        auto rows = view();
        std::vector<Refuel> refuels;
        std::vector<std::string> tabs;
        int total, under14, under7, tabsel;
        std::string pulled, status, esiMod, esiExp, note, upd, corpname;
        {
            std::lock_guard<std::mutex> l(g_mtx);
            total = (int)g_rows.size();
            pulled = g_pulled_at;
            status = g_status;
            refuels = g_refuels;
            esiMod = g_esi_lastmod;
            esiExp = g_esi_expires;
            tabs = g_tab_labels;
            tabsel = g_tab;
            upd = g_update_tag;
            corpname = g_corp_name;
            if (g_note_at && time(nullptr) - g_note_at < 20) note = g_note;
        }
        const bool have_tabs = tabs.size() > 1;
        // first data landed: apply the boot tab's configured default filter
        if (!tab_filter_inited && total > 0) {
            tab_filter_inited = true;
            set_type_filter(tab_default_filter(tabsel));
        }
        // splash disabled for now (splash_scene kept in the source); plain
        // status line until the first snapshot lands
        (void)app_start;
        if (total == 0)
            return vbox({filler(),
                         hbox({filler(),
                               text("STOKER") | bold | color(NEON_PINK),
                               text("  " + (status.empty() ? "loading..." : status)) |
                                   color(status.empty() || status == "connecting to the box..."
                                             ? INK_GRAY : Color::RGB(255, 70, 70)),
                               filler()}),
                         filler()}) |
                   borderRounded;
        // CCP republishes this dataset hourly; show its age + when the next drop is
        // due so a just-done refuel not appearing yet reads as "waiting on CCP".
        std::string fresh;
        if (!esiMod.empty()) {
            fresh = "game data " + rel_age(esiMod);
            if (!esiExp.empty()) {
                long left = (long)(parse_iso(esiExp) - time(nullptr));
                fresh += left > 0 ? "  next +" + std::to_string((left + 59) / 60) + "m"
                                  : "  next any moment";
            }
        } else {
            fresh = pulled.empty() ? "--" : rel_age(pulled);
        }
        under14 = under7 = 0;
        for (auto& r : rows) { if (r.has_fuel && r.days < 14) under14++; if (r.has_fuel && r.days < 7) under7++; }

        // ---- structure detail page ([->] on a selection, [<-]/esc back) ----
        if (detail_mode) {
            Row d;
            bool have = false;
            {
                std::lock_guard<std::mutex> l(g_mtx);
                for (auto& r : g_rows)
                    if (r.sid == detail_sid) { d = r; have = true; break; }
            }
            auto kh = [](const char* k, const char* label) {
                return hbox({text(k) | color(NEON_PINK), text(std::string(" ") + label + "  ") | color(INK_GRAY)});
            };
            auto line = [&](const char* label, Element v) {
                return hbox({text("  " + pad(label, 13)) | color(NEON_DIM_CYAN), v});
            };
            Element dhead = hbox({
                text(" ▌") | color(NEON_PINK), text("STOKER") | bold | color(NEON_PINK),
                text(" » structure detail ") | color(NEON_DIM_CYAN), filler(),
                text(" " + fresh + " ") | color(INK_GRAY),
            });
            std::vector<Element> b;
            if (!have) {
                b.push_back(text("  structure not in the current feed") | color(Color::RGB(128, 136, 150)));
            } else {
                char t[160];
                b.push_back(text(""));
                b.push_back(hbox({text("  "), text(d.name) | bold | color(NEON_PINK)}));
                b.push_back(hbox({text("  "), text(d.system) | color(NEON_CYAN),
                                  text("  " + d.type + "  ") | color(Color::RGB(198, 206, 222)),
                                  text(d.state) | color(INK_GRAY)}));
                b.push_back(hbox({text("  "), paragraph("services: " + (d.services.empty() ? "none" : d.services)) | dim}));
                b.push_back(text(""));
                if (d.has_fuel) {
                    std::snprintf(t, sizeof t, "%.1f days", d.days);
                    std::string ex = d.fuel_expires.empty() ? "" : "  runs out " + d.fuel_expires.substr(0, 16) + " UTC";
                    b.push_back(line("FUEL", hbox({text(t) | color(days_color(d.days)) | bold,
                                                   text("  "),
                                                   bar_with_text("", std::max(0.0, d.days / GAUGE_DAYS),
                                                                 days_color(d.days), 6),
                                                   text(ex) | dim})));
                } else {
                    b.push_back(line("FUEL", text("no fuel data") | color(Color::RGB(128, 136, 150))));
                }
                if (d.burn7 >= 0 || d.burn30 >= 0) {
                    std::string bt;
                    if (d.burn7 >= 0) { std::snprintf(t, sizeof t, "x%.2f (7d)", d.burn7); bt += t; }
                    if (d.burn30 >= 0) { std::snprintf(t, sizeof t, "%sx%.2f (30d)", bt.empty() ? "" : "   ", d.burn30); bt += t; }
                    if (d.est >= 0) { std::snprintf(t, sizeof t, "   est %.1fd at observed burn", d.est); bt += t; }
                    b.push_back(line("BURN", text(bt) | color(Color::RGB(198, 206, 222))));
                } else {
                    b.push_back(line("BURN", text("not enough history yet (~1 day needed)") | color(Color::RGB(128, 136, 150))));
                }
                if (d.bpd >= 0)
                    b.push_back(line("RATE", text(commas(d.bpd) + " blocks/day from online services") | color(Color::RGB(198, 206, 222))));
                if (d.blocks_now >= 0)
                    b.push_back(line("IN BAY", text("~" + commas(d.blocks_now) + " blocks (" + commas(d.m3_now) + " m3), est from fuel clock x rate") | color(Color::RGB(90, 225, 130))));
                if (!d.fuel2_name.empty()) {
                    const char* f2label = d.fuel2_name == "Magmatic Gas" ? "MAGMATIC GAS" : "OZONE";
                    std::string f2s;
                    { std::lock_guard<std::mutex> lf(g_mtx); f2s = g_fuel2_status; }
                    std::string why = f2s == "relogin"
                        ? "unknown - this login predates the corp-assets permission: alt+c and log in again"
                        : f2s == "director" ? "unknown - your character needs the in-game Director role"
                        : f2s == "error" ? "unknown - the corp-assets pull failed, retrying next poll"
                        : "unknown (needs a Director-role data source)";
                    b.push_back(line(f2label, d.fuel2 < 0
                        ? text(why) | color(Color::RGB(128, 136, 150))
                        : text(commas(d.fuel2) + " units" +
                               (fuel2_days(d) >= 0 ? "  (~" + commas(fuel2_days(d)) + "d at drill rate)" : ""))
                              | color(fuel2_color(d))));
                }
                if (d.need >= 0)
                    b.push_back(line("FUEL BLOCKS", d.need == 0
                        ? text("topped past 30 days") | color(Color::RGB(90, 225, 130))
                        : text("haul " + commas(d.need) + " blocks (" + commas(d.m3) + " m3)") | color(need_color(d))));
                if (d.lo_min >= 0)
                    b.push_back(line("DOCTRINE", text("ozone min " + commas(d.lo_min) + " / fill to " + commas(d.lo_target)) | color(Color::RGB(198, 206, 222))));
                if (d.gas_day >= 0)
                    b.push_back(line("GAS", text(commas(d.gas_day) + " magmatic/day, month haul " + commas(d.gas_month) + " units (" + commas(d.gas_m3) + " m3)") | color(Color::RGB(198, 206, 222))));
                if (d.has_refuel) {
                    std::snprintf(t, sizeof t, "%s (+%.1fd)", rel_age(d.last_refuel).c_str(), d.refuel_added);
                    b.push_back(line("LAST FUELED", text(t) | color(Color::RGB(90, 225, 130))));
                }
                b.push_back(text(""));
                b.push_back(hbox({text("  "), text("REFUEL LOG") | color(NEON_DIM_CYAN) | bold,
                                  text("  (this structure, newest first)") | color(INK_GRAY)}));
                if (d.log.empty()) {
                    b.push_back(text("    (no refuels seen yet)") | color(Color::RGB(128, 136, 150)));
                } else {
                    b.push_back(hbox({text("    "), text(pad("WHEN", 10)), text(pad("+DAYS", 7, true)), text("  "),
                                      text(pad("BLOCKS", 10, true)), text("  "),
                                      text(pad("BY", 14))}) | color(NEON_DIM_CYAN));
                    for (auto& ev : d.log) {
                        std::snprintf(t, sizeof t, "+%.1fd", ev.days_added);
                        std::string blk = d.bpd > 0 ? commas(ev.days_added * d.bpd) : "--";
                        b.push_back(hbox({
                            text("    "), text(pad(rel_age(ev.seen_at), 10)) | color(Color::RGB(198, 206, 222)),
                            text(pad(t, 7, true)) | color(Color::RGB(90, 225, 130)) | bold, text("  "),
                            text(pad(blk, 10, true)) | color(Color::RGB(90, 225, 130)), text("  "),
                            text(pad(ev.by.empty() ? "?" : ev.by, 14))
                                | color(ev.by.empty() ? Color::RGB(128, 136, 150) : NEON_PINK),
                        }));
                    }
                }
            }
            Element dkeys = hbox({
                text(" "), kh("←/esc", "back"), kh("1", "I fueled it"),
                kh("r", "refresh"), kh("q", "quit"), filler(),
                (!note.empty() ? text(" " + note + " ") | color(Color::RGB(90, 225, 130))
                 : !status.empty() ? text(" " + status + " ") | color(Color::RGB(250, 215, 70))
                 : g_busy ? text(" refreshing ") | color(Color::RGB(250, 215, 70)) | blink
                          : text("")),
            });
            return vbox({dhead, separator(), vbox(b) | flex, separator(), dkeys}) | borderRounded;
        }

        auto types = type_list();
        std::string tsel = (type_idx > 0 && type_idx <= (int)types.size()) ? types[type_idx - 1] : "All";

        // clamp selection + scroll to the visible window
        int H = Terminal::Size().dimy;
        int W = Terminal::Size().dimx;
        // non-body rows: head, filt, 2 separators, colhead, detail, keys + border
        int visible = H - 9 - (have_tabs ? 1 : 0);
        if (visible < 3) visible = 3;
        // name column soaks up whatever width is left past the fixed columns
        int name_w = (log_mode ? W - 60 : W - 58);
        if (name_w < 12) name_w = 12;
        if (name_w > 60) name_w = 60;
        bool wide = W >= 105;
        int n = log_mode ? (int)refuels.size() : (int)rows.size();
        if (sel >= n) sel = n - 1;
        if (sel < 0) sel = 0;
        if (sel < offset) offset = sel;
        if (sel >= offset + visible) offset = sel - visible + 1;
        if (offset < 0) offset = 0;
        if (offset > std::max(0, n - visible)) offset = std::max(0, n - visible);

        // header
        Element head = hbox({
            text(" ▌") | color(NEON_PINK),
            text("STOKER") | bold | color(NEON_PINK),
            text(std::string(" ") + STOKER_VERSION + " ") | color(INK_GRAY),
            text(log_mode ? "» refuel log "
                          : "» " + (corpname.empty() ? "" : corpname + " ") + "fuel watch ")
                | color(NEON_DIM_CYAN),
            text(g_standalone ? "[standalone] " : "[corp] ") | color(INK_GRAY),
            (upd.empty() ? text("")
                         : text(" " + upd + " available - press u ") | color(Color::RGB(250, 215, 70))),
            text(std::to_string(n) + "/" + std::to_string(total)) | color(INK_GRAY),
            filler(),
            text("under14d ") | color(INK_GRAY),
            text(std::to_string(under14)) | bold | color(under14 ? Color::RGB(250, 215, 70) : Color::RGB(90, 225, 130)),
            text("  under7d ") | color(INK_GRAY),
            text(std::to_string(under7)) | bold | color(under7 ? Color::RGB(255, 70, 70) : Color::RGB(90, 225, 130)),
            text(wide ? "   " + fresh + " " : " ") | color(INK_GRAY),
        });

        Element filt = hbox({
            text(" type ") | color(INK_GRAY), text(tsel) | color(NEON_CYAN),
            text("  sort ") | color(INK_GRAY), text(SORTN[sort_mode]) | color(NEON_CYAN),
            text("  filter ") | color(INK_GRAY),
            (filter_mode ? text(text_filter + "_") | color(Color::RGB(250, 215, 70))
                         : text(text_filter.empty() ? "none" : text_filter) | color(text_filter.empty() ? Color::RGB(128, 136, 150) : Color::RGB(90, 225, 130))),
            filler(),
            (!note.empty() ? text(" " + note + " ") | color(Color::RGB(90, 225, 130))
                 : !status.empty() ? text(" " + status + " ") | color(Color::RGB(250, 215, 70))
                 : g_busy ? text(" refreshing ") | color(Color::RGB(250, 215, 70)) | blink
                          : text("")),
        });

        // column header + body + detail (structures or refuel log)
        Element colhead, detail = text("");
        std::vector<Element> body;
        if (log_mode) {
            // BLOCKS = fuel-log estimate of the deposit (days added x the
            // structure's burn rate); BY comes from the [1] claim.
            colhead = hbox({
                text(pad(" WHEN", 10)), text(pad("+DAYS", 7, true)), text("  "),
                text(pad("BLOCKS", 12, true)), text("  "),
                text(pad("BY", 14)),
                text(pad("SYSTEM", 9)), text("NAME"),
            }) | color(NEON_DIM_CYAN);
            for (int i = offset; i < n && i < offset + visible; i++) {
                const Refuel& v = refuels[i];
                char db[16];
                std::snprintf(db, sizeof db, "+%.1fd", v.days_added);
                std::string blk = v.pending ? "awaiting ESI"
                                : (v.blocks >= 0 ? commas(v.blocks) : "--");
                Color blkc = v.pending ? Color::RGB(250, 215, 70)
                           : (v.blocks >= 0 ? Color::RGB(90, 225, 130) : Color::RGB(128, 136, 150));
                Element line = hbox({
                    text(pad(" " + rel_age(v.seen_at), 10)) | color(Color::RGB(198, 206, 222)),
                    (v.pending ? text(pad("...", 7, true)) | color(Color::RGB(250, 215, 70))
                               : text(pad(db, 7, true)) | color(Color::RGB(90, 225, 130)) | bold),
                    text("  "),
                    text(pad(blk, 12, true)) | color(blkc), text("  "),
                    text(pad(v.by.empty() ? "?" : v.by, 14)) | color(v.by.empty() ? Color::RGB(128, 136, 150) : NEON_PINK),
                    text(pad(v.system, 9)) | color(NEON_CYAN),
                    text(pad(v.name, name_w)),
                });
                if (i == sel) line = line | inverted;
                body.push_back(line);
            }
            if (n == 0)
                body.push_back(text(g_standalone && standalone::g_history_api.empty()
                    ? "  (refuel history is off - set \"history_api\" in the config; ESI"
                      " alone has no deposit events)"
                    : "  (no refuels seen yet - the log fills in as polls catch fuel jumps)")
                    | color(Color::RGB(128, 136, 150)));
            if (sel >= 0 && sel < n) {
                const Refuel& v = refuels[sel];
                if (v.pending) {
                    detail = hbox({text(" > ") | color(NEON_PINK), text(v.name) | bold,
                                   text("  " + v.system + "  ") | color(NEON_CYAN),
                                   text("claim filed - stamps the next fuel jump ESI shows here") | dim});
                } else {
                    std::string ex = v.new_expires.empty() ? "?" : v.new_expires.substr(0, 16) + " UTC";
                    std::string who = v.by.empty() ? "unclaimed - press 1 = I fueled it"
                                                   : "fueled by " + v.by;
                    detail = hbox({text(" > ") | color(NEON_PINK), text(v.name) | bold,
                                   text("  " + v.system + "  ") | color(NEON_CYAN),
                                   text("fuel now runs to " + ex + "  ") | dim,
                                   text(who) | color(v.by.empty() ? Color::RGB(250, 215, 70) : Color::RGB(90, 225, 130))});
                }
            }
        } else {
            // DAYS = ESI days left; UNITS = fuel blocks to top up to 30d;
            // 2ND FUEL = magmatic gas / liquid ozone stock ("?" without a
            // Director-role data source)
            colhead = hbox({
                text(" "), text("■") | color(NEON_CYAN), text(pad(" Blocks", 9)),
                text("30d") | color(INK_GRAY), text(" "),
                text("●") | color(Color::RGB(235, 90, 60)),
                text("◆") | color(Color::RGB(120, 190, 255)), text(pad(" Gas/Oz", 8)),
                text("30d") | color(INK_GRAY), text("  "),
                text(pad("TYPE", 12)), text("  "),
                text(pad("SYSTEM", 9)), text("NAME"),
            }) | color(NEON_DIM_CYAN);
            for (int i = offset; i < n && i < offset + visible; i++) {
                const Row& r = rows[i];
                Element rest = hbox({
                    text(pad(r.type, 12)) | color(Color::RGB(198, 206, 222)), text("  "),
                    text(pad(r.system, 9)) | color(NEON_CYAN),
                    text(pad(r.name, name_w)),
                });
                if (i == sel) rest = rest | inverted;
                Element line = hbox({
                    fuel_cell(r, 14), text(i == sel ? ">" : " ") | color(NEON_PINK) | bold,
                    fuel2_cell(r, 14), text("  "),
                    rest,
                });
                body.push_back(line);
            }
            if (n == 0) body.push_back(text("  (no structures match)") | color(Color::RGB(128, 136, 150)));
            if (sel >= 0 && sel < n) {
                const Row& r = rows[sel];
                std::string svc = r.services.empty() ? "none" : r.services;
                std::string ex = r.fuel_expires.empty() ? "no fuel data" : ("fuel out " + r.fuel_expires.substr(0, 16).append(" UTC"));
                std::string fueled;
                if (r.has_refuel) {
                    char fb[48];
                    std::snprintf(fb, sizeof fb, "  fueled %s (+%.1fd)", rel_age(r.last_refuel).c_str(), r.refuel_added);
                    fueled = fb;
                }
                if (r.burn7 >= 0 || r.burn30 >= 0) {
                    char bb[96];
                    if (r.burn7 >= 0 && r.burn30 >= 0)
                        std::snprintf(bb, sizeof bb, "  burn x%.2f 7d / x%.2f 30d", r.burn7, r.burn30);
                    else
                        std::snprintf(bb, sizeof bb, "  burn x%.2f", burn_of(r));
                    fueled += bb;
                    if (r.est >= 0) {
                        std::snprintf(bb, sizeof bb, "  est %.1fd at that rate", r.est);
                        fueled += bb;
                    }
                }
                detail = hbox({text(" > ") | color(NEON_PINK),
                               text(r.name) | bold, text("  " + r.system + "  ") | color(NEON_CYAN),
                               text(r.state + "  ") | color(Color::RGB(128, 136, 150)),
                               text(ex + "  services: " + svc) | dim,
                               text(fueled) | color(Color::RGB(90, 225, 130))});
            }
        }
        auto key_hint = [](const char* k, const std::string& label) {
            return hbox({text(k) | color(NEON_PINK), text(" " + label + "  ") | color(INK_GRAY)});
        };
        // width-responsive footer: full labels when they fit, short ones when
        // not, keys alone when even those don't; the explainer only rides
        // along when there is genuine room left after the hints
        struct Hint { const char* k; const char* full; const char* mini; bool show; };
        const Hint HINTS[] = {
            {"tab", "type", "type", true},
            {"s", "sort", "sort", true},
            {"/", "filter", "filt", true},
            {"f", "refuel log", "log", true},
            {"→", "details", "info", true},
            {"c", "corp", "corp", have_tabs},
            {"alt+c", "add character", "add", g_standalone},
            {"1", "I fueled it", "claim", !g_standalone},
            {"u", "update", "upd", !upd.empty()},
            {"r", "refresh", "rfsh", true},
            {"q", "quit", "quit", true},
        };
        auto hints_width = [&](bool mini, bool labels) {
            size_t w = 1;
            for (auto& h : HINTS) {
                if (!h.show) continue;
                w += std::strlen(h.k) + (labels ? std::strlen(mini ? h.mini : h.full) + 1 : 0) + 2;
            }
            return (int)w;
        };
        const bool full_fit = hints_width(false, true) <= W - 2;
        const bool mini_fit = hints_width(true, true) <= W - 2;
        Elements ke = {text(" ")};
        for (auto& h : HINTS) {
            if (!h.show) continue;
            ke.push_back(full_fit ? key_hint(h.k, h.full)
                         : mini_fit ? key_hint(h.k, h.mini)
                                    : key_hint(h.k, ""));
        }
        ke.push_back(filler());
        const char* expl = log_mode
            ? "BLOCKS = deposit estimated from the fuel clock jump "
            : "gauges: days left | haul to 30d on the right (? = needs a Director token) ";
        if (full_fit && W - hints_width(false, true) > (int)std::strlen(expl) + 2)
            ke.push_back(text(expl) | color(INK_GRAY));
        Element keys = hbox(ke);

        // corp tabs: only when the logins reach more than one corp
        Elements lay = {head};
        if (have_tabs) {
            Elements te = {text(" ")};
            for (size_t i = 0; i < tabs.size(); i++) {
                std::string lbl = " " + (tabs[i].empty() ? std::string("?") : tabs[i]) + " ";
                te.push_back((int)i == tabsel
                                 ? text(lbl) | bold | color(NEON_PINK) | inverted
                                 : text(lbl) | color(INK_GRAY));
                te.push_back(text(" "));
            }
            te.push_back(filler());
            te.push_back(text("c switches corp ") | color(INK_GRAY));
            lay.push_back(hbox(te));
        }
        lay.push_back(filt);
        lay.push_back(separator());
        lay.push_back(colhead);
        lay.push_back(vbox(body) | flex);
        lay.push_back(separator());
        lay.push_back(detail);
        lay.push_back(keys);
        return vbox(lay) | borderRounded;
    });

    auto component = CatchEvent(renderer, [&](Event e) {
        // STOKER_EVLOG=<path>: append every event's raw bytes (debug aid)
        if (const char* evlog = std::getenv("STOKER_EVLOG"); evlog && *evlog) {
            if (FILE* f = std::fopen(evlog, "a")) {
                for (unsigned char ch : e.input())
                    std::fprintf(f, ch >= 32 && ch < 127 ? "%c" : "\\x%02x", ch);
                std::fprintf(f, "\n");
                std::fclose(f);
            }
        }
        // alt+c (ESC-prefixed 'c') adds a character in standalone mode; works
        // from every view, checked first so no mode-branch can swallow it.
        // Always answers with SOMETHING so a keypress is never silently eaten.
        if (e.input() == "\x1b" "c") {
            if (g_standalone) {
                add_character();
                if (g_busy) {
                    std::lock_guard<std::mutex> l(g_mtx);
                    if (g_status.empty()) g_status = "starting EVE login...";
                }
            } else {
                std::lock_guard<std::mutex> l(g_mtx);
                g_note = "character logins are standalone-mode only (this is corp mode)";
                g_note_at = time(nullptr);
            }
            return true;
        }
        // recompute the current view's row count for paging
        int n;
        if (log_mode) {
            std::lock_guard<std::mutex> l(g_mtx);
            n = (int)g_refuels.size();
        } else {
            n = (int)view().size();
        }

        if (filter_mode) {
            if (e == Event::Return || e == Event::Escape) {
                if (e == Event::Escape) text_filter.clear();
                filter_mode = false;
                return true;
            }
            if (e == Event::Backspace) {
                if (!text_filter.empty()) text_filter.pop_back();
                return true;
            }
            if (e.is_character()) { text_filter += e.character(); sel = 0; offset = 0; return true; }
            return false;
        }

        if (detail_mode) {
            if (e == Event::ArrowLeft || e == Event::Escape) { detail_mode = false; return true; }
            if (e == Event::Character("q")) { g_run = false; screen.Exit(); return true; }
            if (e == Event::Character("r")) { force_refresh(); return true; }
            if (e == Event::Character("1")) { send_claim(detail_sid, ""); return true; }
            return true;  // swallow list-view keys while on the detail page
        }

        if (e == Event::Character("q")) { g_run = false; screen.Exit(); return true; }
        if (e == Event::Escape) {
            if (log_mode) { log_mode = false; sel = 0; offset = 0; return true; }
            g_run = false; screen.Exit(); return true;
        }
        if (e == Event::Character("f")) { log_mode = !log_mode; sel = 0; offset = 0; return true; }
        // current selection target (either view) for claims and type-setting
        auto selected_sid = [&](std::string* seen_at) -> long long {
            if (log_mode) {
                std::lock_guard<std::mutex> l(g_mtx);
                if (sel >= 0 && sel < (int)g_refuels.size() && !g_refuels[sel].pending) {
                    if (seen_at) *seen_at = g_refuels[sel].seen_at;
                    return g_refuels[sel].sid;
                }
                return 0;
            }
            auto rows = view();
            return (sel >= 0 && sel < (int)rows.size()) ? rows[sel].sid : 0;
        };
        if (e == Event::ArrowRight) {
            // open the detail page for the selected structure (or the log
            // entry's structure); [<-]/esc returns to this view
            long long sid = selected_sid(nullptr);
            if (sid) { detail_mode = true; detail_sid = sid; }
            return true;
        }
        if (e == Event::Character("1")) {
            // "I fueled this" - the highlighted log event, or the structure
            // you just fueled from the main view (server matches or parks it)
            std::string seen_at;
            long long sid = selected_sid(&seen_at);
            if (sid) send_claim(sid, seen_at);
            return true;
        }
        if (e == Event::Character("/")) { filter_mode = true; return true; }
        if (e == Event::Character("u")) {  // apply a pending self-update
            std::string tag, url;
            {
                std::lock_guard<std::mutex> l(g_mtx);
                tag = g_update_tag;
                url = g_update_url;
            }
            if (!tag.empty() && !g_busy) {
                g_busy = true;
                {
                    std::lock_guard<std::mutex> l(g_mtx);
                    g_status = "downloading " + tag + "...";
                }
                spawn_bg([&, tag, url]() {
                    std::string res = apply_update(tag, url);
                    {
                        std::lock_guard<std::mutex> l(g_mtx);
                        g_note = res;
                        g_note_at = time(nullptr);
                        g_status = "";
                        if (res.rfind("updated", 0) == 0) g_update_tag.clear();
                    }
                    g_busy = false;
                    if (g_run) if (g_gui_wake) g_gui_wake();
            else screen.PostEvent(Event::Custom);
                });
            }
            return true;
        }
        if (e == Event::Character("c")) {  // cycle corp tabs (standalone, >1 corp)
            std::string data;
            int oldtab = 0, newtab = 0;
            {
                std::lock_guard<std::mutex> l(g_mtx);
                oldtab = g_tab;
                if (g_tab_data.size() > 1) {
                    g_tab = (g_tab + 1) % (int)g_tab_data.size();
                    newtab = g_tab;
                    data = g_tab_data[g_tab];
                }
            }
            if (!data.empty()) {
                // park the old tab's filter by NAME (indices shift between corps)
                auto types = type_list();
                tab_filter_mem[oldtab] =
                    (type_idx > 0 && type_idx <= (int)types.size()) ? types[type_idx - 1] : "";
                ingest(data);
                auto it = tab_filter_mem.find(newtab);
                set_type_filter(it != tab_filter_mem.end() ? it->second
                                                           : tab_default_filter(newtab));
                sel = 0;
                offset = 0;
            }
            return true;
        }
        if (e == Event::Character("s")) { sort_mode = (sort_mode + 1) % 5; sel = 0; offset = 0; return true; }
        if (e == Event::Character("r")) { force_refresh(); return true; }
        if (e == Event::Tab || e == Event::Character("\t")) {
            int tc = (int)type_list().size();
            type_idx = (type_idx + 1) % (tc + 1);
            sel = 0; offset = 0;
            return true;
        }
        if (e == Event::ArrowDown || e == Event::Character("j")) { if (sel < n - 1) sel++; return true; }
        if (e == Event::ArrowUp || e == Event::Character("k")) { if (sel > 0) sel--; return true; }
        if (e == Event::PageDown) { sel = std::min(n - 1, sel + 15); return true; }
        if (e == Event::PageUp) { sel = std::max(0, sel - 15); return true; }
        if (e == Event::Home) { sel = 0; return true; }
        if (e == Event::End) { sel = n - 1; return true; }
        if (e.is_mouse()) {
            if (e.mouse().button == Mouse::WheelDown) { if (sel < n - 1) sel++; return true; }
            if (e.mouse().button == Mouse::WheelUp) { if (sel > 0) sel--; return true; }
        }
        return false;
    });

    // periodic redraw: 1s ticks for the "Xm ago" clock and background fetches
    // (splash disabled; no fast path needed)
    std::thread ticker([&]() {
        int slow = 0;
        while (g_run) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (++slow >= 10) {
                slow = 0;
                if (g_gui_wake) g_gui_wake();
            else screen.PostEvent(Event::Custom);
            }
        }
    });

#ifdef STOKER_GUI
    // no stored login yet: run the browser SSO now, progress in the window
    if (g_standalone && !standalone::have_login()) add_character();
    {
        GLFWwindow* gw = nullptr;
        gui::run(component, g_run, &gw);
    }
#else
    screen.Loop(component);
#endif
    g_run = false;
    if (th.joinable()) th.join();
    {
        std::lock_guard<std::mutex> l(g_bg_mtx);
        for (auto& t : g_bg_threads)
            if (t.joinable()) t.join();
    }
    if (ticker.joinable()) ticker.join();
    return 0;
}
#endif  // STOKER_IMGUI
