// Standalone mode: EVE SSO (PKCE, no secret) + direct ESI pull, for people
// outside the corp endpoint. Produces the same JSON shape as the /bpos
// endpoint so ingest() works unchanged; server-side extras (burn rates,
// refuel log, claims, bay estimates) simply come back unknown.
//
// Requires an EVE developer application (https://developers.eveonline.com):
//   callback URL  http://localhost:8420/callback
//   scopes        esi-corporations.read_structures.v1
//                 esi-assets.read_corporation_assets.v1
// The in-game character needs the Station_Manager role (or Director).
//
// Include AFTER run_cmd/urlenc/parse_iso in bpos-dash.cpp (single-TU project).
#pragma once

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <winhttp.h>
#include <shellapi.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

namespace standalone {

#ifdef _WIN32
// --- native transport: WinHTTP ------------------------------------------------
// The OS HTTP stack: honors the system proxy automatically, negotiates TLS
// like the rest of Windows, and does not depend on whichever curl.exe is in
// the PATH. Primary on Windows; the curl helpers below stay as fallback.
static std::wstring _w(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w += (wchar_t)c;  // our URLs/headers are ASCII
    return w;
}
static std::string _n(const wchar_t* s) {
    std::string o;
    for (; *s; s++) o += (char)(*s < 128 ? *s : '?');
    return o;
}

// Returns the HTTP status (>0), or 0 on transport failure. Fills `out` with
// the body; headers_out gets the x-pages/last-modified/expires trio when asked.
static int winhttp_req(const std::string& url, const wchar_t* method,
                       const std::string& bearer, const std::string& body,
                       const wchar_t* ctype, std::string& out, json* headers_out) {
    out.clear();
    // shutting down: fail fast so mid-sweep joins don't block the window close
    if (!g_run) return 0;
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {0}, path[2048] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    std::wstring wurl = _w(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return 0;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    HINTERNET ses = WinHttpOpen(L"STOKER", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)  // pre-8.1 fallback
        ses = WinHttpOpen(L"STOKER", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#else
    HINTERNET ses = WinHttpOpen(L"STOKER", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
    if (!ses) return 0;
#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(ses, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof dec);
#endif
    WinHttpSetTimeouts(ses, 8000, 8000, 15000, 15000);  // resolve/connect/send/receive
    int status = 0;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = nullptr;
    if (con)
        req = WinHttpOpenRequest(con, method, path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (req) {
        std::wstring hdrs;
        if (!bearer.empty()) hdrs += L"Authorization: Bearer " + _w(bearer) + L"\r\n";
        if (ctype && *ctype) hdrs += std::wstring(L"Content-Type: ") + ctype + L"\r\n";
        BOOL ok = WinHttpSendRequest(
            req, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
            hdrs.empty() ? 0 : (DWORD)hdrs.size(),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
            (DWORD)body.size(), (DWORD)body.size(), 0);
        if (ok && WinHttpReceiveResponse(req, nullptr)) {
            DWORD st = 0, sz = sizeof st;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz,
                                WINHTTP_NO_HEADER_INDEX);
            status = (int)st;
            if (headers_out) {
                auto qh = [&](const wchar_t* name) -> std::string {
                    wchar_t buf[256];
                    DWORD n = sizeof buf;
                    std::wstring nm = name;
                    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, nm.c_str(), buf, &n,
                                            WINHTTP_NO_HEADER_INDEX))
                        return _n(buf);
                    return "";
                };
                (*headers_out)["x-pages"] = qh(L"x-pages");
                (*headers_out)["last-modified"] = qh(L"last-modified");
                (*headers_out)["expires"] = qh(L"expires");
            }
            bool clean_eof = false;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) break;
                if (!avail) { clean_eof = true; break; }
                std::string chunk(avail, 0);
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
                out.append(chunk.data(), got);
            }
            if (!clean_eof) status = 0;  // truncated body: let the fallbacks run
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return status;
}
#endif  // _WIN32

static const int CALLBACK_PORT = 8420;
static const char* SSO_TOKEN_URL = "https://login.eveonline.com/v2/oauth/token";
static const char* ESI = "https://esi.evetech.net/latest";
// Requested at login: structures (the dashboard) + corp assets (the F²
// fuel-bay column + moongoo; Director-gated in-game, others just 403 to "?")
// + corp mining (Athanor moon-pull timers) + waypoint (set destination)
// + own roles/titles (read-only; some corp backends decide feature access
// server-side from the presented token).
// Overridable via config.json "scopes", e.g. to trim back to structures-only.
static const char* SCOPE =
    "esi-corporations.read_structures.v1 esi-assets.read_corporation_assets.v1 "
    "esi-industry.read_corporation_mining.v1 esi-characters.read_notifications.v1 "
    "esi-structures.read_corporation.v1 esi-ui.write_waypoint.v1 "
    "esi-characters.read_corporation_roles.v1 esi-characters.read_titles.v1 "
    "esi-corporations.read_starbases.v1";

// the new-generation ESI endpoints (skyhooks, sov hubs) only exist behind a
// compatibility date; the legacy /latest tree does not carry them
static const char* COMPAT_HDR = "X-Compatibility-Date: 2026-06-01";
static std::string g_scopes = SCOPE;
// Refuel-history service: standalone clients have no storage, so each poll
// reports its snapshot here and reads back the refuel log the server builds
// by diffing snapshots over time. Keyless, scoped per corp; config
// "history_api" overrides, empty string disables. See README (privacy note).
static std::string g_history_api = "https://api.escalateanyways.com/market/stoker";

// Moon-rental classification feed: which anchored moon structures are rented
// out vs corp-kept. Only queried for the corp the feed describes. config
// "rentals_api" (full endpoint URL) + "rentals_token" + "rentals_corp_id";
// empty URL = off.
static std::string g_rentals_api, g_rentals_token;
static long long g_rentals_corp = 98695839;  // SOUSN
// Metenox monthly economics endpoint (same service + token as rentals);
// derived from rentals_api when the config doesn't name it explicitly
static std::string g_econ_api;

// Skyhook watch feed (watchlist + income + live theft windows + raid stats),
// served by the corp backend. config "skyhooks_api"; empty = off. Shown on
// the same corp tab as the rentals feed.
static std::string g_skyhooks_api;
// Backend-free variant: config "skyhook_watchlist" = an array of
// {planet_id, system, planet_roman, hourly_isk, monthly_rent_isk} objects.
// Theft windows come straight from the public raidable feed (no auth), so
// this needs no server at all; takes precedence over "skyhooks_api".
static json g_skyhook_watchlist = json::array();
// config "skyhook_all": also list every hook game-wide that has a theft
// window announced or open (the raidable feed only carries windowed hooks).
// Defaults on when a watchlist is configured.
static bool g_skyhook_all = false;

// Live sweep progress: fetch stages stamp what they are pulling so the UI
// can show a real loading state instead of a frozen "refreshing..." line.
static std::mutex g_prog_mtx;
static std::string g_progress;
static void progress(const std::string& s) {
    std::lock_guard<std::mutex> l(g_prog_mtx);
    g_progress = s;
}
static std::string progress_now() {
    std::lock_guard<std::mutex> l(g_prog_mtx);
    return g_progress;
}

// Run fn(0..n-1) across a small worker pool. Each http_get is a whole curl
// subprocess, so the per-item lookup loops (type names, moon names, tower
// bays, skyhook planets) otherwise run one-at-a-time; ESI is fine with a
// handful of concurrent pulls. Per-item exceptions are swallowed - callers
// that need failure handling catch inside fn.
static void parallel_for_n(size_t n, size_t workers,
                           const std::function<void(size_t)>& fn) {
    if (n == 0) return;
    if (n == 1) { try { fn(0); } catch (...) {} return; }
    std::atomic<size_t> next{0};
    size_t w = workers < n ? workers : n;
    std::vector<std::thread> ts;
    for (size_t i = 0; i < w; i++)
        ts.emplace_back([&] {
            size_t k;
            while ((k = next.fetch_add(1)) < n)
                try { fn(k); } catch (...) {}
        });
    for (auto& t : ts) t.join();
}

// --- sha256 (for the PKCE code challenge) -----------------------------------
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t len = 0;
    unsigned char buf[64];
    size_t fill = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
                   (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const void* data, size_t n) {
        const unsigned char* p = (const unsigned char*)data;
        len += n;
        while (n) {
            size_t take = std::min(n, sizeof buf - fill);
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }

    void digest(unsigned char out[32]) {
        uint64_t bits = len * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char z = 0;
        while (fill != 56) update(&z, 1);
        unsigned char lb[8];
        for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - i * 8));
        update(lb, 8);
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = (unsigned char)(h[i] >> (24 - j * 8));
    }
};

static std::string b64url(const unsigned char* p, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        if (i + 1 < n) o += T[(v >> 6) & 63];
        if (i + 2 < n) o += T[v & 63];
    }
    return o;  // no padding (both PKCE and JWT want it stripped)
}

static std::string b64url_decode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    std::string o;
    int acc = 0, bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; o += (char)((acc >> bits) & 0xFF); }
    }
    return o;
}

static std::string random_token(size_t bytes = 32) {
    std::random_device rd;
    std::string raw;
    for (size_t i = 0; i < bytes; i += 4) {
        uint32_t v = rd();
        raw.append((const char*)&v, 4);
    }
    return b64url((const unsigned char*)raw.data(), bytes);
}

// --- config / token storage --------------------------------------------------
static std::filesystem::path config_dir() {
    // STOKER-specific override; unlike XDG_CONFIG_HOME it is not inherited
    // into meaning by the browser we spawn for login
    if (const char* o = std::getenv("STOKER_CONFIG_DIR"); o && *o) {
        std::filesystem::path d = o;
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    std::filesystem::path d = base && *base ? base : ".";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path d = xdg && *xdg ? std::filesystem::path(xdg)
                                          : std::filesystem::path(home ? home : ".") / ".config";
#endif
    d /= "stoker";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

static json load_json_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f) return json::object();
    try { return json::parse(f); } catch (...) { return json::object(); }
}

static void save_json_file(const std::filesystem::path& p, const json& j) {
    std::ofstream f(p);
    f << j.dump(2) << "\n";
    f.close();
#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(p, std::filesystem::perms::owner_read |
                                        std::filesystem::perms::owner_write, ec);
#endif
}

// --- tiny HTTP helpers (curl subprocess, same approach as corp mode) ---------
// Windows' schannel curl fails outright on networks where certificate
// revocation checks can't complete (AV https-scanning, VPNs, some home
// routers). When a request comes back empty there, retry once with
// --ssl-no-revoke and stick with it for the session.
static std::mutex g_curl_mtx;      // worker + refresh/claim/update threads share these
static std::string g_curl_extra;   // sticky flags that made curl work on this box
static time_t g_chain_cooldown = 0;  // after a full-chain failure, back off

static std::string curl_flags() {
    std::lock_guard<std::mutex> l(g_curl_mtx);
    return g_curl_extra.empty() ? std::string() : g_curl_extra + " ";
}

