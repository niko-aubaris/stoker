// GUI host: run the exact FTXUI component in a native window instead of a
// terminal. The component renders into an off-screen ftxui::Screen, whose
// styled output is parsed into a character grid and drawn with OpenGL 1.1
// (immediate mode: no loader, works everywhere GLFW does) using an embedded
// DejaVu Sans Mono atlas (stb_truetype). Keyboard input is translated back
// into ftxui Events, so every key works exactly like the terminal build.
//
// Compiled only into the stoker-gui target (STOKER_GUI).
#pragma once

#include <GLFW/glfw3.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "font_dejavu_mono.hpp"

#include "ftxui/dom/node.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/terminal.hpp"

namespace gui {

static const float FONT_PX = 17.0f;
static const int PAD = 6;  // window-edge padding in px

// --- glyph atlas ---------------------------------------------------------------
// ASCII plus every non-ASCII glyph the app draws (borders, bars, arrows, F²).
static const unsigned int EXTRA_CP[] = {
    0x2500, 0x2502, 0x256D, 0x256E, 0x2570, 0x256F,  // ─ │ ╭ ╮ ╰ ╯
    0x251C, 0x2524, 0x2588, 0x2591, 0x258C,          // ├ ┤ █ ░ ▌
    0x00BB, 0x00B2, 0x2192, 0x2190,                  // » ² → ←
};
static const int N_EXTRA = int(sizeof(EXTRA_CP) / sizeof(EXTRA_CP[0]));

struct Glyph {
    float u0, v0, u1, v1;    // atlas rect
    float xoff, yoff, xadv;  // placement
    int w, h;
};

struct Atlas {
    GLuint tex = 0;
    float cell_w = 0, cell_h = 0, baseline = 0;
    std::map<unsigned int, Glyph> glyphs;
} g_atlas;

static void build_atlas() {
    stbtt_fontinfo font;
    stbtt_InitFont(&font, kFontData, stbtt_GetFontOffsetForIndex(kFontData, 0));
    float scale = stbtt_ScaleForPixelHeight(&font, FONT_PX);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
    g_atlas.cell_h = std::ceil((asc - desc + gap) * scale);
    g_atlas.baseline = asc * scale;
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font, 'M', &advance, &lsb);
    g_atlas.cell_w = std::ceil(advance * scale);

    const int TEX = 512;
    std::vector<unsigned char> pix(TEX * TEX, 0);
    int x = 1, y = 1, rowh = 0;
    auto bake = [&](unsigned int cp) {
        int w, h, xo, yo;
        unsigned char* bmp = stbtt_GetCodepointBitmap(&font, scale, scale, (int)cp, &w, &h, &xo, &yo);
        if (x + w + 1 >= TEX) { x = 1; y += rowh + 1; rowh = 0; }
        if (y + h + 1 >= TEX) { if (bmp) stbtt_FreeBitmap(bmp, nullptr); return; }
        for (int r = 0; r < h; r++)
            std::memcpy(&pix[(y + r) * TEX + x], bmp + r * w, w);
        Glyph g;
        g.u0 = float(x) / TEX; g.v0 = float(y) / TEX;
        g.u1 = float(x + w) / TEX; g.v1 = float(y + h) / TEX;
        g.xoff = (float)xo; g.yoff = (float)yo; g.w = w; g.h = h;
        int adv2, lsb2;
        stbtt_GetCodepointHMetrics(&font, (int)cp, &adv2, &lsb2);
        g.xadv = adv2 * scale;
        g_atlas.glyphs[cp] = g;
        x += w + 1;
        rowh = std::max(rowh, h);
        if (bmp) stbtt_FreeBitmap(bmp, nullptr);
    };
    for (unsigned int cp = 32; cp < 127; cp++) bake(cp);
    for (int i = 0; i < N_EXTRA; i++) bake(EXTRA_CP[i]);

