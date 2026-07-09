// SMT-style local awareness: tail the EVE client's own chat logs for pilot
// position (Local channel-change lines) and hostile intel (system names
// spotted in intel channels). Pure local file reads, no ESI. Everything is
// best-effort: no logs on this machine = the feature is quietly off.
//
// Included by stoker-imgui.cpp AFTER the universe map globals (needs
// load_universe()/g_sysid/g_sysname) and after bpos-dash.cpp (needs the
// g_eve_logs_cfg/g_intel_channels_cfg config globals). Everything here runs
// on the render thread only (throttled to one directory sweep per 2s).
#pragma once

#include <deque>
#include <filesystem>
#include <fstream>

struct PilotLoc {
    std::string name, system;
    time_t at = 0;
};
struct IntelHit {
    std::string system, channel, pilot, text;
    time_t at = 0;
};

struct LogTail {
    std::streamoff off = 0;
    std::string channel, listener, cur_sys;
    time_t mtime = 0;
};

static std::vector<PilotLoc> g_pilots;
static std::deque<IntelHit> g_intel;  // newest first, pruned to 30 min
static std::string g_logs_dir;        // resolved Chatlogs dir; "" = not found
static std::map<std::string, LogTail> g_tails;

// EVE chat logs are UTF-16LE with BOM
static std::string utf16le_to_utf8(const std::string& b) {
    std::string out;
    out.reserve(b.size() / 2);
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        unsigned int c = (unsigned char)b[i] | ((unsigned int)(unsigned char)b[i + 1] << 8);
        if (c == 0xFEFF) continue;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < b.size()) {
            unsigned int lo =
                (unsigned char)b[i + 2] | ((unsigned int)(unsigned char)b[i + 3] << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 63));
            out += (char)(0x80 | (c & 63));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 63));
            out += (char)(0x80 | ((c >> 6) & 63));
            out += (char)(0x80 | (c & 63));
        }
    }
    return out;
}

static void logs_find_dir() {
    namespace fs = std::filesystem;
    std::vector<std::string> cands;
    if (!g_eve_logs_cfg.empty()) cands.push_back(g_eve_logs_cfg);
#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE")) {
        cands.push_back(std::string(up) + "\\Documents\\EVE\\logs");
        cands.push_back(std::string(up) + "\\OneDrive\\Documents\\EVE\\logs");
    }
#else
    if (const char* h = std::getenv("HOME")) {
        std::string H = h;
        cands.push_back(H + "/Documents/EVE/logs");
        for (const char* base :
             {"/.steam/steam/steamapps/compatdata", "/.local/share/Steam/steamapps/compatdata"}) {
            std::error_code ec;
            for (fs::directory_iterator it(H + base, ec), end; !ec && it != end;
                 it.increment(ec)) {
                cands.push_back(it->path().string() +
                                "/pfx/drive_c/users/steamuser/My Documents/EVE/logs");
                cands.push_back(it->path().string() +
                                "/pfx/drive_c/users/steamuser/Documents/EVE/logs");
            }
        }
    }
#endif
    for (auto& c : cands) {
        std::error_code ec;
        if (fs::path(c).filename() == "Chatlogs" && fs::exists(c, ec)) {
            g_logs_dir = c;
            return;
        }
        if (fs::exists(fs::path(c) / "Chatlogs", ec)) {
            g_logs_dir = (fs::path(c) / "Chatlogs").string();
            return;
        }
    }
}

// new bytes since the last look; first touch of a big file starts 64k from
// the end (enough for the header block on normal-sized logs)
static std::string logs_read_new(const std::filesystem::path& p, LogTail& t) {
    std::error_code ec;
    auto size = (std::streamoff)std::filesystem::file_size(p, ec);
    if (ec || size <= t.off) return "";
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    if (t.off == 0 && size > 65536) t.off = (size - 65536) & ~(std::streamoff)1;
    f.seekg(t.off);
    std::string buf((size_t)(size - t.off), 0);
    f.read(&buf[0], (std::streamsize)buf.size());
    buf.resize((size_t)f.gcount());
    t.off += (std::streamoff)buf.size();
    return utf16le_to_utf8(buf);
}