#ifdef _WIN32
static std::string win_system_proxy();
// The escalating broken-TLS fallbacks, in one place (run_curl + download_file)
static std::vector<std::string> curl_fallbacks() {
    std::vector<std::string> tries = {"--ssl-no-revoke",
                                      "--ssl-no-revoke --tlsv1.2 --tls-max 1.2"};
    std::string prx = win_system_proxy();
    if (!prx.empty()) {
        tries.push_back("--proxy \"" + prx + "\"");
        tries.push_back("--proxy \"" + prx + "\" --ssl-no-revoke");
    }
    return tries;
}
#endif

#ifdef _WIN32
// Browsers use the Windows proxy settings; curl does not. On proxy-required
// networks curl's direct connection is killed mid-handshake while the browser
// works, so read the user's configured proxy and offer it to the fallbacks.
static std::string win_system_proxy() {
    std::string q = run_cmd(
        "reg query \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\""
        QUIET);
    if (q.find("ProxyEnable") == std::string::npos) return "";
    size_t en = q.find("ProxyEnable");
    size_t line_end = q.find('\n', en);
    if (q.substr(en, line_end - en).find("0x1") == std::string::npos) return "";
    size_t ps = q.find("ProxyServer");
    if (ps == std::string::npos) return "";
    size_t reg_sz = q.find("REG_SZ", ps);
    if (reg_sz == std::string::npos) return "";
    size_t v0 = q.find_first_not_of(" \t", reg_sz + 6);
    size_t v1 = q.find_first_of("\r\n", v0);
    if (v0 == std::string::npos) return "";
    std::string server = q.substr(v0, v1 - v0);
    // "host:port" or per-protocol "http=...;https=host:port;ftp=..."
    size_t https = server.find("https=");
    if (https != std::string::npos) {
        size_t end = server.find(';', https);
        server = server.substr(https + 6, end == std::string::npos ? std::string::npos
                                                                   : end - https - 6);
    } else if (server.find('=') != std::string::npos) {
        size_t http = server.find("http=");
        if (http == std::string::npos) return "";
        size_t end = server.find(';', http);
        server = server.substr(http + 5, end == std::string::npos ? std::string::npos
                                                                  : end - http - 5);
    }
    for (char c : server)  // registry data only, but it rides a shell command
        if (!(isalnum((unsigned char)c) || c == '.' || c == ':' || c == '-')) return "";
    return server;
}
#endif

static std::string run_curl(const std::string& args) {
    // shutting down: fail fast so mid-sweep joins don't block the window close
    if (!g_run) return "";
    std::string out = run_cmd(("curl -s " + curl_flags() + args + QUIET).c_str());
#ifdef _WIN32
    if (out.empty()) {
        {
            std::lock_guard<std::mutex> l(g_curl_mtx);
            if (time(nullptr) < g_chain_cooldown) return out;  // network down; don't grind
        }
        std::string winner;
        for (auto& f : curl_fallbacks()) {
            {
                std::lock_guard<std::mutex> l(g_curl_mtx);
                if (g_curl_extra == f) continue;
            }
            out = run_cmd(("curl -s " + f + " " + args + QUIET).c_str());
            if (!out.empty()) { winner = f; break; }
        }
        std::lock_guard<std::mutex> l(g_curl_mtx);
        if (!winner.empty()) g_curl_extra = winner;
        else g_chain_cooldown = time(nullptr) + 300;
    }
#endif
    return out;
}

// What went wrong: rerun with errors visible and pull curl's own message.
static std::string curl_error(const std::string& args) {
    std::string raw = run_cmd(("curl -sS " + curl_flags() + args + " 2>&1").c_str());
    size_t at = raw.find("curl: (");
    std::string msg = "no reply";
    if (at != std::string::npos) {
        size_t end = raw.find_first_of("\r\n", at);  // \r would garble the status line
        msg = raw.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }
    // which curl matters when debugging from screenshots
    std::string ver = run_cmd("curl --version" QUIET);
    ver = ver.substr(0, ver.find_first_of("\r\n"));
    if (!ver.empty()) msg += "  [" + ver.substr(0, 48) + "]";
    return msg;
}

// body-only GET for callers that don't care about the status code
static std::string http_get(const std::string& url, const std::string& bearer,
                            int& status, json* headers_out = nullptr,
                            const char* extra_hdr = nullptr);
static std::string http_post_auth(const std::string& url, const std::string& bearer,
                                  int& status) {
    status = 0;
    std::string raw = run_curl("-i --max-time 20 -X POST -H \"Authorization: Bearer " +
                               bearer + "\" \"" + url + "\"");
    if (!raw.empty()) {
        size_t sp = raw.find(' ');
        if (sp != std::string::npos) status = std::atoi(raw.c_str() + sp);
        return raw;
    }
#ifdef _WIN32
    std::string out;
    int st = winhttp_req(url, L"POST", bearer, "", nullptr, out, nullptr);
    if (st > 0) { status = st; return out; }
#endif
    return raw;
}

static std::string http_get_body(const std::string& url) {
    int st = 0;
    return http_get(url, "", st, nullptr);
}

static std::string http_post_form(const std::string& url, const std::string& body) {
    std::string out = run_curl("--max-time 20 -X POST "
                               "-H \"Content-Type: application/x-www-form-urlencoded\" "
                               "-d \"" + body + "\" \"" + url + "\"");
#ifdef _WIN32
    if (out.empty())
        winhttp_req(url, L"POST", "", body, L"application/x-www-form-urlencoded", out, nullptr);
#endif
    return out;
}

static std::string http_post_json(const std::string& url, const std::string& body,
                                  const std::string& bearer = "") {
    // body goes through a temp file: JSON is full of double quotes, which a
    // double-quoted shell argument cannot carry (and @file keeps the payload
    // off the process command line)
    auto tmp = config_dir() / ("post-" + random_token(6) + ".json");
    { std::ofstream f(tmp); f << body; }
    std::string auth = bearer.empty()
        ? std::string()
        : "-H \"Authorization: Bearer " + bearer + "\" ";
    std::string out = run_curl("--compressed --max-time 20 -X POST "
                                "-H \"Content-Type: application/json\" " + auth +
                                "--data @\"" + tmp.string() + "\" \"" + url + "\"");
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
#ifdef _WIN32
    if (out.empty())
        winhttp_req(url, L"POST", bearer, body, L"application/json", out, nullptr);
#endif
    return out;
}

// GET with response headers; returns body, fills status + wanted headers.
// extra_hdr: one extra request header, e.g. the X-Compatibility-Date the
// new-generation ESI endpoints require (curl path only; the Windows winhttp
// fallback skips it and such calls degrade to an ESI error, handled upstream).
static std::string http_get(const std::string& url, const std::string& bearer,
                            int& status, json* headers_out, const char* extra_hdr) {
    std::string args = "-i --compressed --max-time 30 ";
    if (!bearer.empty()) args += "-H \"Authorization: Bearer " + bearer + "\" ";
    if (extra_hdr && *extra_hdr) args += "-H \"" + std::string(extra_hdr) + "\" ";
    args += "\"" + url + "\"";
    std::string raw = run_curl(args);
#ifdef _WIN32
    if (raw.empty()) {
        std::string out;
        int st = winhttp_req(url, L"GET", bearer, "", nullptr, out, headers_out);
        if (st > 0) { status = st; return out; }  // 403/404 are real answers too
    }
#endif
    status = 0;
    const char* seps[] = {"\r\n\r\n", "\n\n"};  // some pipes eat the \r
    size_t sep = std::string::npos;
    size_t sw = 4;
    for (auto* s : seps) {
        sep = raw.find(s);
        sw = std::strlen(s);
        // skip informational/continuation header blocks (e.g. HTTP/1.1 100)
        while (sep != std::string::npos && raw.find("HTTP/", sep + sw) == sep + sw)
            sep = raw.find(s, sep + sw);
        if (sep != std::string::npos) break;
    }
    if (sep == std::string::npos) return raw;
    std::string hdrs = raw.substr(0, sep);
    if (size_t sp = hdrs.find(' '); sp != std::string::npos)
        status = std::atoi(hdrs.c_str() + sp);
    if (headers_out) {
        auto grab = [&](const char* name) -> std::string {
            std::string low = hdrs, key = std::string(name) + ":";
            for (auto& c : low) c = (char)std::tolower((unsigned char)c);
            size_t p = low.find(key);
            if (p == std::string::npos) return "";
            p += key.size();
            size_t e = low.find("\r\n", p);
            std::string v = hdrs.substr(p, e - p);
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            return v;
        };
        (*headers_out)["x-pages"] = grab("x-pages");
        (*headers_out)["last-modified"] = grab("last-modified");
        (*headers_out)["expires"] = grab("expires");
    }
    return raw.substr(sep + sw);
}

// Download url to path (counts only if >= min_size bytes land). WinHTTP first
// on Windows, then curl with the same escalating TLS fallbacks as run_curl -
// keyed on the file arriving, since -o leaves stdout empty either way.
static bool download_file(const std::string& url, const std::filesystem::path& path,
                          size_t min_size) {
    std::error_code ec;
    auto landed = [&]() {
        return std::filesystem::exists(path, ec) &&
               std::filesystem::file_size(path, ec) >= min_size;
    };
    auto try_curl = [&](const std::string& flags) {
        std::filesystem::remove(path, ec);
        run_cmd(("curl -sL " + (flags.empty() ? std::string() : flags + " ") +
                 "--max-time 120 -o \"" + path.string() + "\" \"" + url + "\"" QUIET)
                    .c_str());
        return landed();
    };
    if (try_curl(curl_flags())) return true;
#ifdef _WIN32
    for (auto& f : curl_fallbacks()) {
        {
            std::lock_guard<std::mutex> l(g_curl_mtx);
            if (g_curl_extra == f) continue;
        }
        if (try_curl(f + " ")) {
            std::lock_guard<std::mutex> l(g_curl_mtx);
            g_curl_extra = f;
            return true;
        }
    }
    {   // last resort: the OS HTTP stack
        std::string blob;
        if (winhttp_req(url, L"GET", "", "", nullptr, blob, nullptr) == 200 &&
            blob.size() >= min_size) {
            std::ofstream f(path, std::ios::binary);
            f.write(blob.data(), (std::streamsize)blob.size());
        }
        if (landed()) return true;
    }
#endif
    return false;
}

// --- loopback callback listener ----------------------------------------------
static bool open_browser(const std::string& url) {
#ifdef _WIN32
    return (INT_PTR)ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL) > 32;
#else
    std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#endif
}

// An abandoned browser login (tab closed, no character picked) used to hold
// the listener for its full 3-minute timeout; bumping the generation makes
// the pending wait_for_callback give up within a second so a fresh login can
// take the port immediately. login_pending() tells the UI a login (not a
// data refresh) is what the busy flag is holding.
static std::atomic<int> g_login_gen{0};
static std::atomic<bool> g_login_pending{false};
static bool login_pending() { return g_login_pending.load(); }
static void cancel_pending_login() { g_login_gen++; }

