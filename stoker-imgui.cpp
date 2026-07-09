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
#include "window_icon_data.hpp"

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

// --- gl textures for the pixel-art icons --------------------------------------
static ImTextureID make_icon(const unsigned char* rgba) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kIconSize, kIconSize, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return (ImTextureID)(intptr_t)tex;
}
static ImTextureID g_ic_fuel, g_ic_gas, g_ic_ozone;

// --- the gauge widget: fill fraction + text inside -----------------------------
static void gauge(const char* id, double frac, int band, const std::string& left,
                  const std::string& right, float w, bool secondary = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight() + 4;
    ImU32 col = band_u32(band, secondary);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(30, 32, 44, 255), 3.0f);
    if (frac >= 0) {
        float f = frac > 1 ? 1.f : (float)frac;
        dl->AddRectFilled(p, ImVec2(p.x + w * f, p.y + h), col, 3.0f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), (col & 0xFFFFFF) | 0x60000000, 3.0f);
    }
    auto put = [&](const std::string& s, bool rightside) {
        if (s.empty()) return;
        ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        float x = rightside ? p.x + w - ts.x - 5 : p.x + 5;
        // readable over both fill and trough: dark text on the fill, colored past it
        float fillx = frac >= 0 ? p.x + w * (frac > 1 ? 1.f : (float)frac) : p.x;
        ImU32 tc = (x + ts.x * 0.5f) < fillx ? IM_COL32(12, 12, 18, 255) : col;
        dl->AddText(ImVec2(x, p.y + 2), tc, s.c_str());
    };
    if (frac < 0) {  // no gauge: centered label
        ImVec2 ts = ImGui::CalcTextSize(left.c_str());
        dl->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + 2), col, left.c_str());
    } else {
        put(left, false);
        put(right, true);
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
    if (!g_standalone || g_busy) return;
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
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    g_ic_fuel = make_icon(kIcon_fuel);
    g_ic_gas = make_icon(kIcon_gas);
    g_ic_ozone = make_icon(kIcon_ozone);

    std::thread th(worker);
    if (g_standalone && !standalone::have_login()) act_add_character();

    char filter[128] = {0};
    long long detail_sid = 0;
    int view_tab = 0;  // 0 detail, 1 refuel log

    while (!glfwWindowShouldClose(win) && g_run) {
        glfwWaitEventsTimeout(0.25);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        std::vector<Row> rows;
        std::vector<Refuel> refuels;
        std::vector<std::string> tabs;
        std::string status, note, corpname, upd, updurl, pulled, esiMod;
        int tabsel;
        {
            std::lock_guard<std::mutex> l(g_mtx);
            rows = g_rows;
            refuels = g_refuels;
            tabs = g_tab_labels;
            tabsel = g_tab;
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

        // filter
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##filter", "filter name/system/type", filter, sizeof filter);
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
        if (ImGui::BeginTable("structs", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Blocks / 30d", ImGuiTableColumnFlags_WidthFixed, 170);
            ImGui::TableSetupColumn("Gas-Oz / 30d", ImGuiTableColumnFlags_WidthFixed, 170);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
                if (sp->SpecsCount) {
                    int ci = sp->Specs[0].ColumnIndex;
                    bool asc = sp->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                    std::stable_sort(view.begin(), view.end(),
                                     [&](const Row* a, const Row* b) {
                        auto key = [&](const Row* r) -> std::string {
                            switch (ci) {
                                case 2: return r->type;
                                case 3: return r->system;
                                default: return r->name;
                            }
                        };
                        if (ci == 0) {
                            double da = a->has_fuel ? a->days : 1e9,
                                   db = b->has_fuel ? b->days : 1e9;
                            return asc ? da < db : da > db;
                        }
                        if (ci == 1) {
                            double da = fuel2_days(*a), db = fuel2_days(*b);
                            if (da < 0) da = 1e9;
                            if (db < 0) db = 1e9;
                            return asc ? da < db : da > db;
                        }
                        return asc ? key(a) < key(b) : key(a) > key(b);
                    });
                }
            }

            for (auto* rp : view) {
                const Row& r = *rp;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char db[32];
                std::snprintf(db, sizeof db, "%.1fd", r.days);
                if (!r.has_fuel)
                    gauge("f", -1, -1, "--", "", 160);
                else
                    gauge("f", r.days / GAUGE_DAYS, urgency_band(r.days), db,
                          units_raw(r), 160);
                ImGui::TableSetColumnIndex(1);
                if (r.fuel2_name.empty())
                    gauge("g", -1, -1, "-", "", 160, true);
                else if (r.fuel2 < 0)
                    gauge("g", -1, -1, "?", "", 160, true);
                else if (r.gas_day > 0) {
                    char gb[32];
                    std::snprintf(gb, sizeof gb, "%.1fd", fuel2_days(r));
                    gauge("g", fuel2_days(r) / GAUGE_DAYS, fuel2_band(r), gb, gas30_raw(r), 160, true);
                } else {
                    gauge("g", r.lo_target > 0 ? r.fuel2 / r.lo_target : -1, fuel2_band(r),
                          compact_units(r.fuel2), gas30_raw(r), 160, true);
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(r.type.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(CYAN_, "%s", r.system.c_str());
                ImGui::TableSetColumnIndex(4);
                if (ImGui::Selectable((r.name + "##" + std::to_string(r.sid)).c_str(),
                                      r.sid == detail_sid,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    detail_sid = r.sid;
                    view_tab = 0;
                }
            }
            ImGui::EndTable();
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
                if (!d) {
                    ImGui::TextColored(GREY_, "select a structure");
                } else {
                    ImGui::Image(g_ic_fuel, ImVec2(24, 24));
                    ImGui::SameLine();
                    ImGui::TextColored(CYAN_, "%s", d->name.c_str());
                    ImGui::TextColored(GREY_, "%s   %s   %s", d->system.c_str(),
                                       d->type.c_str(), d->state.c_str());
                    ImGui::Separator();
                    char b[64];
                    std::snprintf(b, sizeof b, "%.1f days", d->days);
                    if (d->has_fuel)
                        gauge("df", d->days / GAUGE_DAYS, urgency_band(d->days), b,
                              units_raw(*d) + " blocks to 30d",
                              ImGui::GetContentRegionAvail().x - 4);
                    if (!d->fuel2_name.empty()) {
                        ImGui::Image(d->gas_day > 0 ? g_ic_gas : g_ic_ozone, ImVec2(20, 20));
                        ImGui::SameLine();
                        if (d->fuel2 < 0) {
                            std::string f2s;
                            {
                                std::lock_guard<std::mutex> l(g_mtx);
                                f2s = g_fuel2_status;
                            }
                            ImGui::TextColored(GREY_, "%s unknown - %s", d->fuel2_name.c_str(),
                                               f2s == "relogin" ? "re-login to grant corp-assets (+ add character)"
                                               : f2s == "director" ? "needs the in-game Director role"
                                                                   : "no Director-role data source");
                        } else {
                            ImGui::Text("%s %s", commas(d->fuel2).c_str(), d->fuel2_name.c_str());
                            if (double dd = fuel2_days(*d); dd >= 0) {
                                ImGui::SameLine();
                                ImGui::TextColored(GREY_, " (~%sd at drill rate, %s to 30d)",
                                                   commas(dd).c_str(), gas30_raw(*d).c_str());
                            }
                        }
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
