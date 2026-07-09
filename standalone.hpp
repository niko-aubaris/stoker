// Standalone mode: EVE SSO (PKCE, no secret) + direct ESI pull, for people
// outside the corp endpoint. Produces the same JSON shape as the /bpos
// endpoint so ingest() works unchanged; server-side extras (burn rates,
// refuel log, claims, bay estimates) simply come back unknown.
//
// Requires an EVE developer application (https://developers.eveonline.com):
//   callback URL  http://localhost:8420/callback
//   scope         esi-corporations.read_structures.v1
// The in-game character needs the Station_Manager role (or Director).
//
// Include AFTER run_cmd/urlenc/parse_iso in bpos-dash.cpp (single-TU project).
#pragma once

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
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

static const int CALLBACK_PORT = 8420;
static const char* SSO_TOKEN_URL = "https://login.eveonline.com/v2/oauth/token";
static const char* ESI = "https://esi.evetech.net/latest";
static const char* SCOPE = "esi-corporations.read_structures.v1";
// Requested at login. Overridable via config.json "scopes" so a copy whose
// dev app allows more (e.g. + esi-assets.read_corporation_assets.v1 for the
// 2ND FUEL column) can ask for it without changing everyone's default.
static std::string g_scopes = SCOPE;

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
static std::string http_post_form(const std::string& url, const std::string& body) {
    std::string cmd = "curl -s --max-time 20 -X POST "
                      "-H \"Content-Type: application/x-www-form-urlencoded\" "
                      "-d \"" + body + "\" \"" + url + "\"" QUIET;
    return run_cmd(cmd.c_str());
}

static std::string http_post_json(const std::string& url, const std::string& body) {
    std::string cmd = "curl -s --compressed --max-time 20 -X POST "
                      "-H \"Content-Type: application/json\" "
                      "-d \"" + body + "\" \"" + url + "\"" QUIET;
    return run_cmd(cmd.c_str());
}

// GET with response headers; returns body, fills status + wanted headers.
static std::string http_get(const std::string& url, const std::string& bearer,
                            int& status, json* headers_out = nullptr) {
    std::string cmd = "curl -s -i --compressed --max-time 30 ";
    if (!bearer.empty()) cmd += "-H \"Authorization: Bearer " + bearer + "\" ";
    cmd += "\"" + url + "\"" QUIET;
    std::string raw = run_cmd(cmd.c_str());
    status = 0;
    size_t sep = raw.find("\r\n\r\n");
    // skip informational/continuation header blocks (e.g. HTTP/1.1 100)
    while (sep != std::string::npos && raw.find("HTTP/", sep + 4) == sep + 4)
        sep = raw.find("\r\n\r\n", sep + 4);
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
    return raw.substr(sep + 4);
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

// Blocks until the browser hits /callback; returns the auth code ("" on error).
// Gives up after timeout_s so an abandoned login can't wedge the caller (and
// can't hold the port against the next attempt).
static std::string wait_for_callback(const std::string& expect_state, std::string& err,
                                     int timeout_s = 180) {
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
    for (int tries = 0; tries < 32 && code.empty(); tries++) {
        // wait for a connection, but never past the deadline
        long left = (long)(deadline - time(nullptr));
        if (left <= 0) { err = "login timed out (nothing came back from the browser)"; break; }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv, &rd);
        timeval tv{};
        tv.tv_sec = left;
        int sr = select((int)srv + 1, &rd, nullptr, nullptr, &tv);
        if (sr <= 0) { err = "login timed out (nothing came back from the browser)"; break; }
        sock_t c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
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

    std::string code = wait_for_callback(state, err);
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
            err = "token exchange failed: no reply from login.eveonline.com";
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
static std::string fetch_corp(const std::string& tok, long long corp_id,
                              std::string& err) try {
    int st = 0;
    json structures = json::array(), hdr = json::object();
    for (int page = 1, pages = 1; page <= pages && page <= 20; page++) {
        json* h = page == 1 ? &hdr : nullptr;
        std::string body = http_get(ESI + std::string("/corporations/") +
                                        std::to_string(corp_id) +
                                        "/structures/?datasource=tranquility&page=" +
                                        std::to_string(page),
                                    tok, st, h);
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
        if (page == 1 && hdr.value("x-pages", std::string()) != "")
            pages = std::atoi(hdr["x-pages"].get<std::string>().c_str());
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
    bool assets_ok = false;
    if (g_scopes.find("read_corporation_assets") != std::string::npos) {
        json ahdr = json::object();
        for (int page = 1, pages = 1; page <= pages && page <= 40; page++) {
            json* h = page == 1 ? &ahdr : nullptr;
            std::string body = http_get(ESI + std::string("/corporations/") +
                                            std::to_string(corp_id) +
                                            "/assets/?datasource=tranquility&page=" +
                                            std::to_string(page),
                                        tok, st, h);
            if (st != 200) break;
            json j;
            try { j = json::parse(body); } catch (...) { break; }
            if (!j.is_array()) break;
            assets_ok = true;
            for (auto& it : j) {
                if (it.value("location_flag", "") != "StructureFuel") continue;
                long long loc = it.value("location_id", 0LL);
                long long tid = it.value("type_id", 0LL);
                double q = it.value("quantity", 0.0);
                if (tid == 81143) gas_at[loc] += q;        // Magmatic Gas
                else if (tid == 16273) ozone_at[loc] += q;  // Liquid Ozone
            }
            if (page == 1 && ahdr.value("x-pages", std::string()) != "")
                pages = std::atoi(ahdr["x-pages"].get<std::string>().c_str());
        }
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
        std::string svc;
        if (s.contains("services") && s["services"].is_array())
            for (auto& v : s["services"]) {
                if (v.value("state", "") != "online") continue;
                if (!svc.empty()) svc += ", ";
                svc += v.value("name", "");
            }
        r["services"] = svc;
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
            if (ty == "Metenox Moon Drill")
                r["fuel2_units"] = gas_at.count(sid) ? gas_at[sid] : 0.0;
            else if (ty.rfind("Ansiblex", 0) == 0 || ty.rfind("Pharolux", 0) == 0)
                r["fuel2_units"] = ozone_at.count(sid) ? ozone_at[sid] : 0.0;
        }
        out["structures"].push_back(std::move(r));
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
        } catch (...) { continue; }
        long long corp_id = me.value("corporation_id", 0LL);
        if (!corp_id || std::find(seen.begin(), seen.end(), corp_id) != seen.end())
            continue;
        seen.push_back(corp_id);

        std::string label = std::to_string(corp_id);
        try {
            json corp = json::parse(http_get(ESI + std::string("/corporations/") +
                                                 std::to_string(corp_id) + "/", "", st));
            std::string tick = corp.value("ticker", ""), nm = corp.value("name", "");
            label = !tick.empty() ? tick : (!nm.empty() ? nm : label);
        } catch (...) {}

        std::string err;
        std::string data = fetch_corp(tok, corp_id, err);
        if (data.empty()) {
            if (first_err.empty() && !err.empty()) first_err = who + ": " + err;
            continue;
        }
        out.push_back({label, std::move(data)});
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