// Blocks until the browser hits /callback; returns the auth code ("" on error).
// Gives up after timeout_s so an abandoned login can't wedge the caller (and
// can't hold the port against the next attempt). err is "cancelled" when
// cancel_pending_login() aborted the wait (the caller stays quiet about it).
static std::string wait_for_callback(const std::string& expect_state, std::string& err,
                                     int timeout_s = 180) {
    const int my_gen = g_login_gen.load();
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = "WSAStartup failed"; return ""; }
#endif
    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { err = "socket() failed"; return ""; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(CALLBACK_PORT);
    if (bind(srv, (sockaddr*)&addr, sizeof addr) != 0 || listen(srv, 4) != 0) {
        err = "cannot listen on localhost:" + std::to_string(CALLBACK_PORT) +
              " (already in use?)";
        CLOSESOCK(srv);
#ifdef _WIN32
        WSACleanup();
#endif
        return "";
    }
    time_t deadline = time(nullptr) + timeout_s;
    std::string code;
    int conns = 0;
    while (code.empty() && conns < 32) {
        if (g_login_gen.load() != my_gen) { err = "cancelled"; break; }
        // wait for a connection in 1s slices (so a cancel lands fast), but
        // never past the deadline
        long left = (long)(deadline - time(nullptr));
        if (left <= 0) { err = "login timed out (nothing came back from the browser)"; break; }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv, &rd);
        timeval tv{};
        tv.tv_sec = 1;
        int sr = select((int)srv + 1, &rd, nullptr, nullptr, &tv);
        if (sr < 0) { err = "login timed out (nothing came back from the browser)"; break; }
        if (sr == 0) continue;  // slice elapsed; re-check cancel + deadline
        sock_t c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        conns++;
        std::string req;
        char buf[4096];
        // request line + headers arrive in one or two reads for a local socket
        for (int r = 0; r < 4 && req.find("\r\n\r\n") == std::string::npos; r++) {
            int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.append(buf, n);
        }
        bool is_cb = req.compare(0, 14, "GET /callback?") == 0;
        std::string body;
        if (is_cb) {
            auto param = [&](const char* k) -> std::string {
                std::string key = std::string(k) + "=";
                size_t p = req.find(key);
                if (p == std::string::npos) return "";
                p += key.size();
                size_t e = req.find_first_of("& \r\n", p);
                return req.substr(p, e - p);
            };
            if (param("state") != expect_state) {
                err = "state mismatch (stale login tab?)";
                body = "<h2>STOKER: login rejected (state mismatch). Try again.</h2>";
            } else if (!param("code").empty()) {
                code = param("code");
                body = "<h2>STOKER: logged in. You can close this tab.</h2>";
            } else {
                err = "SSO returned no code";
                body = "<h2>STOKER: login failed.</h2>";
            }
        } else {
            body = "<h2>STOKER waiting for EVE login...</h2>";  // favicon etc.
        }
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        send(c, resp.c_str(), (int)resp.size(), 0);
        CLOSESOCK(c);
    }
    CLOSESOCK(srv);
#ifdef _WIN32
    WSACleanup();
#endif
    return code;
}

// --- multi-character token store ----------------------------------------------
// tokens.json holds {"characters": [...]}; one entry per logged-in character.
// A character grants its own corp's structures, so several characters =
// several corp tabs. Old single-token files migrate transparently.
// Guards every read-modify-write of tokens.json: the 300s poll refreshes
// tokens while an alt+c login may add a character; unsynchronized saves can
// drop a character or a rotated refresh token (which kills that login).
static std::mutex g_store_mtx;

static json load_characters() {
    json t = load_json_file(config_dir() / "tokens.json");
    if (t.contains("characters") && t["characters"].is_array()) return t["characters"];
    if (t.value("character_id", 0LL) != 0) return json::array({t});  // pre-multi format
    return json::array();
}

static void save_characters(const json& chars) {
    save_json_file(config_dir() / "tokens.json", json{{"characters", chars}});
}

// --- SSO ----------------------------------------------------------------------
// Runs the browser PKCE flow and adds (or re-auths) one character in the
// store. `progress` gets human-readable status lines (stdout for the CLI
// path, the TUI status line for alt+c); out_name receives the character
// name on success. Safe to run from a background thread: no stdin, and the
// callback listener times out instead of blocking forever.
static bool login(const std::string& client_id, std::string& err,
                  std::string* out_name = nullptr,
                  const std::function<void(const std::string&)>& progress = {}) {
    auto say = [&](const std::string& s) {
        if (progress) progress(s);
        else std::printf("%s\n", s.c_str());
    };
    std::string verifier = random_token(32);
    Sha256 sh;
    sh.update(verifier.data(), verifier.size());
    unsigned char dig[32];
    sh.digest(dig);
    std::string challenge = b64url(dig, 32);
    std::string state = random_token(16);

    std::string redirect = "http://localhost:" + std::to_string(CALLBACK_PORT) + "/callback";
    std::string url = "https://login.eveonline.com/v2/oauth/authorize/?response_type=code"
                      "&redirect_uri=" + urlenc(redirect) +
                      "&client_id=" + client_id +
                      "&scope=" + urlenc(g_scopes) +
                      "&code_challenge=" + challenge +
                      "&code_challenge_method=S256&state=" + state;

    say("opening EVE login in your browser...");
    if (!open_browser(url))
        say("could not open a browser; paste this URL into one: " + url);
    say("waiting for the login to come back on localhost:" +
        std::to_string(CALLBACK_PORT) + " ...");

    g_login_pending = true;
    std::string code = wait_for_callback(state, err);
    g_login_pending = false;
    if (code.empty()) { if (err.empty()) err = "no auth code received"; return false; }

    std::string body = "grant_type=authorization_code&code=" + urlenc(code) +
                       "&client_id=" + client_id + "&code_verifier=" + verifier;
    std::string resp = http_post_form(SSO_TOKEN_URL, body);
    json tok = json::object();
    try { tok = json::parse(resp); } catch (...) { tok = json::object(); }
    auto sv = [&](const char* k) {
        return tok.contains(k) && tok[k].is_string() ? tok[k].get<std::string>()
                                                     : std::string();
    };
    if (sv("access_token").empty()) {
        if (!sv("error").empty())
            err = "token exchange failed: " + sv("error") + " " + sv("error_description");
        else if (resp.empty())
            err = "token exchange failed: " +
                  curl_error("--max-time 20 -X POST "
                             "-H \"Content-Type: application/x-www-form-urlencoded\" "
                             "-d \"" + body + "\" \"" + std::string(SSO_TOKEN_URL) + "\"") +
                  " (a VPN, proxy, or antivirus intercepting HTTPS is the usual cause)";
        else
            err = "token exchange failed: login.eveonline.com sent an HTTP error page "
                  "instead of a token (usually an app-registration problem on "
                  "developers.eveonline.com)";
        return false;
    }

    // character id/name ride inside the JWT payload; no verification needed,
    // we only use them for display and the /characters lookup
    std::string at = sv("access_token");
    std::string payload;
    size_t d1 = at.find('.'), d2 = at.find('.', d1 + 1);
    if (d1 != std::string::npos && d2 != std::string::npos)
        payload = b64url_decode(at.substr(d1 + 1, d2 - d1 - 1));
    long long char_id = 0;
    std::string char_name;
    try {
        json p = json::parse(payload);
        std::string sub = p.value("sub", "");  // "CHARACTER:EVE:2112..."
        char_id = std::atoll(sub.substr(sub.rfind(':') + 1).c_str());
        char_name = p.value("name", "");
    } catch (...) {}
    if (!char_id) { err = "could not read character id from token"; return false; }

    json entry = {{"access_token", at},
                  {"refresh_token", sv("refresh_token")},
                  {"expires_at", (long long)time(nullptr) + tok.value("expires_in", 1199) - 60},
                  {"character_id", char_id},
                  {"character_name", char_name}};
    json chars;
    {
        std::lock_guard<std::mutex> lk(g_store_mtx);
        chars = load_characters();
        bool replaced = false;
        for (auto& c : chars)
            if (c.value("character_id", 0LL) == char_id) { c = entry; replaced = true; }
        if (!replaced) chars.push_back(entry);
        save_characters(chars);
    }
    if (out_name) *out_name = char_name;
    say("logged in as " + char_name + " (" + std::to_string(chars.size()) +
        (chars.size() == 1 ? " character" : " characters") + " on file)");
    return true;
}

// Valid access token for one stored character, refreshing over the wire when
// expired (the whole store is saved back so rotated refresh tokens stick).
// Never opens a browser (safe to call from the fetch thread mid-TUI).
static std::string ensure_token_entry(const std::string& client_id, json& chars, size_t i) {
    json& t = chars[i];
    if (!t.contains("access_token") || !t.value("character_id", 0LL)) return "";
    if ((long long)time(nullptr) < t.value("expires_at", 0LL))
        return t["access_token"];
    std::string rt = t.value("refresh_token", "");
    if (rt.empty()) return "";
    json tok;
    try {
        tok = json::parse(http_post_form(
            SSO_TOKEN_URL, "grant_type=refresh_token&refresh_token=" + urlenc(rt) +
                               "&client_id=" + client_id));
    } catch (...) { return ""; }
    if (!tok.contains("access_token")) return "";
    t["access_token"] = tok["access_token"];
    if (tok.contains("refresh_token")) t["refresh_token"] = tok["refresh_token"];
    t["expires_at"] = (long long)time(nullptr) + tok.value("expires_in", 1199) - 60;
    {
        // merge into a fresh load so a concurrent login's new character
        // can't be clobbered by this save
        std::lock_guard<std::mutex> lk(g_store_mtx);
        json all = load_characters();
        bool found = false;
        for (auto& c : all)
            if (c.value("character_id", 0LL) == t.value("character_id", 0LL)) { c = t; found = true; }
        if (!found) all.push_back(t);
        save_characters(all);
    }
    return t["access_token"];
}

// Scopes ride in the access token's JWT "scp" claim; a token minted before
// a scope was added to the request never has it (relogin required).
static bool token_has_scope(const std::string& tok, const std::string& scope) {
    size_t d1 = tok.find('.'), d2 = tok.find('.', d1 + 1);
    if (d1 == std::string::npos || d2 == std::string::npos) return false;
    try {
        json pl = json::parse(b64url_decode(tok.substr(d1 + 1, d2 - d1 - 1)));
        if (!pl.contains("scp")) return false;
        if (pl["scp"].is_string()) return pl["scp"].get<std::string>() == scope;
        for (auto& s : pl["scp"])
            if (s.is_string() && s.get<std::string>() == scope) return true;
    } catch (...) {}
    return false;
}

static bool have_login() {
    json chars = load_characters();
    for (auto& c : chars)
        if (c.contains("refresh_token") && c.value("character_id", 0LL) != 0) return true;
    return false;
}