static std::string lower_(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// strip chat punctuation so "4-P4FE," and "*4-P4FE*" still match
static std::string clean_token(const std::string& s) {
    size_t a = 0, b = s.size();
    auto junk = [](char c) {
        return c == ',' || c == '*' || c == '(' || c == ')' || c == '<' || c == '>' ||
               c == '"' || c == '\'' || c == '?' || c == '!' || c == '.' || c == ':' ||
               c == ';' || c == '[' || c == ']';
    };
    while (a < b && junk(s[a])) a++;
    while (b > a && junk(s[b - 1])) b--;
    return s.substr(a, b - a);
}

static bool is_intel_channel(const std::string& channel) {
    std::string lc = lower_(channel);
    if (lc == "local") return false;
    if (!g_intel_channels_cfg.empty()) {
        for (auto& c : g_intel_channels_cfg)
            if (lower_(c) == lc) return true;
        return false;
    }
    // default: intel-style channels plus the Imperium standing-fleet ones
    return lc.find("intel") != std::string::npos || lc.find(".imperium") != std::string::npos;
}

static void eve_logs_scan() {
    static time_t last_scan = 0, last_dirtry = 0;
    time_t nowt = time(nullptr);
    if (nowt - last_scan < 2) return;
    last_scan = nowt;
    if (g_logs_dir.empty()) {
        if (nowt - last_dirtry < 60) return;
        last_dirtry = nowt;
        logs_find_dir();
        if (g_logs_dir.empty()) return;
    }
    load_universe();
    namespace fs = std::filesystem;
    static const std::regex fname_re(R"(^(.*)_(\d{8})_(\d{6})(_\d+)?$)");
    static const std::regex line_re(R"(\[\s*[\d.]+\s+[\d:]+\s*\]\s*(.*?)\s*>\s*(.*))");
    std::error_code ec;
    for (fs::directory_iterator it(g_logs_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        fs::path p = it->path();
        if (p.extension() != ".txt") continue;
        auto ft = fs::last_write_time(p, ec);
        if (ec) continue;
        time_t mt = (time_t)std::chrono::duration_cast<std::chrono::seconds>(
                        ft.time_since_epoch() -
                        fs::file_time_type::clock::now().time_since_epoch())
                        .count() +
                    nowt;
        if (nowt - mt > 12 * 3600) continue;  // stale session
        std::smatch fm;
        std::string stem = p.stem().string();
        if (!std::regex_match(stem, fm, fname_re)) continue;
        std::string channel = fm[1].str();
        bool is_local = lower_(channel) == "local";
        if (!is_local && !is_intel_channel(channel)) continue;

        LogTail& t = g_tails[p.string()];
        t.channel = channel;
        t.mtime = mt;
        std::string text = logs_read_new(p, t);
        if (text.empty()) continue;

        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (t.listener.empty()) {
                size_t lp = line.find("Listener:");
                if (lp != std::string::npos) {
                    t.listener = line.substr(lp + 9);
                    while (!t.listener.empty() && t.listener.front() == ' ')
                        t.listener.erase(t.listener.begin());
                }
            }
            std::smatch m;
            if (!std::regex_search(line, m, line_re)) continue;
            std::string pilot = m[1].str(), msg = m[2].str();
            if (is_local) {
                size_t cp = msg.find("Channel changed to Local : ");
                if (pilot == "EVE System" && cp != std::string::npos)
                    t.cur_sys = msg.substr(cp + 27);
                continue;
            }
            if (pilot == "EVE System") continue;
            // intel line: first token that names a system wins; clr/clear
            // lines retire that system's intel instead
            std::istringstream ws(msg);
            std::string tok, sys;
            bool clr = false;
            while (ws >> tok) {
                std::string ct = lower_(clean_token(tok));
                if (ct == "clr" || ct == "clear" || ct == "status") clr = true;
                if (sys.empty() && g_sysid.count(ct)) sys = g_sysname[g_sysid[ct]];
            }
            if (sys.empty()) continue;
            if (clr) {
                for (auto iit = g_intel.begin(); iit != g_intel.end();)
                    iit = iit->system == sys ? g_intel.erase(iit) : iit + 1;
                continue;
            }
            g_intel.push_front({sys, channel, pilot, msg, nowt});
        }
    }
    while (g_intel.size() > 40 || (!g_intel.empty() && nowt - g_intel.back().at > 1800))
        g_intel.pop_back();
    // pilot list: newest Local session per listener
    std::map<std::string, PilotLoc> best;
    for (auto& kv : g_tails) {
        LogTail& t = kv.second;
        if (lower_(t.channel) != "local" || t.cur_sys.empty()) continue;
        if (nowt - t.mtime > 12 * 3600) continue;
        std::string who = t.listener.empty() ? "pilot" : t.listener;
        auto& b = best[who];
        if (t.mtime >= b.at) b = {who, t.cur_sys, t.mtime};
    }
    g_pilots.clear();
    for (auto& kv : best) g_pilots.push_back(kv.second);
}