    glGenTextures(1, &g_atlas.tex);
    glBindTexture(GL_TEXTURE_2D, g_atlas.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, TEX, TEX, 0, GL_ALPHA, GL_UNSIGNED_BYTE, pix.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

// --- styled-text grid ------------------------------------------------------------
static unsigned char bg_default_r() { return 15; }
static unsigned char bg_default_g() { return 15; }
static unsigned char bg_default_b() { return 23; }

struct Cell {
    unsigned int cp = ' ';
    unsigned char fr = 230, fg_ = 230, fb = 230;
    unsigned char br = 15, bg_ = 15, bb = 23;
    bool bold = false, dim = false, under = false;
};

// Parse the ANSI-styled frame FTXUI prints into a cols x rows cell grid.
// Only the SGR subset FTXUI emits for this app is handled; unknown codes
// are ignored.
static void parse_frame(const std::string& s, int cols, int rows, std::vector<Cell>& grid) {
    grid.assign((size_t)cols * rows, Cell{});
    int cx = 0, cy = 0;
    unsigned char fr = 230, fg = 230, fb = 230, br = 15, bg = 15, bb = 23;
    bool bold = false, dim = false, inv = false, under = false;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        if (c == 0x1B) {
            size_t m = s.find('m', i);
            size_t bad = s.find_first_not_of("[;0123456789", i + 1);
            if (m == std::string::npos || bad < m) { i++; continue; }  // non-SGR escape
            std::vector<int> code;
            int cur = -1;
            for (size_t k = i + 2; k < m; k++) {
                char ch = s[k];
                if (ch >= '0' && ch <= '9') cur = (cur < 0 ? 0 : cur * 10) + (ch - '0');
                else { code.push_back(cur < 0 ? 0 : cur); cur = -1; }
            }
            code.push_back(cur < 0 ? 0 : cur);
            for (size_t k = 0; k < code.size(); k++) {
                switch (code[k]) {
                    case 0: fr = fg = fb = 230; br = bg_default_r(); bg = bg_default_g(); bb = bg_default_b();
                            bold = dim = inv = under = false; break;
                    case 1: bold = true; break;
                    case 2: dim = true; break;
                    case 4: under = true; break;
                    case 7: inv = true; break;
                    case 22: bold = dim = false; break;
                    case 24: under = false; break;
                    case 27: inv = false; break;
                    case 39: fr = fg = fb = 230; break;
                    case 49: br = bg_default_r(); bg = bg_default_g(); bb = bg_default_b(); break;
                    case 38:
                    case 48:
                        if (k + 4 < code.size() && code[k + 1] == 2) {
                            unsigned char r = (unsigned char)code[k + 2],
                                          g2 = (unsigned char)code[k + 3],
                                          b = (unsigned char)code[k + 4];
                            if (code[k] == 38) { fr = r; fg = g2; fb = b; }
                            else { br = r; bg = g2; bb = b; }
                            k += 4;
                        }
                        break;
                    default: break;
                }
            }
            i = m + 1;
            continue;
        }
        if (c == '\r') { cx = 0; i++; continue; }
        if (c == '\n') { cx = 0; cy++; i++; continue; }
        // utf-8 decode
        unsigned int cp = c;
        int len = 1;
        if (c >= 0xF0) { len = 4; cp = c & 0x07; }
        else if (c >= 0xE0) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xC0) { len = 2; cp = c & 0x1F; }
        for (int k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        i += len;
        if (cy >= rows || cx >= cols) continue;
        Cell& cell = grid[(size_t)cy * cols + cx];
        cell.cp = cp;
        cell.bold = bold; cell.dim = dim; cell.under = under;
        if (inv) { cell.fr = br; cell.fg_ = bg; cell.fb = bb; cell.br = fr; cell.bg_ = fg; cell.bb = fb; }
        else { cell.fr = fr; cell.fg_ = fg; cell.fb = fb; cell.br = br; cell.bg_ = bg; cell.bb = bb; }
        cx++;
    }
}

// --- input translation -----------------------------------------------------------
static ftxui::Component* g_comp = nullptr;
static bool g_swallow_next_char = false;  // alt+letter also fires the char callback

static void post(const ftxui::Event& e) {
    if (g_comp && *g_comp) (*g_comp)->OnEvent(e);
}

static void char_cb(GLFWwindow*, unsigned int cp) {
    if (g_swallow_next_char) { g_swallow_next_char = false; return; }
    std::string u;
    if (cp < 0x80) u += (char)cp;
    else if (cp < 0x800) { u += (char)(0xC0 | (cp >> 6)); u += (char)(0x80 | (cp & 0x3F)); }
    else { u += (char)(0xE0 | (cp >> 12)); u += (char)(0x80 | ((cp >> 6) & 0x3F)); u += (char)(0x80 | (cp & 0x3F)); }
    post(ftxui::Event::Character(u));
}

static void key_cb(GLFWwindow*, int key, int, int action, int mods) {
    using ftxui::Event;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    if ((mods & GLFW_MOD_ALT) && key == GLFW_KEY_C) {
        g_swallow_next_char = true;  // the 'c' char event that may follow
        post(Event::Special("\x1b" "c"));
        return;
    }
    switch (key) {
        case GLFW_KEY_ESCAPE: post(Event::Escape); break;
        case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: post(Event::Return); break;
        case GLFW_KEY_BACKSPACE: post(Event::Backspace); break;
        case GLFW_KEY_TAB: post(Event::Tab); break;
        case GLFW_KEY_UP: post(Event::ArrowUp); break;
        case GLFW_KEY_DOWN: post(Event::ArrowDown); break;
        case GLFW_KEY_LEFT: post(Event::ArrowLeft); break;
        case GLFW_KEY_RIGHT: post(Event::ArrowRight); break;
        case GLFW_KEY_PAGE_UP: post(Event::PageUp); break;
        case GLFW_KEY_PAGE_DOWN: post(Event::PageDown); break;
        case GLFW_KEY_HOME: post(Event::Home); break;
        case GLFW_KEY_END: post(Event::End); break;
        default: break;
    }
}

static void scroll_cb(GLFWwindow*, double, double dy) {
    ftxui::Mouse m;
    m.button = dy > 0 ? ftxui::Mouse::WheelUp : ftxui::Mouse::WheelDown;
    m.motion = ftxui::Mouse::Pressed;
    m.x = 1; m.y = 1;
    post(ftxui::Event::Mouse("", m));
}

// --- the loop ---------------------------------------------------------------------
// Runs until quit; returns when the app exits. `running` is the app's g_run.
static int run(ftxui::Component& component, std::atomic<bool>& running,
               GLFWwindow** window_out) {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    GLFWwindow* win = glfwCreateWindow(1180, 760, "STOKER", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    *window_out = win;
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    build_atlas();

    g_comp = &component;
    glfwSetCharCallback(win, char_cb);
    glfwSetKeyCallback(win, key_cb);
    glfwSetScrollCallback(win, scroll_cb);

    // FTXUI would otherwise size frames for a real terminal (or its 80x25
    // fallback) and downgrade the RGB colors when stdout is not a tty.
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);

    std::vector<Cell> grid;
    while (running && !glfwWindowShouldClose(win)) {
        glfwWaitEventsTimeout(0.25);
        int fbw, fbh;
        glfwGetFramebufferSize(win, &fbw, &fbh);
        int cols = std::max(40, (int)((fbw - 2 * PAD) / g_atlas.cell_w));
        int rows = std::max(10, (int)((fbh - 2 * PAD) / g_atlas.cell_h));
        ftxui::Terminal::SetFallbackSize({cols, rows});

        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(cols),
                                            ftxui::Dimension::Fixed(rows));
        ftxui::Render(screen, component->Render());
        parse_frame(screen.ToString(), cols, rows, grid);

        glViewport(0, 0, fbw, fbh);
        glClearColor(bg_default_r() / 255.f, bg_default_g() / 255.f, bg_default_b() / 255.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, fbw, fbh, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        float cw = g_atlas.cell_w, ch = g_atlas.cell_h;
        // background quads
        glDisable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                const Cell& c = grid[(size_t)y * cols + x];
                if (c.br == bg_default_r() && c.bg_ == bg_default_g() && c.bb == bg_default_b())
                    continue;
                float px = PAD + x * cw, py = PAD + y * ch;
                glColor3ub(c.br, c.bg_, c.bb);
                glVertex2f(px, py); glVertex2f(px + cw, py);
                glVertex2f(px + cw, py + ch); glVertex2f(px, py + ch);
            }
        glEnd();
        // glyphs (second pass 1px right for bold)
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_atlas.tex);
        glBegin(GL_QUADS);
        for (int pass = 0; pass < 2; pass++)
            for (int y = 0; y < rows; y++)
                for (int x = 0; x < cols; x++) {
                    const Cell& c = grid[(size_t)y * cols + x];
                    if (c.cp == ' ' || (pass == 1 && !c.bold)) continue;
                    auto it = g_atlas.glyphs.find(c.cp);
                    if (it == g_atlas.glyphs.end()) it = g_atlas.glyphs.find('?');
                    const Glyph& g = it->second;
                    float px = PAD + x * cw + g.xoff + pass, py = PAD + y * ch + g_atlas.baseline + g.yoff;
                    unsigned char a = c.dim ? 140 : 255;
                    glColor4ub(c.fr, c.fg_, c.fb, a);
                    glTexCoord2f(g.u0, g.v0); glVertex2f(px, py);
                    glTexCoord2f(g.u1, g.v0); glVertex2f(px + g.w, py);
                    glTexCoord2f(g.u1, g.v1); glVertex2f(px + g.w, py + g.h);
                    glTexCoord2f(g.u0, g.v1); glVertex2f(px, py + g.h);
                }
        glEnd();
        // underlines
        glDisable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                const Cell& c = grid[(size_t)y * cols + x];
                if (!c.under) continue;
                float px = PAD + x * cw, py = PAD + (y + 1) * ch - 2;
                glColor3ub(c.fr, c.fg_, c.fb);
                glVertex2f(px, py); glVertex2f(px + cw, py);
                glVertex2f(px + cw, py + 1); glVertex2f(px, py + 1);
            }
        glEnd();

        glfwSwapBuffers(win);
    }
    running = false;
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}

}  // namespace gui