// "Tue, 08 Jul 2026 19:20:33 GMT" -> "2026-07-08T19:20:33Z" ("" if not that shape)
static std::string http_date_to_iso(const std::string& h) {
    static const char* MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {0};
    if (std::sscanf(h.c_str(), "%*[^,], %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6)
        return "";
    const char* p = std::strstr(MONTHS, mon);
    if (!p) return "";
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  y, (int)(p - MONTHS) / 3 + 1, d, hh, mm, ss);
    return buf;
}

// Blocks/day a structure burns, from its ONLINE service list. Mirrors the
// corp backend's _fuel_per_day (ESI service name -> Standup module behind it
// + base blocks/hour from dogma attr 2109; hull role bonuses cut 20-25%).
// -1 = no recognised services, nothing to base a rate on.
static double fuel_per_day(const std::string& type_name, const json& services) {
    static const struct { const char* svc; const char* mod; double rate; } TBL[] = {
        {"Jump Bridge Access", "ansiblex", 30},
        {"Jump Access", "pharolux", 15},
        {"Clone Bay", "cloning", 10},
        {"Reprocessing", "reprocessing", 10},
        {"Moon Drilling", "moon_drill", 5},
        {"Automatic Moon Drilling", "metenox_drill", 5},
        {"Manufacturing (Standard)", "mfg_standard", 12},
        {"Manufacturing (Capitals)", "cap_shipyard", 24},
        {"Composite Reactions", "composite_reactor", 15},
        {"Hybrid Reactions", "hybrid_reactor", 15},
        {"Biochemical Reactions", "biochem_reactor", 15},
        {"Blueprint Copying", "research_lab", 12},
        {"Material Efficiency Research", "research_lab", 12},
        {"Time Efficiency Research", "research_lab", 12},
        {"Invention", "invention_lab", 12},
        {"Market", "market_hub", 40},
    };
    auto in = [](const std::string& v, std::initializer_list<const char*> set) {
        for (auto* s : set) if (v == s) return true;
        return false;
    };
    std::map<std::string, double> mods;  // dedupes the shared research lab
    if (services.is_array())
        for (auto& s : services) {
            if (s.value("state", "") != "online") continue;
            std::string name = s.value("name", "");
            for (auto& t : TBL)
                if (name == t.svc) mods[t.mod] = t.rate;
        }
    if (mods.empty()) return -1;
    double total = 0;
    for (auto& [mod, rate] : mods) {
        double mult = 1.0;
        if (in(type_name, {"Astrahus", "Fortizar", "Keepstar"}) &&
            in(mod, {"cloning", "market_hub"}))
            mult = 0.75;
        else if (in(type_name, {"Raitaru", "Azbel", "Sotiyo"}) &&
                 in(mod, {"mfg_standard", "cap_shipyard", "research_lab", "invention_lab"}))
            mult = 0.75;
        else if (in(mod, {"reprocessing", "composite_reactor", "hybrid_reactor",
                          "biochem_reactor"})) {
            if (type_name == "Athanor") mult = 0.80;
            else if (type_name == "Tatara") mult = 0.75;
        }
        total += rate * mult;
    }
    return std::round(total * 24);
}

// --- the fetch: one corp's structures straight off ESI ------------------------
// Returns /bpos-shaped JSON, or "" with err set (keeps the last table on screen).
// "SYSTEM - VI-20" -> planet 6, moon 20: the corp's Metenox naming convention
// carries the moon designation, which is the join key into the moon catalog
static bool parse_moon_tag(const std::string& name, int& planet, int& moonno) {
    size_t p = name.rfind(" - ");
    if (p == std::string::npos) return false;
    std::string tail = name.substr(p + 3);
    size_t d = tail.find('-');
    if (d == std::string::npos || d == 0 || d + 1 >= tail.size()) return false;
    int v = 0, prev = 0;
    for (size_t i = 0; i < d; i++) {  // roman planet numeral
        char c = tail[i];
        int x = c == 'I' ? 1 : c == 'V' ? 5 : c == 'X' ? 10 : c == 'L' ? 50 : 0;
        if (!x) return false;
        v += x;
        if (prev && prev < x) v -= 2 * prev;  // subtractive pairs (IV, IX, ...)
        prev = x;
    }
    long long m = 0;
    for (size_t i = d + 1; i < tail.size(); i++) {
        if (!isdigit((unsigned char)tail[i])) return false;
        m = m * 10 + (tail[i] - '0');
    }
    planet = v;
    moonno = (int)m;
    return planet > 0 && moonno > 0;
}

// pulls a "key: &anchor 12345" style value out of a notification's YAML text
static long long yaml_ll(const std::string& text, const std::string& key) {
    size_t p = text.find(key + ":");
    if (p == std::string::npos) return 0;
    p += key.size() + 1;
    while (p < text.size() && (text[p] == ' ' || text[p] == '&')) {
        if (text[p] == '&') {
            while (p < text.size() && text[p] != ' ') p++;
        } else {
            p++;
        }
    }
    long long v = 0;
    while (p < text.size() && isdigit((unsigned char)text[p])) v = v * 10 + (text[p++] - '0');
    return v;
}

// --- POS control towers --------------------------------------------------------
// Hourly upkeep from the tower's name; reproduces invControlTowerResources
// exactly (re-verified against the SDE 2026-08-20). Base large = 40 blocks/hr,
// medium/small scale by half/quarter; faction hulls (Blood/Guristas/Sansha/
// Serpentis/Angel) burn 10% less (36) and pirate elite hulls (Dark Blood/Dread
// Guristas/True Sansha/Shadow/Domination) 20% less (32). Strontium (reinforce
// upkeep) is 400/hr on a large regardless of faction, scaled by size the same way.
static bool pos_fuel_rate(const std::string& tower, double& blocks_hr,
                          double& stront_hr) {
    if (tower.find("Control Tower") == std::string::npos) return false;
    double base = 40;
    for (const char* f : {"Dark Blood", "Dread Guristas", "True Sansha",
                          "Domination", "Shadow"})
        if (tower.rfind(f, 0) == 0) base = 32;
    if (base == 40)
        for (const char* f : {"Blood", "Guristas", "Sansha", "Serpentis", "Angel"})
            if (tower.rfind(f, 0) == 0) base = 36;
    double size = tower.find("Small") != std::string::npos    ? 0.25
                  : tower.find("Medium") != std::string::npos ? 0.5
                                                              : 1.0;
    blocks_hr = base * size;
    stront_hr = 400 * size;
    return true;
}

// which race's tower art a hull wears (pirate factions map to their lore parent)
static const char* pos_race_of(const std::string& tower) {
    for (const char* f : {"Amarr", "Blood", "Dark Blood", "Sansha", "True Sansha"})
        if (tower.rfind(f, 0) == 0) return "amarr";
    for (const char* f : {"Caldari", "Guristas", "Dread Guristas"})
        if (tower.rfind(f, 0) == 0) return "caldari";
    for (const char* f : {"Gallente", "Serpentis", "Shadow"})
        if (tower.rfind(f, 0) == 0) return "gallente";
    return "minmatar";  // Minmatar / Angel / Domination
}

static std::string fetch_corp(const std::string& tok, long long corp_id,
                              const std::string& corp_display, long long char_id,
                              std::string& err) try {
    int st = 0;
    progress(corp_display + ": structures");
    json structures = json::array(), hdr = json::object();
    {
        // page 1 sets the tone (auth errors, page count); the rest pull in
        // parallel and ingest in order so multi-page corps stop paying
        // one-curl-per-page wall clock
        std::string body = http_get(ESI + std::string("/corporations/") +
                                        std::to_string(corp_id) +
                                        "/structures/?datasource=tranquility&page=1",
                                    tok, st, &hdr);
        if (st == 403) {
            err = "ESI 403: that character needs the Station_Manager "
                  "(or Director) in-game role";
            return "";
        }
        if (st != 200) { err = "ESI error " + std::to_string(st); return ""; }
        json j;
        try { j = json::parse(body); } catch (...) { err = "ESI sent junk"; return ""; }
        if (!j.is_array()) { err = "ESI sent junk"; return ""; }
        for (auto& s : j) structures.push_back(s);
        int pages = 1;
        if (hdr.value("x-pages", std::string()) != "")
            pages = std::atoi(hdr["x-pages"].get<std::string>().c_str());
        if (pages > 20) pages = 20;
        if (pages > 1) {
            std::vector<json> more((size_t)pages - 1);
            parallel_for_n(more.size(), 6, [&](size_t i) {
                int pst = 0;
                std::string pb = http_get(ESI + std::string("/corporations/") +
                                              std::to_string(corp_id) +
                                              "/structures/?datasource=tranquility&page=" +
                                              std::to_string((int)i + 2),
                                          tok, pst);
                if (pst == 200) more[i] = json::parse(pb);
            });
            for (auto& j2 : more) {
                if (!j2.is_array()) { err = "ESI sent junk"; return ""; }
                for (auto& s : j2) structures.push_back(s);
            }
        }
    }

    // resolve type + system ids in one public lookup
    std::vector<long long> ids;
    auto want = [&](long long id) {
        if (id && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    };
    for (auto& s : structures) {
        want(s.value("type_id", 0LL));
        want(s.value("system_id", 0LL));
    }
    std::map<long long, std::string> names;
    if (!ids.empty()) {
        std::string body = "[";
        for (size_t i = 0; i < ids.size(); i++)
            body += (i ? "," : "") + std::to_string(ids[i]);
        body += "]";
        try {
            json r = json::parse(http_post_json(
                ESI + std::string("/universe/names/?datasource=tranquility"), body));
            for (auto& e : r) names[e.value("id", 0LL)] = e.value("name", "");
        } catch (...) {}
    }

    // Secondary fuel (2ND FUEL column): Magmatic Gas / Liquid Ozone live in
    // the corp-assets fuel bay (location_flag StructureFuel; discovered via
    // SeAT 2026-07-09, gas is NOT in a separate reagent bay). Needs the
    // esi-assets.read_corporation_assets.v1 scope + an in-game Director role;
    // anything short of that 403s and the column shows "?".
    std::map<long long, double> gas_at, ozone_at;
    std::map<long long, std::map<long long, double>> goo_at;  // structure -> type -> qty
    std::map<long long, long long> rig_at;  // structure -> fitted moon drilling rig type
    bool assets_ok = false;
    std::string fuel2_status = "off";  // ok | relogin | director | error | off
    if (!token_has_scope(tok, "esi-assets.read_corporation_assets.v1")) {
        if (g_scopes.find("read_corporation_assets") != std::string::npos)
            fuel2_status = "relogin";  // we ask for it now; this token predates that
    } else {
        progress(corp_display + ": corp assets");
        json ahdr = json::object();
        auto ingest_assets = [&](json& j) {
            for (auto& it : j) {
                std::string flag = it.value("location_flag", "");
                long long loc = it.value("location_id", 0LL);
                long long tid = it.value("type_id", 0LL);
                double q = it.value("quantity", 0.0);
                if (flag == "StructureFuel") {
                    if (tid == 81143) gas_at[loc] += q;        // Magmatic Gas
                    else if (tid == 16273) ozone_at[loc] += q;  // Liquid Ozone
                } else if (flag == "MoonMaterialBay") {
                    goo_at[loc][tid] += q;  // mined moongoo output
                } else if (flag.rfind("RigSlot", 0) == 0 &&
                           (tid == 46325 || tid == 46326 ||   // M-Set Drilling Stability I/II
                            tid == 46327 || tid == 46328)) {  // L-Set Drilling Proficiency I/II
                    rig_at[loc] = tid;  // stretches the fire window + popped field life
                }
            }
        };
        // page 1 sequential (auth verdict + page count), the rest in parallel;
        // a failed later page keeps the partial totals but flags "error" so
        // the bars never silently undercount
        std::string body = http_get(ESI + std::string("/corporations/") +
                                        std::to_string(corp_id) +
                                        "/assets/?datasource=tranquility&page=1",
                                    tok, st, &ahdr);
        if (st == 403) fuel2_status = "director";
        else if (st != 200) fuel2_status = "error";
        else {
            json j;
            bool ok1 = true;
            try { j = json::parse(body); } catch (...) { ok1 = false; }
            if (ok1 && j.is_array()) {
                assets_ok = true;
                ingest_assets(j);
                int pages = 1;
                if (ahdr.value("x-pages", std::string()) != "")
                    pages = std::atoi(ahdr["x-pages"].get<std::string>().c_str());
                if (pages > 40) pages = 40;
                if (pages > 1) {
                    std::vector<json> more((size_t)pages - 1);
                    parallel_for_n(more.size(), 6, [&](size_t i) {
                        int pst = 0;
                        std::string pb = http_get(
                            ESI + std::string("/corporations/") +
                                std::to_string(corp_id) +
                                "/assets/?datasource=tranquility&page=" +
                                std::to_string((int)i + 2),
                            tok, pst);
                        if (pst == 200) more[i] = json::parse(pb);
                    });
                    for (auto& j2 : more) {
                        if (j2.is_array()) ingest_assets(j2);
                        else {
                            // a page is missing: totals would undercount
                            assets_ok = false;
                            fuel2_status = "error";
                        }
                    }
                }
            }
        }
        if (assets_ok) fuel2_status = "ok";
    }

    // Moongoo valuation: public market averages (hourly cache) plus type
    // name/volume (session cache; the moon-material set is a few dozen types).
    // The slow HTTP work happens OUTSIDE the mutex so parallel corp sweeps
    // never serialize on a 30s curl call; the hour slot is claimed up front,
    // so a failed price pull simply retries next hour (best effort).
    static std::mutex s_val_mtx;
    static std::map<long long, double> s_price;
    static time_t s_price_at = 0;
    static std::map<long long, std::pair<std::string, double>> s_type;  // name, m3/unit
    if (!goo_at.empty()) {
        bool need_prices = false;
        std::vector<long long> missing;
        {
            std::lock_guard<std::mutex> vl(s_val_mtx);
            // type names/volumes are static game data: seed from the disk
            // cache once so a fresh boot doesn't redo dozens of lookups
            static bool s_type_loaded = false;
            if (!s_type_loaded) {
                s_type_loaded = true;
                json tc = load_json_file(config_dir() / "type-cache.json");
                if (tc.is_object())
                    for (auto it = tc.begin(); it != tc.end(); ++it)
                        if (it.value().is_array() && it.value().size() == 2 &&
                            it.value()[0].is_string() && it.value()[1].is_number())
                            s_type[std::atoll(it.key().c_str())] = {
                                it.value()[0].get<std::string>(),
                                it.value()[1].get<double>()};
            }
            if (time(nullptr) - s_price_at > 3600) {
                s_price_at = time(nullptr);
                need_prices = true;
            }
            for (auto& lt : goo_at)
                for (auto& tq : lt.second)
                    if (!s_type.count(tq.first)) {
                        // placeholder claims the id so sibling threads skip it
                        s_type[tq.first] = {"type " + std::to_string(tq.first), 0.0};
                        missing.push_back(tq.first);
                    }
        }
        if (need_prices) try {
            json pj = json::parse(http_get_body(
                ESI + std::string("/markets/prices/?datasource=tranquility")));
            std::lock_guard<std::mutex> vl(s_val_mtx);
            for (auto& e : pj) {
                double ap = e.contains("average_price") && e["average_price"].is_number()
                                ? e["average_price"].get<double>()
                                : e.value("adjusted_price", 0.0);
                s_price[e.value("type_id", 0LL)] = ap;
            }
        } catch (...) { /* valuation is best-effort */ }
        if (!missing.empty())
            progress(corp_display + ": type names (" + std::to_string(missing.size()) + ")");
        parallel_for_n(missing.size(), 6, [&](size_t i) {
            long long tid = missing[i];
            // a failed pull keeps the placeholder (parallel_for_n eats throws)
            json tj = json::parse(http_get_body(ESI + std::string("/universe/types/") +
                                                std::to_string(tid) +
                                                "/?datasource=tranquility"));
            std::lock_guard<std::mutex> vl(s_val_mtx);
            s_type[tid] = {tj.value("name", "type " + std::to_string(tid)),
                           tj.value("volume", 0.0)};
        });
        if (!missing.empty()) {
            json tc;
            std::lock_guard<std::mutex> vl(s_val_mtx);
            for (auto& t : s_type)
                // skip unresolved "type NNNN" placeholders so they retry
                if (t.second.first.rfind("type ", 0) != 0)
                    tc[std::to_string(t.first)] =
                        json::array({t.second.first, t.second.second});
            save_json_file(config_dir() / "type-cache.json", tc);
        }
    }

    // Athanor/Tatara moon-pull schedule. Needs esi-industry.read_corporation_mining.v1
    // on the token (in the default SCOPE since v2.5.0; the shared dev app allows
    // it - tokens minted earlier need a re-login). natural_decay_time is the
    // server-computed auto-fracture moment (chunk arrival + ~3h fire window,
    // already stretched by any drilling rig), so the UI never guesses it.
    struct Extr { std::string start, arrival, decay; };
    std::map<long long, Extr> extr;  // sid -> newest schedule
    bool extr_ok = false;
    if (token_has_scope(tok, "esi-industry.read_corporation_mining.v1")) {
        int xst = 0;
        std::string xbody = http_get(ESI + std::string("/corporation/") +
                                         std::to_string(corp_id) +
                                         "/mining/extractions/?datasource=tranquility",
                                     tok, xst);
        if (xst == 200) try {
            json xj = json::parse(xbody);
            if (xj.is_array()) {
                extr_ok = true;
                for (auto& e : xj) {
                    long long esid = e.value("structure_id", 0LL);
                    std::string arr = e.value("chunk_arrival_time", "");
                    if (arr > extr[esid].arrival)  // newest schedule wins (ISO sorts)
                        extr[esid] = {e.value("extraction_start_time", ""), arr,
                                      e.value("natural_decay_time", "")};
                }
            }
        } catch (...) {}
    }

    time_t now = time(nullptr);
    char now_iso[32];
    std::tm now_tm{};
#ifdef _WIN32
    gmtime_s(&now_tm, &now);
#else
    gmtime_r(&now, &now_tm);
#endif
    std::strftime(now_iso, sizeof now_iso, "%Y-%m-%dT%H:%M:%SZ", &now_tm);

    json out;
    out["corp"] = corp_display;  // the header brands itself with this
    out["fuel2_status"] = fuel2_status;
    out["extractions_ok"] = extr_ok;
    out["timers_ok"] = true;  // this feed carries state timers; the corp feed doesn't
    out["pulled_at"] = now_iso;
    std::string lm = http_date_to_iso(hdr.value("last-modified", std::string()));
    std::string ex = http_date_to_iso(hdr.value("expires", std::string()));
    if (!lm.empty()) out["esi_last_modified"] = lm;
    if (!ex.empty()) out["esi_expires"] = ex;
    out["structures"] = json::array();
    for (auto& s : structures) {
        json r;
        r["structure_id"] = s.value("structure_id", 0LL);
        r["name"] = s.value("name", "");
        r["system"] = names.count(s.value("system_id", 0LL))
                          ? names[s.value("system_id", 0LL)] : "";
        r["type"] = names.count(s.value("type_id", 0LL))
                        ? names[s.value("type_id", 0LL)] : "";
        r["state"] = s.value("state", "");
        if (s.contains("state_timer_start") && s["state_timer_start"].is_string())
            r["state_timer_start"] = s["state_timer_start"];
        if (s.contains("state_timer_end") && s["state_timer_end"].is_string())
            r["state_timer_end"] = s["state_timer_end"];
        if (extr_ok) {
            auto eit = extr.find(s.value("structure_id", 0LL));
            if (eit != extr.end()) {
                r["extraction_start"] = eit->second.start;
                r["chunk_arrival"] = eit->second.arrival;
                if (!eit->second.decay.empty())
                    r["natural_decay"] = eit->second.decay;
            }
        }
        std::string svc;
        int sv_on = 0, sv_off = 0;
        if (s.contains("services") && s["services"].is_array())
            for (auto& v : s["services"]) {
                if (v.value("state", "") != "online") {
                    sv_off++;
                    continue;
                }
                sv_on++;
                if (!svc.empty()) svc += ", ";
                svc += v.value("name", "");
            }
        r["services"] = svc;
        r["services_online"] = sv_on;
        r["services_offline"] = sv_off;
        if (s.contains("unanchors_at") && s["unanchors_at"].is_string())
            r["unanchors_at"] = s["unanchors_at"];
        std::string fe = s.value("fuel_expires", "");
        r["fuel_expires"] = fe;
        double days = -1;
        if (!fe.empty()) {
            time_t t = parse_iso(fe);
            if (t) {
                days = (double)(t - now) / 86400.0;
                r["fuel_days_left"] = days;
            }
        }
        // same math as the corp backend: top-up-to-30d from the service rate
        double bpd = fuel_per_day(r["type"].get<std::string>(),
                                  s.contains("services") ? s["services"] : json());
        if (bpd > 0 && days >= 0) {
            double need = std::max(0.0, std::round((30 - days) * bpd));
            double have = std::max(0.0, std::round(days * bpd));
            r["blocks_per_day"] = bpd;
            r["blocks_to_30d"] = need;
            r["m3_to_30d"] = need * 5;  // fuel blocks are 5 m3 each
            r["blocks_now"] = have;
            r["m3_now"] = have * 5;
        }
        if (r["type"] == "Metenox Moon Drill") {
            // 200 Magmatic Gas/hr on top of the blocks
            r["gas_per_day"] = 200 * 24;
            r["gas_month_units"] = 200 * 24 * 30;
            r["gas_month_m3"] = std::round(200 * 24 * 30 * 0.01);
        }
        if (assets_ok) {
            long long sid = r["structure_id"].get<long long>();
            std::string ty = r["type"].get<std::string>();
            if (ty == "Athanor" || ty == "Tatara") {
                // fitted moon drilling rig from the same corp-assets sweep;
                // tier 0 = confirmed bare (assets were readable, no rig found)
                long long rt = rig_at.count(sid) ? rig_at[sid] : 0;
                r["drill_rig_tier"] = rt == 0 ? 0 : (rt == 46326 || rt == 46328) ? 2 : 1;
                if (rt == 46325) r["drill_rig"] = "M-Set Moon Drilling Stability I";
                else if (rt == 46326) r["drill_rig"] = "M-Set Moon Drilling Stability II";
                else if (rt == 46327) r["drill_rig"] = "L-Set Moon Drilling Proficiency I";
                else if (rt == 46328) r["drill_rig"] = "L-Set Moon Drilling Proficiency II";
            }
            if (ty == "Metenox Moon Drill")
                r["fuel2_units"] = gas_at.count(sid) ? gas_at[sid] : 0.0;
            else if (ty.rfind("Ansiblex", 0) == 0 || ty.rfind("Pharolux", 0) == 0)
                r["fuel2_units"] = ozone_at.count(sid) ? ozone_at[sid] : 0.0;
            if (ty == "Metenox Moon Drill") {
                // mined output sitting in the moon material bay, priced at the
                // public market average
                double tm3 = 0, tisk = 0;
                std::vector<json> items;
                if (goo_at.count(sid)) {
                    std::lock_guard<std::mutex> vl(s_val_mtx);
                    for (auto& tq : goo_at[sid]) {
                        auto& ti = s_type[tq.first];
                        double m3 = ti.second * tq.second;
                        double isk = (s_price.count(tq.first) ? s_price[tq.first] : 0.0) *
                                     tq.second;
                        tm3 += m3;
                        tisk += isk;
                        items.push_back({{"name", ti.first},
                                         {"qty", tq.second},
                                         {"m3", m3},
                                         {"isk", isk}});
                    }
                }
                std::sort(items.begin(), items.end(), [](const json& a, const json& b) {
                    return a.value("isk", 0.0) > b.value("isk", 0.0);
                });
                r["goo_m3"] = tm3;
                r["goo_isk"] = tisk;
                r["goo_capacity"] = 500000.0;  // Metenox moon material storage m3
                r["goo"] = items;
            }
        }
        out["structures"].push_back(std::move(r));
    }

    // --- POS control towers: the classic starbases, Director-gated. The list
    // endpoint gives hull/moon/state, the per-tower detail gives the actual
    // fuel bay (blocks + strontium). CCP ships no fuel_expires for towers, so
    // one is synthesized from stock / burn rate - every clock, gauge and
    // refuel tracker downstream then works unchanged. Towers in systems where
    // the corp's alliance holds sovereignty burn 25% less (nullsec only).
    std::string pos_status = "off";
    if (!token_has_scope(tok, "esi-corporations.read_starbases.v1")) {
        if (g_scopes.find("read_starbases") != std::string::npos)
            pos_status = "relogin";  // we ask for it now; this token predates that
    } else {
        json towers = json::array(), phdr = json::object();
        int pst = 0;
        pos_status = "ok";
        for (int page = 1, pages = 1; page <= pages && page <= 10; page++) {
            json* h = page == 1 ? &phdr : nullptr;
            std::string body = http_get(ESI + std::string("/corporations/") +
                                            std::to_string(corp_id) +
                                            "/starbases/?datasource=tranquility&page=" +
                                            std::to_string(page),
                                        tok, pst, h);
            if (pst != 200) {
                pos_status = pst == 403 ? "director" : "error";
                break;
            }
            json j;
            try { j = json::parse(body); } catch (...) { pos_status = "error"; break; }
            if (!j.is_array()) { pos_status = "error"; break; }
            for (auto& t : j) towers.push_back(t);
            if (page == 1 && phdr.value("x-pages", std::string()) != "")
                pages = std::atoi(phdr["x-pages"].get<std::string>().c_str());
        }
        if (pos_status == "ok" && !towers.empty()) {
            // tower hull + system names (the structures pull above resolved
            // only its own ids)
            std::vector<long long> pids;
            for (auto& t : towers) {
                for (long long id : {t.value("type_id", 0LL), t.value("system_id", 0LL)})
                    if (id && !names.count(id) &&
                        std::find(pids.begin(), pids.end(), id) == pids.end())
                        pids.push_back(id);
            }
            if (!pids.empty()) try {
                std::string body = "[";
                for (size_t i = 0; i < pids.size(); i++)
                    body += (i ? "," : "") + std::to_string(pids[i]);
                body += "]";
                json r = json::parse(http_post_json(
                    ESI + std::string("/universe/names/?datasource=tranquility"), body));
                for (auto& e : r) names[e.value("id", 0LL)] = e.value("name", "");
            } catch (...) {}
            // moon names: /universe/names does not resolve moons, so each one
            // is a single lookup - cached for the process lifetime (they never
            // change), placeholder-claimed so parallel corp sweeps skip it
            static std::mutex s_pos_mtx;
            static std::map<long long, std::string> s_moon_name;
            static std::map<long long, long long> s_corp_ally;   // corp -> alliance
            static std::map<long long, long long> s_sov;         // system -> sov alliance
            static time_t s_sov_at = 0;
            std::vector<long long> want_moons;
            bool need_sov = false;
            {
                std::lock_guard<std::mutex> pl(s_pos_mtx);
                // moon names are static game data: seed from disk once
                static bool s_moon_loaded = false;
                if (!s_moon_loaded) {
                    s_moon_loaded = true;
                    json mc = load_json_file(config_dir() / "moon-names.json");
                    if (mc.is_object())
                        for (auto it = mc.begin(); it != mc.end(); ++it)
                            if (it.value().is_string() &&
                                !it.value().get<std::string>().empty())
                                s_moon_name[std::atoll(it.key().c_str())] =
                                    it.value().get<std::string>();
                }
                for (auto& t : towers) {
                    long long m = t.value("moon_id", 0LL);
                    if (m && !s_moon_name.count(m)) {
                        s_moon_name[m] = "";  // claim
                        want_moons.push_back(m);
                    }
                }
                if (now - s_sov_at > 6 * 3600) {
                    s_sov_at = now;
                    need_sov = true;
                }
            }
            if (!want_moons.empty())
                progress(corp_display + ": moon names (" + std::to_string(want_moons.size()) + ")");
            parallel_for_n(want_moons.size(), 6, [&](size_t i) {
                long long m = want_moons[i];
                try {
                    json mj = json::parse(http_get_body(
                        ESI + std::string("/universe/moons/") + std::to_string(m) +
                        "/?datasource=tranquility"));
                    std::lock_guard<std::mutex> pl(s_pos_mtx);
                    s_moon_name[m] = mj.value("name", "");
                } catch (...) {
                    // transient failure: drop our "" claim so the next sweep
                    // retries, instead of caching the empty name for the process
                    // lifetime. This round still falls back to "<system> POS".
                    std::lock_guard<std::mutex> pl(s_pos_mtx);
                    s_moon_name.erase(m);
                }
            });
            if (!want_moons.empty()) {
                json mc;
                std::lock_guard<std::mutex> pl(s_pos_mtx);
                for (auto& kv : s_moon_name)
                    if (!kv.second.empty()) mc[std::to_string(kv.first)] = kv.second;
                save_json_file(config_dir() / "moon-names.json", mc);
            }
            if (need_sov) try {
                json sj = json::parse(http_get_body(
                    ESI + std::string("/sovereignty/map/?datasource=tranquility")));
                std::lock_guard<std::mutex> pl(s_pos_mtx);
                s_sov.clear();
                for (auto& e : sj) {
                    long long a = e.value("alliance_id", 0LL);
                    if (a) s_sov[e.value("system_id", 0LL)] = a;
                }
            } catch (...) {
                // sov discount is best-effort (base rate this round), but a failed
                // fetch must not hold the 6h stamp: reset so the next sweep retries.
                std::lock_guard<std::mutex> pl(s_pos_mtx);
                s_sov_at = 0;
            }
            long long ally = 0;
            bool have_ally = false;
            {
                std::lock_guard<std::mutex> pl(s_pos_mtx);
                auto it = s_corp_ally.find(corp_id);
                if (it != s_corp_ally.end()) { ally = it->second; have_ally = true; }
            }
            if (!have_ally) {
                try {
                    json cj = json::parse(http_get_body(ESI + std::string("/corporations/") +
                                                        std::to_string(corp_id) +
                                                        "/?datasource=tranquility"));
                    ally = cj.value("alliance_id", 0LL);
                } catch (...) {}
                std::lock_guard<std::mutex> pl(s_pos_mtx);
                s_corp_ally[corp_id] = ally;
            }
            // per-tower bay details prefetched in parallel: one call per tower,
            // and the row loop below consumes the results by index
            progress(corp_display + ": tower bays (" + std::to_string(towers.size()) + ")");
            std::vector<json> tower_det(towers.size());
            parallel_for_n(towers.size(), 6, [&](size_t i) {
                long long sid = towers[i].value("starbase_id", 0LL);
                long long sysid = towers[i].value("system_id", 0LL);
                if (!sid) return;
                int dst = 0;
                json det = json::parse(http_get(
                    ESI + std::string("/corporations/") + std::to_string(corp_id) +
                        "/starbases/" + std::to_string(sid) +
                        "/?datasource=tranquility&system_id=" + std::to_string(sysid),
                    tok, dst));
                if (dst == 200) tower_det[i] = std::move(det);
            });
            for (size_t ti = 0; ti < towers.size(); ti++) {
                json& t = towers[ti];
                long long sid = t.value("starbase_id", 0LL);
                long long sysid = t.value("system_id", 0LL);
                long long moonid = t.value("moon_id", 0LL);
                if (!sid) continue;
                std::string tower = names.count(t.value("type_id", 0LL))
                                        ? names[t.value("type_id", 0LL)]
                                        : "Control Tower";
                std::string state = t.value("state", "");
                if (state == "reinforcing") state = "reinforced";  // older spec spelling
                double blocks_hr = 0, stront_hr = 0;
                pos_fuel_rate(tower, blocks_hr, stront_hr);
                bool sov = false;
                {
                    std::lock_guard<std::mutex> pl(s_pos_mtx);
                    sov = ally && s_sov.count(sysid) && s_sov[sysid] == ally;
                }
                if (sov) blocks_hr *= 0.75;
                // the actual bay: fuel blocks + strontium from the tower detail
                double blocks = -1, stront = -1;
                try {
                    json& det = tower_det[ti];
                    if (det.contains("fuels") && det["fuels"].is_array()) {
                        // an empty fuels array is a real answer: the bay is empty
                        blocks = 0;
                        stront = 0;
                        for (auto& f : det["fuels"]) {
                            long long ft = f.value("type_id", 0LL);
                            double q = f.value("quantity", 0.0);
                            if (ft == 16275) stront += q;
                            else if (ft == 4051 || ft == 4246 || ft == 4247 || ft == 4312)
                                blocks += q;
                        }
                    }
                } catch (...) { /* bay stays unknown; the row still renders */ }
                json r;
                r["structure_id"] = sid;
                r["is_pos"] = true;
                r["pos_race"] = pos_race_of(tower);
                r["moon_id"] = moonid;  // tower attack notifications match by moon
                std::string sys = names.count(sysid) ? names[sysid] : "";
                std::string mname;
                {
                    std::lock_guard<std::mutex> pl(s_pos_mtx);
                    if (moonid && s_moon_name.count(moonid)) mname = s_moon_name[moonid];
                }
                r["name"] = !mname.empty() ? mname : sys + " POS";
                r["system"] = sys;
                r["type"] = tower;  // display_type() shortens to "POS" client-side
                r["state"] = state;
                if (t.contains("reinforced_until") && t["reinforced_until"].is_string())
                    r["state_timer_end"] = t["reinforced_until"];
                if (t.contains("unanchor_at") && t["unanchor_at"].is_string())
                    r["unanchors_at"] = t["unanchor_at"];
                r["pos_sov"] = sov;
                double bpd = blocks_hr * 24;
                r["blocks_per_day"] = bpd;
                if (stront >= 0) {
                    r["pos_stront"] = stront;
                    if (stront_hr > 0) r["pos_stront_hours"] = stront / stront_hr;
                }
                if (blocks >= 0) {
                    r["blocks_now"] = blocks;
                    r["m3_now"] = blocks * 5;
                    // offline/unanchoring towers burn nothing: no fuel clock,
                    // the client badges them instead of counting them down
                    if (state == "online" && blocks_hr > 0) {
                        double days = blocks / bpd;
                        time_t fe = now + (time_t)llround(blocks / blocks_hr * 3600.0);
                        std::tm fe_tm{};
#ifdef _WIN32
                        gmtime_s(&fe_tm, &fe);
#else
                        gmtime_r(&fe, &fe_tm);
#endif
                        char fe_iso[32];
                        std::strftime(fe_iso, sizeof fe_iso, "%Y-%m-%dT%H:%M:%SZ", &fe_tm);
                        r["fuel_expires"] = fe_iso;
                        r["fuel_days_left"] = days;
                        double need = std::max(0.0, std::round((30 - days) * bpd));
                        r["blocks_to_30d"] = need;
                        r["m3_to_30d"] = need * 5;
                    }
                }
                out["structures"].push_back(std::move(r));
            }
        }
    }
    out["starbases_status"] = pos_status;

    // persistent refuel log via the history service: report what this poll
    // saw, read back everything the server has accumulated for this corp
    if (!g_history_api.empty()) {
        try {
            json rep = {{"corp_id", corp_id}, {"structures", json::array()}};
            for (auto& r : out["structures"])
                rep["structures"].push_back({{"structure_id", r.value("structure_id", 0LL)},
                                             {"name", r.value("name", "")},
                                             {"system", r.value("system", "")},
                                             {"fuel_expires", r.value("fuel_expires", "")}});
            http_post_json(g_history_api + "/report", rep.dump());
            int hst = 0;
            json hj = json::parse(http_get(g_history_api + "/refuels?corp_id=" +
                                               std::to_string(corp_id), "", hst));
            if (hst == 200 && hj.contains("refuels") && hj["refuels"].is_array())
                out["refuels"] = hj["refuels"];
        } catch (...) { /* history is best-effort; the live table never waits on it */ }
    }

    // corp-kept vs rented-out classification from the moon-rental ticket API.
    // Matched by in-game structure name against the ticket's recorded name;
    // the (C)/(P) naming-convention suffix covers renamed/unticketed ones.
    if (corp_id == g_rentals_corp && !g_rentals_api.empty()) {
        try {
            int rst = 0;
            json rj = json::parse(http_get(g_rentals_api, g_rentals_token, rst));
            if (rst == 200 && rj.contains("rentals") && rj["rentals"].is_array()) {
                std::map<std::string, std::pair<std::string, std::string>> by_name;
                for (auto& t : rj["rentals"])
                    by_name[t.value("station_name", "")] = {t.value("type", ""),
                                                            t.value("renter", "")};
                for (auto& r : out["structures"]) {
                    std::string nm = r.value("name", ""), ty, renter;
                    auto it = by_name.find(nm);
                    if (it != by_name.end()) {
                        ty = it->second.first;
                        renter = it->second.second;
                    } else if (nm.find("(P)") != std::string::npos) {
                        ty = "private";
                    } else if (nm.find("(C)") != std::string::npos) {
                        ty = "corp";
                    }
                    if (!ty.empty()) r["rental"] = ty;
                    if (ty == "private" && !renter.empty()) r["renter"] = renter;
                }
                out["rentals_online"] = true;
            }
        } catch (...) { /* classification is best-effort */ }
    }
    // Metenox monthly economics: moon parsed from the corp's naming convention
    // ("SYSTEM - VI-20"), valued by the moon-rental API (moon-calc composition
    // DB + market-bot prices). Goo - alliance rent - fuel blocks - magmatic = net.
    if (corp_id == g_rentals_corp && !g_econ_api.empty()) {
        json want = json::array();
        for (auto& r : out["structures"]) {
            if (r.value("type", "") != "Metenox Moon Drill") continue;
            int planet = 0, moonno = 0;
            if (!parse_moon_tag(r.value("name", ""), planet, moonno)) continue;
            want.push_back({{"system", r.value("system", "")},
                            {"planet", planet},
                            {"moon", moonno}});
        }
        if (!want.empty()) try {
            json rj = json::parse(http_post_json(
                g_econ_api, json{{"moons", want}}.dump(), g_rentals_token));
            std::map<std::string, json> by_moon;
            if (rj.contains("moons") && rj["moons"].is_array())
                for (auto& e : rj["moons"])
                    if (e.value("found", false)) {
                        // uppercase the key exactly like the lookup below, so the
                        // join survives whatever casing the server echoes back
                        std::string ksys = e.value("system", "");
                        for (auto& c : ksys) c = (char)toupper((unsigned char)c);
                        by_moon[ksys + "|" +
                                std::to_string(e.value("planet", 0)) + "|" +
                                std::to_string(e.value("moon", 0))] = e;
                    }
            for (auto& r : out["structures"]) {
                if (r.value("type", "") != "Metenox Moon Drill") continue;
                int planet = 0, moonno = 0;
                if (!parse_moon_tag(r.value("name", ""), planet, moonno)) continue;
                std::string sys = r.value("system", "");
                for (auto& c : sys) c = (char)toupper((unsigned char)c);
                auto it = by_moon.find(sys + "|" + std::to_string(planet) + "|" +
                                       std::to_string(moonno));
                if (it == by_moon.end()) continue;
                r["econ_goo"] = it->second.value("goo", 0.0);
                r["econ_rent"] = it->second.value("rent", 0.0);
                r["econ_fuel"] = it->second.value("fuel_blocks", 0.0);
                r["econ_gas"] = it->second.value("magmatic", 0.0);
                r["econ_net"] = it->second.value("net", 0.0);
            }
        } catch (...) { /* economics are best-effort; the table never waits */ }
    }
    // the corp's watched skyhooks ride along on the same tab. Watchlist mode
    // is self-contained: the hooks come from config and their theft windows
    // from the public game-wide raidable feed (no auth), matched by planet id.
    // The compatibility date must ride as a query param - the header alone
    // 404s on the new-generation endpoints since ~Aug 2026.
    if (corp_id == g_rentals_corp &&
        (!g_skyhook_watchlist.empty() || g_skyhook_all)) {
        json hooks = json::array();
        std::map<long long, size_t> watch_at;
        for (auto& w : g_skyhook_watchlist)
            if (w.is_object() && w.value("planet_id", 0LL)) {
                watch_at[w.value("planet_id", 0LL)] = hooks.size();
                json h = w;
                h["watch"] = true;
                hooks.push_back(std::move(h));
            }
        try {
            progress("skyhook windows");  // game-wide, no corp label
            int rst = 0;
            json feed = json::parse(
                http_get("https://esi.evetech.net/skyhooks/raidable"
                         "?compatibility_date=2026-06-01",
                         "", rst, nullptr, COMPAT_HDR));
            if (rst == 200 && feed.contains("skyhooks") && feed["skyhooks"].is_array()) {
                // planet -> [name, type_id] cache; planets are static game
                // data so each one is resolved once ever, persisted to disk
                static std::mutex s_sky_mtx;
                static json s_planets;
                {
                    std::lock_guard<std::mutex> sl(s_sky_mtx);
                    static bool loaded = false;
                    if (!loaded) {
                        loaded = true;
                        s_planets = load_json_file(config_dir() / "skyhook-planets.json");
                    }
                    if (!s_planets.is_object()) s_planets = json::object();
                }
                std::vector<long long> resolve;
                std::vector<json> extra;
                for (auto& e : feed["skyhooks"]) {
                    long long pid = e.value("planet_id", 0LL);
                    if (!pid || !e.contains("theft_vulnerability") ||
                        !e["theft_vulnerability"].is_object())
                        continue;
                    std::string ws = e["theft_vulnerability"].value("start", "");
                    std::string we = e["theft_vulnerability"].value("end", "");
                    auto wit = watch_at.find(pid);
                    if (wit != watch_at.end()) {
                        hooks[wit->second]["window_start"] = ws;
                        hooks[wit->second]["window_end"] = we;
                        continue;
                    }
                    if (!g_skyhook_all) continue;
                    json h;
                    h["planet_id"] = pid;
                    h["window_start"] = ws;
                    h["window_end"] = we;
                    {
                        std::lock_guard<std::mutex> sl(s_sky_mtx);
                        if (!s_planets.contains(std::to_string(pid)))
                            resolve.push_back(pid);
                    }
                    extra.push_back(std::move(h));
                }
                if (!resolve.empty()) {
                    progress("skyhook planets (" + std::to_string(resolve.size()) + ")");
                    parallel_for_n(resolve.size(), 6, [&](size_t i) {
                        long long pid = resolve[i];
                        json pj = json::parse(http_get_body(
                            ESI + std::string("/universe/planets/") +
                            std::to_string(pid) + "/?datasource=tranquility"));
                        if (pj.contains("name") && pj["name"].is_string()) {
                            std::lock_guard<std::mutex> sl(s_sky_mtx);
                            s_planets[std::to_string(pid)] = json::array(
                                {pj["name"].get<std::string>(), pj.value("type_id", 0LL)});
                        }
                    });
                    std::lock_guard<std::mutex> sl(s_sky_mtx);
                    save_json_file(config_dir() / "skyhook-planets.json", s_planets);
                }
                for (auto& h : extra) {
                    std::string key = std::to_string(h.value("planet_id", 0LL));
                    std::lock_guard<std::mutex> sl(s_sky_mtx);
                    if (s_planets.contains(key) && s_planets[key].is_array() &&
                        s_planets[key].size() == 2) {
                        // planet name is "SYSTEM ROMAN"; sov-null system names
                        // carry no spaces, so the last token is the planet
                        std::string pn = s_planets[key][0].get<std::string>();
                        size_t sp = pn.rfind(' ');
                        h["system"] = sp == std::string::npos ? pn : pn.substr(0, sp);
                        if (sp != std::string::npos)
                            h["planet_roman"] = pn.substr(sp + 1);
                        long long tid = s_planets[key][1].is_number()
                                            ? s_planets[key][1].get<long long>()
                                            : 0;
                        if (tid == 2015) h["hook_kind"] = "gas";
                        else if (tid == 12) h["hook_kind"] = "ice";
                    } else {
                        h["system"] = "?";
                    }
                    hooks.push_back(std::move(h));
                }
            }
        } catch (...) { /* windows are best-effort; the rows still render */ }
        out["skyhooks"] = std::move(hooks);
    } else if (corp_id == g_rentals_corp && !g_skyhooks_api.empty()) {
        // legacy corp-backend feed; the character's own token goes along as
        // the bearer and the backend decides per-character access, so a
        // non-200 here simply means no skyhook rows.
        try {
            int kst = 0;
            json kj = json::parse(http_get(g_skyhooks_api, tok, kst));
            if (kst == 200 && kj.contains("skyhooks") && kj["skyhooks"].is_array())
                out["skyhooks"] = kj["skyhooks"];
        } catch (...) { /* best-effort */ }
    }

    // owner-level skyhook bays: reagent stocks (secured = reserve hold,
    // unsecured = raidable bay), live theft window and real state, from the
    // compat-dated corp skyhooks endpoint. Needs esi-structures.read_corporation.v1
    // + Station_Manager. Merged into the watch-feed rows by planet id.
    if (out.contains("skyhooks") &&
        token_has_scope(tok, "esi-structures.read_corporation.v1")) {
        try {
            int sst = 0;
            std::string base = std::string("https://esi.evetech.net/corporations/") +
                               std::to_string(corp_id) + "/structures/skyhooks";
            json lst = json::parse(http_get(base, tok, sst, nullptr, COMPAT_HDR));
            if (sst == 200 && lst.contains("skyhooks") && lst["skyhooks"].is_array()) {
                std::map<long long, json> by_planet;
                for (auto& e : lst["skyhooks"]) {
                    long long sk_id = e.value("id", 0LL);
                    long long pid = e.value("planet_id", 0LL);
                    if (!sk_id || !pid) continue;
                    int dst = 0;
                    try {
                        json det = json::parse(http_get(base + "/" + std::to_string(sk_id),
                                                        tok, dst, nullptr, COMPAT_HDR));
                        if (dst == 200) by_planet[pid] = std::move(det);
                    } catch (...) {}
                }
                // CamelCase enum -> the snake states the whole UI already knows
                auto snake_state = [](const std::string& s) -> std::string {
                    if (s == "ShieldVulnerable") return "shield_vulnerable";
                    if (s == "ArmorReinforced") return "armor_reinforce";
                    if (s == "ArmorVulnerable") return "armor_vulnerable";
                    if (s == "HullReinforced") return "hull_reinforce";
                    if (s == "HullVulnerable") return "hull_vulnerable";
                    return "";
                };
                for (auto& row : out["skyhooks"]) {
                    long long pid = row.value("planet_id", 0LL);
                    auto it = by_planet.find(pid);
                    if (it == by_planet.end()) continue;
                    json& det = it->second;
                    double um3 = 0, uisk = 0, sm3 = 0, sisk = 0;
                    if (det.contains("reagents") && det["reagents"].is_array()) {
                        std::lock_guard<std::mutex> vl(s_val_mtx);
                        for (auto& rg : det["reagents"]) {
                            long long tid = rg.value("type_id", 0LL);
                            double un = rg.value("unsecured_stock", 0.0);
                            double se = rg.value("secured_stock", 0.0);
                            double vol = s_type.count(tid) ? s_type[tid].second : 0.01;
                            double prc = s_price.count(tid) ? s_price[tid] : 0.0;
                            um3 += un * vol;
                            uisk += un * prc;
                            sm3 += se * vol;
                            sisk += se * prc;
                        }
                    }
                    row["unsec_m3"] = um3;
                    row["unsec_isk"] = uisk;
                    row["sec_m3"] = sm3;
                    row["sec_isk"] = sisk;
                    if (det.contains("state") && det["state"].is_string())
                        row["state"] = snake_state(det["state"].get<std::string>());
                    if (det.contains("theft_vulnerability") &&
                        det["theft_vulnerability"].is_object()) {
                        row["window_start"] =
                            det["theft_vulnerability"].value("start", "");
                        row["window_end"] = det["theft_vulnerability"].value("end", "");
                    }
                    row["bays_ok"] = true;
                }
            }
        } catch (...) { /* bays are best-effort; the watch feed still renders */ }
    }

    // Rented (alliance-owned) skyhooks expose no bay contents to us, so
    // estimate the raidable surplus from the tracker's unraided STREAK: each
    // consecutive unraided window is one ~72h cycle of gross accrual at the
    // watchlist income rate (streak 0 = just raided = empty). Income figures
    // are thousands of ISK per hour; m3 derives via the magmatic gas market
    // price. Marked "~".
    if (out.contains("skyhooks"))
        for (auto& row : out["skyhooks"]) {
            if (row.value("bays_ok", false)) continue;
            double hourly = row.contains("hourly_isk") && row["hourly_isk"].is_number()
                                ? row["hourly_isk"].get<double>()
                                : -1;
            if (hourly <= 0) continue;
            double streak = row.contains("streak") && row["streak"].is_number()
                                ? row["streak"].get<double>()
                                : -1;
            if (streak < 0) continue;
            double isk = hourly * 1000.0 * 72.0 * streak;
            row["unsec_isk"] = isk;
            {
                std::lock_guard<std::mutex> vl(s_val_mtx);
                auto p = s_price.find(81143LL);  // Magmatic Gas
                if (p != s_price.end() && p->second > 0)
                    row["unsec_m3"] = isk / p->second * 0.01;
            }
            row["est"] = true;
        }

    // structure + moonmining notifications: a much faster signal than the
    // hourly corp structures dataset (attacks land within ~10 min, plus
    // anchoring/destruction events the list itself won't show until the next
    // roll, and the actual chunk-fracture moment: MoonminingLaserFired names
    // who pressed the button, MoonminingAutomaticFracture is the 3h timeout)
    if (char_id && token_has_scope(tok, "esi-characters.read_notifications.v1")) {
        progress(corp_display + ": notifications");
        int nst = 0;
        std::string nb = http_get(ESI + std::string("/characters/") +
                                      std::to_string(char_id) +
                                      "/notifications/?datasource=tranquility",
                                  tok, nst);
        if (nst == 200) try {
            json nj = json::parse(nb);
            json outn = json::array();
            std::vector<long long> fired_ids;
            for (auto& n : nj) {
                std::string ty = n.value("type", "");
                bool moon = ty.rfind("Moonmining", 0) == 0;
                // TowerAlertMe etc: POS attacks carry a moonID, not a structureID
                bool pos = ty.rfind("Tower", 0) == 0;
                if (!moon && !pos && ty.rfind("Structure", 0) != 0) continue;
                std::string text = n.value("text", "");
                json e = {{"type", ty},
                          {"timestamp", n.value("timestamp", "")},
                          {"structure_id", yaml_ll(text, "structureID")},
                          // moonmining/tower YAML capitalizes the S the structure ones don't
                          {"system_id", yaml_ll(text, (moon || pos) ? "solarSystemID"
                                                                    : "solarsystemID")}};
                if (pos) e["moon_id"] = yaml_ll(text, "moonID");
                if (ty == "MoonminingLaserFired") {
                    long long fb = yaml_ll(text, "firedBy");
                    if (fb) {
                        e["fired_by"] = fb;
                        fired_ids.push_back(fb);
                    }
                }
                outn.push_back(std::move(e));
            }
            // who fired the laser, by name (public lookup, best-effort)
            if (!fired_ids.empty()) try {
                std::sort(fired_ids.begin(), fired_ids.end());
                fired_ids.erase(std::unique(fired_ids.begin(), fired_ids.end()),
                                fired_ids.end());
                std::string body = "[";
                for (size_t i = 0; i < fired_ids.size(); i++)
                    body += (i ? "," : "") + std::to_string(fired_ids[i]);
                body += "]";
                json r = json::parse(http_post_json(
                    ESI + std::string("/universe/names/?datasource=tranquility"), body));
                std::map<long long, std::string> who;
                for (auto& e : r) who[e.value("id", 0LL)] = e.value("name", "");
                for (auto& e : outn)
                    if (e.contains("fired_by") && who.count(e["fired_by"].get<long long>()))
                        e["fired_by_name"] = who[e["fired_by"].get<long long>()];
            } catch (...) {}
            out["notifications"] = outn;
            out["notifications_ok"] = true;
        } catch (...) {}
    }
    return out.dump();
} catch (...) {
    err = "unexpected error during the ESI pull";
    return "";  // surfaced as the status line's "no data"; never crash the app
}

// --- multi-corp fetch -----------------------------------------------------------
// One tab per corp the stored characters can actually read. Two characters in
// the same corp share a tab; a character whose corp pull fails contributes no
// tab (their error only surfaces if NO corp works). Labels are corp tickers.
struct Snap {
    std::string label;  // tab label (corp ticker, name fallback)
    std::string data;   // /bpos-shaped JSON
};

static std::vector<Snap> fetch_snapshots(const std::string& client_id) {
    std::vector<Snap> out;
    json chars = load_characters();
    std::vector<long long> seen;
    std::string first_err;
    struct Job {
        std::string tok, label, corp_name;
        long long corp_id = 0, char_id = 0;
    };
    std::vector<Job> jobs;
    for (size_t i = 0; i < chars.size(); i++) {
        std::string tok = ensure_token_entry(client_id, chars, i);
        std::string who = chars[i].value("character_name", "?");
        if (tok.empty()) {
            if (first_err.empty()) first_err = who + ": login expired (stoker --add to re-auth)";
            continue;
        }
        long long char_id = chars[i].value("character_id", 0LL);
        int st = 0;
        json me;
        try {
            me = json::parse(http_get(ESI + std::string("/characters/") +
                                          std::to_string(char_id) + "/", "", st));
        } catch (...) {
            if (first_err.empty())
                first_err = who + ": cannot reach ESI (is curl installed and the network up?)";
            continue;
        }
        long long corp_id = me.value("corporation_id", 0LL);
        if (!corp_id) {
            if (first_err.empty())
                first_err = who + ": ESI lookup failed (HTTP " + std::to_string(st) + ")";
            continue;
        }
        if (std::find(seen.begin(), seen.end(), corp_id) != seen.end())
            continue;
        seen.push_back(corp_id);

        std::string label = std::to_string(corp_id), corp_name;
        try {
            json corp = json::parse(http_get(ESI + std::string("/corporations/") +
                                                 std::to_string(corp_id) + "/", "", st));
            std::string tick = corp.value("ticker", ""), nm = corp.value("name", "");
            label = !tick.empty() ? tick : (!nm.empty() ? nm : label);
            corp_name = nm;
        } catch (...) {}
        jobs.push_back({tok, label, corp_name, corp_id, char_id});
    }

    // the heavy per-corp sweeps run in parallel: with two or more corps the
    // boot takes as long as the slowest one instead of the sum
    std::vector<Snap> results(jobs.size());
    std::vector<std::string> errs(jobs.size());
    std::vector<std::thread> ths;
    for (size_t j = 0; j < jobs.size(); j++)
        ths.emplace_back([&, j]() {
            std::string err;
            std::string data =
                fetch_corp(jobs[j].tok, jobs[j].corp_id,
                           jobs[j].corp_name.empty() ? jobs[j].label : jobs[j].corp_name,
                           jobs[j].char_id, err);
            results[j] = {jobs[j].label, std::move(data)};
            errs[j] = err;
        });
    for (auto& t : ths) t.join();
    for (size_t j = 0; j < jobs.size(); j++) {
        if (results[j].data.empty()) {
            if (first_err.empty() && !errs[j].empty())
                first_err = jobs[j].label + ": " + errs[j];
            continue;
        }
        out.push_back(std::move(results[j]));
    }
    if (out.empty()) {
        std::string msg = first_err.empty()
                              ? "no usable character logins (run stoker --add)"
                              : first_err;
        json e = {{"structures", json::array()}, {"error", msg}};
        out.push_back({"", e.dump()});
    }
    return out;
}

}  // namespace standalone
