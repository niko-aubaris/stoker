// STOKER GUI, Rust edition: native-window fuel-watch dashboard on egui
// (pure-Rust immediate mode, the spiritual sibling of the C++ build's Dear
// ImGui). Corp mode only, same data core as the TUI (stoker::net/data).
//
// Layout mirrors stoker-imgui.cpp: structure-portrait rows with a stacked
// dual meter (fuel top, gas/ozone bottom), state/power/timer badges under
// the name, a detail side panel with the refuel log and one-click claims.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui::{self, Align2, Color32, FontId, Pos2, Rect, Rounding, Sense, Stroke, Vec2};
use std::collections::HashMap;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use stoker::data::*;
use stoker::net::{self, App};
use stoker::util::*;

// Hot Neon palette
const NEON_PINK: Color32 = Color32::from_rgb(255, 43, 214);
const NEON_CYAN: Color32 = Color32::from_rgb(0, 229, 255);
const NEON_DIM_CYAN: Color32 = Color32::from_rgb(0, 130, 150);
const INK_GRAY: Color32 = Color32::from_rgb(110, 118, 138);
const GREY: Color32 = Color32::from_rgb(128, 136, 150);
const LIGHT: Color32 = Color32::from_rgb(198, 206, 222);
const GREEN: Color32 = Color32::from_rgb(90, 225, 130);
const YELLOW: Color32 = Color32::from_rgb(250, 215, 70);
const RED: Color32 = Color32::from_rgb(255, 70, 70);
const ORANGE: Color32 = Color32::from_rgb(255, 140, 0);
const BG: Color32 = Color32::from_rgb(10, 10, 16);
const BG_PANEL: Color32 = Color32::from_rgb(13, 14, 21);
const BG_BAR: Color32 = Color32::from_rgb(16, 18, 26);
const BG_ROW_SEL: Color32 = Color32::from_rgb(26, 30, 44);
const BG_ROW_HOVER: Color32 = Color32::from_rgb(19, 22, 32);

// type roles: Departure Mono (pixel grid) carries the console identity in
// the wordmark, column heads, labels and badges; JetBrains Mono carries
// every number and name. All-mono on purpose - this is an instrument.
fn disp(size: f32) -> FontId {
    FontId::new(size, egui::FontFamily::Name("display".into()))
}
fn mono(size: f32) -> FontId {
    FontId::monospace(size)
}
fn bold(size: f32) -> FontId {
    FontId::new(size, egui::FontFamily::Name("bold".into()))
}

fn setup_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    fonts.font_data.insert(
        "departure".into(),
        egui::FontData::from_static(include_bytes!("../../assets/fonts/DepartureMono-Regular.otf")),
    );
    fonts.font_data.insert(
        "jet".into(),
        egui::FontData::from_static(include_bytes!(
            "../../assets/fonts/JetBrainsMonoNerdFont-Regular.ttf"
        )),
    );
    fonts.font_data.insert(
        "jetbold".into(),
        egui::FontData::from_static(include_bytes!(
            "../../assets/fonts/JetBrainsMonoNerdFont-Bold.ttf"
        )),
    );
    fonts
        .families
        .insert(egui::FontFamily::Proportional, vec!["jet".into(), "departure".into()]);
    fonts
        .families
        .insert(egui::FontFamily::Monospace, vec!["jet".into()]);
    fonts.families.insert(
        egui::FontFamily::Name("display".into()),
        vec!["departure".into()],
    );
    fonts
        .families
        .insert(egui::FontFamily::Name("bold".into()), vec!["jetbold".into()]);
    ctx.set_fonts(fonts);
}

fn band_color32(band: i32, secondary: bool) -> Color32 {
    match band {
        0 => RED,
        1 => ORANGE,
        2 => YELLOW,
        3 => {
            if secondary {
                Color32::from_rgb(172, 128, 255)
            } else {
                Color32::from_rgb(56, 216, 232)
            }
        }
        _ => GREY,
    }
}

fn days_color32(days: f64) -> Color32 {
    band_color32(urgency_band(days), false)
}

fn fuel_color32(r: &Row) -> Color32 {
    if !r.has_fuel {
        GREY
    } else {
        days_color32(r.days)
    }
}

fn fuel2_color32(r: &Row) -> Color32 {
    band_color32(fuel2_band(r), true)
}

fn fmt_dur(mut s: i64) -> String {
    if s < 0 {
        s = 0;
    }
    let (d, h, m) = (s / 86400, (s % 86400) / 3600, (s % 3600) / 60);
    if d > 0 {
        format!("{d}d{h}h")
    } else if h > 0 {
        format!("{h}h{m}m")
    } else {
        format!("{m}m")
    }
}

// embedded 64px structure portraits (from art/structures, Semper's art)
const ICONS: &[(&str, &[u8])] = &[
    ("Astrahus", include_bytes!("../../assets/icons/astrahus.png")),
    ("Fortizar", include_bytes!("../../assets/icons/fortizar.png")),
    ("Keepstar", include_bytes!("../../assets/icons/keepstar.png")),
    ("Athanor", include_bytes!("../../assets/icons/athanor.png")),
    ("Tatara", include_bytes!("../../assets/icons/tatara.png")),
    ("Raitaru", include_bytes!("../../assets/icons/raitaru.png")),
    ("Azbel", include_bytes!("../../assets/icons/azbel.png")),
    ("Sotiyo", include_bytes!("../../assets/icons/sotiyo.png")),
    ("Metenox", include_bytes!("../../assets/icons/metenox.png")),
    ("Jump-Bridge", include_bytes!("../../assets/icons/ansiblex.png")),
    ("Cyno Bacon", include_bytes!("../../assets/icons/cyno-beacon.png")),
    ("Skyhook", include_bytes!("../../assets/icons/skyhook.png")),
    ("POS:amarr", include_bytes!("../../assets/icons/pos-amarr.png")),
    ("POS:caldari", include_bytes!("../../assets/icons/pos-caldari.png")),
    ("POS:gallente", include_bytes!("../../assets/icons/pos-gallente.png")),
    ("POS:minmatar", include_bytes!("../../assets/icons/pos-minmatar.png")),
];

fn decode_png(bytes: &[u8]) -> Option<egui::ColorImage> {
    let img = image::load_from_memory(bytes).ok()?.to_rgba8();
    let (w, h) = img.dimensions();
    Some(egui::ColorImage::from_rgba_unmultiplied(
        [w as usize, h as usize],
        img.as_raw(),
    ))
}

#[derive(PartialEq, Clone, Copy)]
enum Tab {
    Structures,
    Refuels,
}

struct Gui {
    app: Arc<App>,
    icons: HashMap<String, egui::TextureHandle>,
    tab: Tab,
    sort: usize,
    type_filter: String, // "" = All
    filter: String,
    sel_sid: i64,
    sel_refuel: Option<usize>,
}

const SORTS: [&str; 5] = ["fuel", "type", "system", "name", "need"];

impl Gui {
    fn new(cc: &eframe::CreationContext<'_>, app: Arc<App>) -> Self {
        setup_fonts(&cc.egui_ctx);
        let mut visuals = egui::Visuals::dark();
        visuals.panel_fill = BG_PANEL;
        visuals.window_fill = BG;
        visuals.extreme_bg_color = BG_BAR;
        visuals.selection.bg_fill = NEON_DIM_CYAN;
        visuals.override_text_color = Some(LIGHT);
        let edge = Stroke::new(1.0_f32, Color32::from_rgb(30, 34, 46));
        for w in [
            &mut visuals.widgets.inactive,
            &mut visuals.widgets.noninteractive,
            &mut visuals.widgets.open,
        ] {
            w.rounding = Rounding::same(3.0);
            w.weak_bg_fill = BG_BAR;
            w.bg_stroke = edge;
        }
        visuals.widgets.hovered.rounding = Rounding::same(3.0);
        visuals.widgets.hovered.weak_bg_fill = Color32::from_rgb(20, 24, 34);
        visuals.widgets.hovered.bg_stroke = Stroke::new(1.0_f32, NEON_DIM_CYAN);
        visuals.widgets.active.rounding = Rounding::same(3.0);
        visuals.widgets.active.weak_bg_fill = Color32::from_rgb(24, 28, 40);
        visuals.widgets.active.bg_stroke = Stroke::new(1.0_f32, NEON_CYAN);
        cc.egui_ctx.set_visuals(visuals);
        let mut style = (*cc.egui_ctx.style()).clone();
        style.spacing.item_spacing = Vec2::new(8.0, 6.0);
        style.spacing.button_padding = Vec2::new(10.0, 4.0);
        cc.egui_ctx.set_style(style);
        let mut icons = HashMap::new();
        for (name, bytes) in ICONS {
            if let Some(img) = decode_png(bytes) {
                icons.insert(
                    name.to_string(),
                    cc.egui_ctx
                        .load_texture(*name, img, egui::TextureOptions::LINEAR),
                );
            }
        }
        Gui {
            app,
            icons,
            tab: Tab::Structures,
            sort: 0,
            type_filter: String::new(),
            filter: String::new(),
            sel_sid: 0,
            sel_refuel: None,
        }
    }

    fn icon_for(&self, r: &Row) -> Option<&egui::TextureHandle> {
        if r.is_pos {
            return self.icons.get(&format!("POS:{}", r.pos_race)).or_else(|| self.icons.get("POS:minmatar"));
        }
        self.icons.get(&r.typ)
    }

    fn view(&self, rows: &[Row]) -> Vec<Row> {
        let mut f: Vec<Row> = Vec::new();
        for r in rows {
            if self.type_filter.is_empty() && r.is_skyhook {
                continue; // skyhooks live on their own tab only, never in All
            }
            if !self.type_filter.is_empty() && r.typ != self.type_filter {
                continue;
            }
            if !self.filter.is_empty() {
                let hay = format!("{} {}", r.name, r.system).to_lowercase();
                if !hay.contains(&self.filter.to_lowercase()) {
                    continue;
                }
            }
            f.push(r.clone());
        }
        f.sort_by(|a, b| match self.sort {
            1 => a.typ.cmp(&b.typ),
            2 => a.system.cmp(&b.system),
            3 => a.name.cmp(&b.name),
            4 => b.need.partial_cmp(&a.need).unwrap_or(std::cmp::Ordering::Equal),
            _ => {
                let da = if a.has_fuel { effective_days(a) } else { 1e9 };
                let db = if b.has_fuel { effective_days(b) } else { 1e9 };
                da.partial_cmp(&db).unwrap_or(std::cmp::Ordering::Equal)
            }
        });
        f
    }
}

// power badge (port of the C++ game-rule classification; ESI has no
// low-power state): ABANDONED = fuel 7+ days gone, LOW POWER = expired or
// absent fuel, OFFLINE = fueled but zero services online, LOW FUEL < 7d.
fn power_badge(r: &Row) -> Option<(&'static str, Color32)> {
    if r.is_skyhook || r.state.contains("anchor") {
        return None;
    }
    if !r.has_fuel || r.days <= 0.0 {
        if r.has_fuel && r.days < -7.0 {
            return Some(("ABANDONED", RED));
        }
        return Some(("LOW POWER", ORANGE));
    }
    if r.sv_on == 0 {
        return Some(("OFFLINE", GREY));
    }
    if r.days < 7.0 {
        return Some(("LOW FUEL", YELLOW));
    }
    None
}

fn under_attack(r: &Row, notifs: &[Notif]) -> bool {
    if r.state.contains("armor_reinforce") || r.state.contains("hull_reinforce") {
        return true;
    }
    let t = now();
    notifs.iter().any(|n| {
        n.sid == r.sid
            && t - n.at < 1800
            && matches!(
                n.typ.as_str(),
                "StructureUnderAttack" | "StructureLostShields" | "StructureLostArmor"
            )
    })
}

// one stacked-meter strip: value label rides the bar, black over the fill
// and band-colored past it (the C++ clip-split). frac < 0 = no gauge.
fn mini_bar(
    painter: &egui::Painter,
    rect: Rect,
    frac: f64,
    label: &str,
    right: &str,
    col: Color32,
) {
    painter.rect_filled(rect, Rounding::same(3.0), BG_BAR);
    let font = FontId::monospace(11.0);
    if frac < 0.0 {
        painter.text(rect.center(), Align2::CENTER_CENTER, label, font, col);
        painter.rect_stroke(rect, Rounding::same(3.0), Stroke::new(1.0_f32, Color32::from_rgb(30, 34, 46)));
        return;
    }
    let frac = frac.clamp(0.0, 1.0) as f32;
    let fill = Rect::from_min_size(rect.min, Vec2::new(rect.width() * frac, rect.height()));
    painter.rect_filled(fill, Rounding::same(3.0), col);
    let lp = Pos2::new(rect.min.x + 5.0, rect.center().y);
    let rp = Pos2::new(rect.max.x - 5.0, rect.center().y);
    let dark = Color32::from_rgb(12, 12, 18);
    // label over fill = dark, past fill = band color (two clipped passes)
    let pf = painter.with_clip_rect(fill.intersect(rect));
    pf.text(lp, Align2::LEFT_CENTER, label, font.clone(), dark);
    pf.text(rp, Align2::RIGHT_CENTER, right, font.clone(), dark);
    let rest = Rect::from_min_max(Pos2::new(fill.max.x, rect.min.y), rect.max);
    let pr = painter.with_clip_rect(rest.intersect(rect));
    pr.text(lp, Align2::LEFT_CENTER, label, font.clone(), col);
    pr.text(rp, Align2::RIGHT_CENTER, right, font, col);
    painter.rect_stroke(rect, Rounding::same(3.0), Stroke::new(1.0_f32, Color32::from_rgb(30, 34, 46)));
}

// fuel on top, gas/ozone (or skyhook meters) below
fn dual_gauge(painter: &egui::Painter, rect: Rect, r: &Row) {
    let h = (rect.height() - 3.0) / 2.0;
    let top = Rect::from_min_size(rect.min, Vec2::new(rect.width(), h));
    let bot = Rect::from_min_size(Pos2::new(rect.min.x, rect.min.y + h + 3.0), Vec2::new(rect.width(), h));
    if r.is_skyhook {
        if r.sky_unsec_m3 >= 0.0 {
            let label = format!(
                "{}{}",
                if r.sky_est { "~" } else { "" },
                isk_compact(r.sky_unsec_isk.max(0.0))
            );
            mini_bar(painter, top, r.sky_unsec_m3 / 10000.0, &label, "raidable", NEON_CYAN);
        } else {
            mini_bar(painter, top, -1.0, "NA", "", GREY);
        }
        if r.sky_streak >= 0 {
            let s = r.sky_streak;
            let col = if s <= 2 { GREEN } else if s <= 5 { YELLOW } else if s <= 8 { ORANGE } else { RED };
            mini_bar(painter, bot, s as f64 / 10.0, &format!("unraided x{s}"), "", col);
        } else {
            mini_bar(painter, bot, -1.0, "-", "", GREY);
        }
        return;
    }
    if r.has_fuel {
        mini_bar(
            painter,
            top,
            (r.days / GAUGE_DAYS).max(0.0),
            &format!("{:.1}d", r.days),
            &units_raw(r),
            fuel_color32(r),
        );
    } else {
        mini_bar(painter, top, -1.0, "--", "", GREY);
    }
    if r.fuel2_name.is_empty() {
        mini_bar(painter, bot, -1.0, "-", "", GREY);
    } else if r.fuel2 < 0.0 {
        mini_bar(painter, bot, -1.0, "?", "", GREY);
    } else if r.gas_day > 0.0 {
        let d = fuel2_days(r);
        mini_bar(painter, bot, (d / GAUGE_DAYS).max(0.0), &format!("{d:.1}d"), &gas30_raw(r), fuel2_color32(r));
    } else {
        let frac = if r.lo_target > 0.0 { r.fuel2 / r.lo_target } else { -1.0 };
        mini_bar(painter, bot, frac, &compact_units(r.fuel2), &gas30_raw(r), fuel2_color32(r));
    }
}

// state line under the name: state + power badge + timers + moon pull
fn state_bits(r: &Row, notifs: &[Notif], flash_on: bool) -> Vec<(String, Color32)> {
    let mut out: Vec<(String, Color32)> = Vec::new();
    if under_attack(r, notifs) {
        if flash_on {
            out.push(("UNDER ATTACK".into(), RED));
        } else {
            out.push(("UNDER ATTACK".into(), Color32::from_rgb(120, 30, 30)));
        }
    }
    if !r.state.is_empty() {
        out.push((r.state.replace('_', " "), INK_GRAY));
    }
    if let Some((b, c)) = power_badge(r) {
        out.push((b.to_string(), c));
    }
    let t = now();
    let te = parse_iso(&r.state_timer_end);
    if te > t {
        out.push((format!("timer {}", fmt_dur(te - t)), YELLOW));
    }
    let ua = parse_iso(&r.unanchors_at);
    if ua > t {
        out.push((format!("UNANCHORING {}", fmt_dur(ua - t)), YELLOW));
    } else if r.state == "unanchored" {
        out.push(("UNANCHORED".into(), GREY));
    }
    let ca = parse_iso(&r.chunk_arrival);
    if ca > t {
        out.push((format!("moon pull {}", fmt_dur(ca - t)), NEON_DIM_CYAN));
    } else if ca != 0 {
        let nd = parse_iso(&r.natural_decay);
        if r.popped_at > 0 && r.popped_at + 3 * 3600 > ca {
            out.push((
                format!(
                    "popped{}",
                    if r.popped_manual && !r.popped_by.is_empty() {
                        format!(" by {}", r.popped_by)
                    } else if r.popped_manual {
                        String::new()
                    } else {
                        " (auto)".into()
                    }
                ),
                GREEN,
            ));
        } else if nd > t {
            out.push((format!("chunk ready, auto-frac {}", fmt_dur(nd - t)), NEON_CYAN));
        } else {
            out.push(("chunk ready".into(), NEON_CYAN));
        }
    }
    if r.is_skyhook {
        let (ws, we) = (parse_iso(&r.sky_wstart), parse_iso(&r.sky_wend));
        if ws <= t && we > t {
            if flash_on {
                out.push(("RAIDABLE".into(), RED));
            }
            out.push((format!("window closes {}", fmt_dur(we - t)), RED));
        } else if ws > t {
            out.push((format!("window in {}", fmt_dur(ws - t)), YELLOW));
        }
        if r.sky_hourly >= 0.0 {
            out.push((format!("{}/h", commas(r.sky_hourly)), INK_GRAY));
        }
    }
    out
}

impl eframe::App for Gui {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let flash_on = (now_ms() / 400) % 2 == 0;
        ctx.request_repaint_after(std::time::Duration::from_millis(250));

        let g = self.app.shared.lock().unwrap().clone();
        let busy = self.app.busy.load(Ordering::SeqCst);
        let note = if g.note_at != 0 && now() - g.note_at < 20 {
            g.note.clone()
        } else {
            String::new()
        };
        let rows = self.view(&g.rows);
        let types = {
            let mut t: Vec<String> = Vec::new();
            for r in &g.rows {
                if !t.contains(&r.typ) {
                    t.push(r.typ.clone());
                }
            }
            t.sort();
            t
        };
        let (mut under14, mut under7) = (0, 0);
        for r in &rows {
            if r.has_fuel && r.days < 14.0 {
                under14 += 1;
            }
            if r.has_fuel && r.days < 7.0 {
                under7 += 1;
            }
        }
        let fresh = if !g.esi_lastmod.is_empty() {
            let mut s = format!("game data {}", rel_age(&g.esi_lastmod));
            if !g.esi_expires.is_empty() {
                let left = parse_iso(&g.esi_expires) - now();
                s += &if left > 0 {
                    format!("  next +{}m", (left + 59) / 60)
                } else {
                    "  next any moment".to_string()
                };
            }
            s
        } else if g.pulled_at.is_empty() {
            "--".into()
        } else {
            rel_age(&g.pulled_at)
        };

        // ---- masthead + burn line ----
        egui::TopBottomPanel::top("head").show(ctx, |ui| {
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                let (mark, _) = ui.allocate_exact_size(Vec2::new(8.0, 20.0), Sense::hover());
                ui.painter().rect_filled(mark, Rounding::ZERO, NEON_PINK);
                ui.label(egui::RichText::new("STOKER").font(disp(20.0)).color(NEON_PINK));
                ui.label(
                    egui::RichText::new(format!("v{}", env!("CARGO_PKG_VERSION")))
                        .font(disp(9.0))
                        .color(INK_GRAY),
                );
                ui.add_space(6.0);
                ui.label(
                    egui::RichText::new(format!(
                        "{}FUEL WATCH",
                        if g.corp_name.is_empty() {
                            String::new()
                        } else {
                            format!("{} · ", g.corp_name.to_uppercase())
                        }
                    ))
                    .font(disp(11.0))
                    .color(NEON_DIM_CYAN),
                );
                ui.label(
                    egui::RichText::new(" CORP FEED ")
                        .font(disp(8.5))
                        .color(INK_GRAY)
                        .background_color(Color32::from_rgb(18, 20, 30)),
                );
                ui.label(
                    egui::RichText::new(format!("{}/{}", rows.len(), g.rows.len()))
                        .font(mono(11.0))
                        .color(INK_GRAY),
                );
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.label(egui::RichText::new(&fresh).font(mono(10.0)).color(INK_GRAY));
                    ui.add_space(10.0);
                    let chip = |ui: &mut egui::Ui, label: &str, n: i32, col: Color32| {
                        ui.label(
                            egui::RichText::new(format!(" {label} {n} "))
                                .font(disp(10.0))
                                .color(if n > 0 { col } else { GREEN })
                                .background_color(Color32::from_rgb(18, 20, 30)),
                        );
                    };
                    chip(ui, "<7D", under7, RED);
                    chip(ui, "<14D", under14, YELLOW);
                });
            });
            ui.add_space(2.0);
            self.burn_line(ui, &g.rows, flash_on);
            ui.add_space(2.0);
            ui.horizontal(|ui| {
                ui.selectable_value(&mut self.tab, Tab::Structures, "Structures");
                ui.selectable_value(&mut self.tab, Tab::Refuels, "Refuel Log");
                ui.separator();
                egui::ComboBox::from_id_salt("type")
                    .selected_text(if self.type_filter.is_empty() {
                        "All types".to_string()
                    } else {
                        self.type_filter.clone()
                    })
                    .show_ui(ui, |ui| {
                        ui.selectable_value(&mut self.type_filter, String::new(), "All types");
                        for t in &types {
                            ui.selectable_value(&mut self.type_filter, t.clone(), t);
                        }
                    });
                egui::ComboBox::from_id_salt("sort")
                    .selected_text(format!("sort: {}", SORTS[self.sort]))
                    .show_ui(ui, |ui| {
                        for (i, s) in SORTS.iter().enumerate() {
                            ui.selectable_value(&mut self.sort, i, *s);
                        }
                    });
                ui.add(
                    egui::TextEdit::singleline(&mut self.filter)
                        .hint_text("filter name/system")
                        .desired_width(160.0),
                );
                if !self.filter.is_empty() && ui.small_button("x").clicked() {
                    self.filter.clear();
                }
                ui.separator();
                if ui.button("refresh").clicked() {
                    let c = ctx.clone();
                    net::force_refresh(&self.app, move || c.request_repaint());
                }
                if busy {
                    ui.label(egui::RichText::new("refreshing...").color(YELLOW));
                }
            });
            ui.add_space(4.0);
        });

        // ---- status bar ----
        egui::TopBottomPanel::bottom("status").show(ctx, |ui| {
            ui.horizontal(|ui| {
                if !note.is_empty() {
                    ui.label(egui::RichText::new(&note).color(GREEN));
                } else if !g.status.is_empty() {
                    ui.label(egui::RichText::new(&g.status).color(YELLOW));
                } else {
                    ui.label(
                        egui::RichText::new(
                            "gauges: days left | haul to 30d on the right (? = needs a Director token)",
                        )
                        .color(INK_GRAY)
                        .size(11.0),
                    );
                }
            });
        });

        // ---- detail side panel ----
        let sel_row = g.rows.iter().find(|r| r.sid == self.sel_sid).cloned();
        if let Some(d) = &sel_row {
            egui::SidePanel::right("detail")
                .resizable(true)
                .default_width(340.0)
                .show(ctx, |ui| {
                    egui::ScrollArea::vertical().show(ui, |ui| {
                        ui.add_space(6.0);
                        ui.horizontal(|ui| {
                            if let Some(tex) = self.icon_for(d) {
                                ui.image((tex.id(), Vec2::splat(48.0)));
                            }
                            ui.vertical(|ui| {
                                ui.label(egui::RichText::new(&d.name).color(NEON_PINK).font(bold(13.5)));
                                ui.horizontal(|ui| {
                                    ui.label(egui::RichText::new(&d.system).color(NEON_CYAN));
                                    ui.label(egui::RichText::new(if d.is_pos { &d.tower } else { &d.typ }).color(LIGHT));
                                });
                                ui.label(egui::RichText::new(d.state.replace('_', " ")).color(INK_GRAY).size(11.0));
                            });
                        });
                        ui.add_space(4.0);
                        ui.label(
                            egui::RichText::new(format!(
                                "services: {}",
                                if d.services.is_empty() { "none" } else { &d.services }
                            ))
                            .color(INK_GRAY)
                            .size(11.0),
                        );
                        ui.separator();
                        let kv = |ui: &mut egui::Ui, k: &str, v: String, c: Color32| {
                            ui.horizontal(|ui| {
                                ui.label(
                                    egui::RichText::new(format!("{k:>12}"))
                                        .color(NEON_DIM_CYAN)
                                        .font(disp(9.5)),
                                );
                                ui.label(egui::RichText::new(v).color(c).font(mono(12.0)));
                            });
                        };
                        if d.has_fuel {
                            let ex = if d.fuel_expires.is_empty() {
                                String::new()
                            } else {
                                format!("  runs out {} UTC", cells_prefix(&d.fuel_expires, 16))
                            };
                            kv(ui, "FUEL", format!("{:.1} days{ex}", d.days), days_color32(d.days));
                        } else {
                            kv(ui, "FUEL", "no fuel data".into(), GREY);
                        }
                        if d.burn7 >= 0.0 || d.burn30 >= 0.0 {
                            let mut bt = String::new();
                            if d.burn7 >= 0.0 {
                                bt += &format!("x{:.2} (7d)", d.burn7);
                            }
                            if d.burn30 >= 0.0 {
                                bt += &format!("{}x{:.2} (30d)", if bt.is_empty() { "" } else { "  " }, d.burn30);
                            }
                            if d.est >= 0.0 {
                                bt += &format!("  est {:.1}d", d.est);
                            }
                            kv(ui, "BURN", bt, LIGHT);
                        }
                        if d.bpd >= 0.0 {
                            kv(ui, "RATE", format!("{} blocks/day", commas(d.bpd)), LIGHT);
                        }
                        if d.blocks_now >= 0.0 {
                            kv(
                                ui,
                                "IN BAY",
                                format!("~{} blocks ({} m3)", commas(d.blocks_now), commas(d.m3_now)),
                                GREEN,
                            );
                        }
                        if !d.fuel2_name.is_empty() {
                            let label = if d.fuel2_name == "Magmatic Gas" { "MAGMATIC" } else { "OZONE" };
                            if d.fuel2 < 0.0 {
                                kv(ui, label, "? (needs a Director-role data source)".into(), GREY);
                            } else {
                                let extra = if fuel2_days(d) >= 0.0 {
                                    format!("  (~{}d at drill rate)", commas(fuel2_days(d)))
                                } else {
                                    String::new()
                                };
                                kv(ui, label, format!("{} units{extra}", commas(d.fuel2)), fuel2_color32(d));
                            }
                        }
                        if d.need >= 0.0 {
                            if d.need == 0.0 {
                                kv(ui, "FUEL BLOCKS", "topped past 30 days".into(), GREEN);
                            } else {
                                kv(
                                    ui,
                                    "FUEL BLOCKS",
                                    format!("haul {} blocks ({} m3)", commas(d.need), commas(d.m3)),
                                    days_color32(d.days),
                                );
                            }
                        }
                        if d.lo_min >= 0.0 {
                            kv(
                                ui,
                                "DOCTRINE",
                                format!("ozone min {} / fill to {}", commas(d.lo_min), commas(d.lo_target)),
                                LIGHT,
                            );
                        }
                        if d.gas_day >= 0.0 {
                            kv(
                                ui,
                                "GAS",
                                format!("{}/day, month haul {} ({} m3)", commas(d.gas_day), commas(d.gas_month), commas(d.gas_m3)),
                                LIGHT,
                            );
                        }
                        if d.stront >= 0.0 {
                            let h = if d.stront_hours >= 0.0 {
                                format!("  (~{:.1}h reinforce)", d.stront_hours)
                            } else {
                                String::new()
                            };
                            kv(ui, "STRONT", format!("{} units{h}", commas(d.stront)), LIGHT);
                        }
                        if d.has_refuel {
                            kv(
                                ui,
                                "LAST FUELED",
                                format!("{} (+{:.1}d)", rel_age(&d.last_refuel), d.refuel_added),
                                GREEN,
                            );
                        }
                        for (txt, col) in state_bits(d, &g.notifs, flash_on) {
                            kv(ui, "", txt, col);
                        }
                        ui.separator();
                        let claim_btn = egui::Button::new(
                            egui::RichText::new("I FUELED THIS")
                                .font(disp(10.0))
                                .color(NEON_PINK),
                        )
                        .fill(Color32::from_rgb(40, 12, 34))
                        .stroke(Stroke::new(1.0_f32, NEON_PINK.gamma_multiply(0.5)));
                        if ui.add(claim_btn).clicked() {
                            let c = ctx.clone();
                            let seen = if self.tab == Tab::Refuels {
                                self.sel_refuel
                                    .and_then(|i| g.refuels.get(i))
                                    .filter(|v| !v.pending && v.sid == d.sid)
                                    .map(|v| v.seen_at.clone())
                                    .unwrap_or_default()
                            } else {
                                String::new()
                            };
                            net::send_claim(&self.app, d.sid, seen, move || c.request_repaint());
                        }
                        ui.add_space(6.0);
                        ui.label(
                            egui::RichText::new("REFUEL LOG · THIS STRUCTURE, NEWEST FIRST")
                                .color(NEON_DIM_CYAN)
                                .font(disp(9.5)),
                        );
                        if d.log.is_empty() {
                            ui.label(egui::RichText::new("(no refuels seen yet)").color(GREY).size(11.0));
                        } else {
                            for ev in &d.log {
                                let blk = if d.bpd > 0.0 { commas(ev.days_added * d.bpd) } else { "--".into() };
                                ui.horizontal(|ui| {
                                    ui.label(
                                        egui::RichText::new(format!("{:<10}", rel_age(&ev.seen_at)))
                                            .color(LIGHT)
                                            .monospace()
                                            .size(11.0),
                                    );
                                    ui.label(
                                        egui::RichText::new(format!("+{:.1}d", ev.days_added))
                                            .color(GREEN)
                                            .monospace()
                                            .size(11.0),
                                    );
                                    ui.label(egui::RichText::new(format!("{blk} blk")).color(GREEN).monospace().size(11.0));
                                    ui.label(
                                        egui::RichText::new(if ev.by.is_empty() { "?" } else { &ev.by })
                                            .color(if ev.by.is_empty() { GREY } else { NEON_PINK })
                                            .size(11.0),
                                    );
                                });
                            }
                        }
                        ui.add_space(8.0);
                    });
                });
        }

        // ---- main table ----
        egui::CentralPanel::default().show(ctx, |ui| {
            if g.rows.is_empty() {
                ui.centered_and_justified(|ui| {
                    ui.label(
                        egui::RichText::new(if g.status.is_empty() {
                            "loading...".to_string()
                        } else {
                            g.status.clone()
                        })
                        .color(INK_GRAY),
                    );
                });
                return;
            }
            match self.tab {
                Tab::Structures => self.structures_table(ui, &rows, &g.notifs, flash_on),
                Tab::Refuels => self.refuels_table(ui, &g),
            }
        });
    }
}

impl Gui {
    // THE BURN LINE: the whole corp on one 0-30d heat ruler. Every fueled
    // structure is a tick at its usage-adjusted days left; ticks under 3d
    // flicker like embers on the shared flash clock. Hover names a tick,
    // click selects it in the table.
    fn burn_line(&mut self, ui: &mut egui::Ui, rows: &[Row], flash_on: bool) {
        const H: f32 = 84.0; // strip height
        const LANES: usize = 4; // overlapping ticks stack instead of hiding
        const LANE_H: f32 = 12.0;
        let (rect, resp) =
            ui.allocate_exact_size(Vec2::new(ui.available_width(), H), Sense::click());
        let p = ui.painter_at(rect);
        let x0 = rect.min.x + 8.0;
        let x1 = rect.max.x - 8.0;
        let w = x1 - x0;
        let base = rect.max.y - 16.0;
        p.text(
            Pos2::new(x0, rect.min.y + 2.0),
            Align2::LEFT_TOP,
            "BURN LINE",
            disp(10.0),
            NEON_DIM_CYAN,
        );
        p.text(
            Pos2::new(x1, rect.min.y + 3.0),
            Align2::RIGHT_TOP,
            "EACH TICK = ONE STRUCTURE AT ITS DAYS OF FUEL · CLICK TO SELECT",
            disp(8.5),
            Color32::from_rgb(60, 66, 82),
        );
        p.line_segment(
            [Pos2::new(x0, base), Pos2::new(x1, base)],
            Stroke::new(1.0_f32, Color32::from_rgb(30, 34, 46)),
        );
        let xat = |d: f64| x0 + w * (d.clamp(0.0, 30.0) / 30.0) as f32;
        for (d, lab) in [(0.0, "0d"), (3.0, "3"), (7.0, "7"), (14.0, "14"), (30.0, "30d+")] {
            let x = xat(d);
            p.line_segment(
                [Pos2::new(x, base), Pos2::new(x, base + 5.0)],
                Stroke::new(1.0_f32, Color32::from_rgb(46, 52, 68)),
            );
            // end labels hug inward so the panel edge can't clip them
            let align = if d == 0.0 {
                Align2::LEFT_TOP
            } else if d == 30.0 {
                Align2::RIGHT_TOP
            } else {
                Align2::CENTER_TOP
            };
            p.text(Pos2::new(x, base + 6.0), align, lab, disp(9.0), INK_GRAY);
        }
        // lay ticks into lanes: same-x neighbours stack upward instead of
        // painting over each other, so dense clusters stay clickable
        struct Tick {
            x: f32,
            lane: usize,
            sid: i64,
            label: String,
            days: f64,
            col: Color32,
        }
        let mut lane_count: HashMap<i32, usize> = HashMap::new();
        let mut ticks: Vec<Tick> = Vec::new();
        for r in rows {
            if !r.has_fuel || r.is_skyhook {
                continue;
            }
            let d = effective_days(r);
            let x = xat(d);
            let bucket = ((x - x0) / 5.0) as i32;
            let n = lane_count.entry(bucket).or_insert(0);
            let lane = (*n).min(LANES - 1);
            *n += 1;
            let mut col = days_color32(d);
            if d < 3.0 && !flash_on {
                col = col.gamma_multiply(0.55); // ember dims between flares
            }
            ticks.push(Tick {
                x,
                lane,
                sid: r.sid,
                label: format!("{}  {}", r.name, r.system),
                days: d,
                col,
            });
        }
        let hover = resp.hover_pos();
        let mut near: Option<(f32, usize)> = None;
        for (i, t) in ticks.iter().enumerate() {
            if let Some(hp) = hover {
                // aim by x, break ties by lane so stacked ticks stay reachable
                let bot = base - 2.0 - t.lane as f32 * LANE_H;
                let dx = (hp.x - t.x).abs();
                let dy = (hp.y - (bot - LANE_H / 2.0)).abs().min(LANE_H);
                let dist = dx * 2.0 + dy * 0.5;
                if dx < 10.0 && near.as_ref().map_or(true, |(bd, _)| dist < *bd) {
                    near = Some((dist, i));
                }
            }
        }
        for (i, t) in ticks.iter().enumerate() {
            let bot = base - 2.0 - t.lane as f32 * LANE_H;
            let mut top = bot - (LANE_H - 2.0);
            let mut col = t.col;
            let mut width = 3.0_f32;
            if t.sid == self.sel_sid {
                col = NEON_PINK;
                top -= 3.0;
                width = 4.0;
            }
            if near.map(|(_, ni)| ni) == Some(i) {
                col = Color32::WHITE;
                top -= 3.0;
                width = 4.0;
            } else if t.days < 3.0 && flash_on {
                top -= 3.0; // ember flare
            }
            p.line_segment([Pos2::new(t.x, top), Pos2::new(t.x, bot)], Stroke::new(width, col));
        }
        if let Some((_, i)) = near {
            let t = &ticks[i];
            if resp.clicked() {
                self.sel_sid = t.sid;
            }
            resp.clone().on_hover_cursor(egui::CursorIcon::PointingHand);
            egui::show_tooltip_at_pointer(ui.ctx(), ui.layer_id(), resp.id.with("bl"), |ui| {
                ui.label(
                    egui::RichText::new(format!("{}   {:.1}d", t.label, t.days))
                        .font(mono(12.0))
                        .color(LIGHT),
                );
            });
        }
    }

    fn structures_table(&mut self, ui: &mut egui::Ui, rows: &[Row], notifs: &[Notif], flash_on: bool) {
        const ROW_H: f32 = 46.0;
        let w = ui.available_width();
        let meter_w = 180.0_f32.min(w * 0.25);
        let x_icon = 4.0;
        let x_meter = x_icon + 42.0;
        let x_type = x_meter + meter_w + 10.0;
        let x_sys = x_type + 96.0;
        let x_name = x_sys + 76.0;
        // column headers
        ui.horizontal(|ui| {
            let p = ui.painter();
            let y = ui.cursor().min.y + 8.0;
            let base = ui.cursor().min.x;
            let font = disp(9.5);
            p.text(Pos2::new(base + x_meter, y), Align2::LEFT_CENTER, "FUEL / GAS·OZ (30D)", font.clone(), NEON_DIM_CYAN);
            p.text(Pos2::new(base + x_type, y), Align2::LEFT_CENTER, "TYPE", font.clone(), NEON_DIM_CYAN);
            p.text(Pos2::new(base + x_sys, y), Align2::LEFT_CENTER, "SYSTEM", font.clone(), NEON_DIM_CYAN);
            p.text(Pos2::new(base + x_name, y), Align2::LEFT_CENTER, "NAME", font, NEON_DIM_CYAN);
            ui.allocate_space(Vec2::new(w, 16.0));
        });
        egui::ScrollArea::vertical()
            .auto_shrink([false; 2])
            .show_rows(ui, ROW_H, rows.len(), |ui, range| {
                for i in range {
                    let r = &rows[i];
                    let (rect, resp) =
                        ui.allocate_exact_size(Vec2::new(ui.available_width(), ROW_H), Sense::click());
                    if resp.clicked() {
                        self.sel_sid = if self.sel_sid == r.sid { 0 } else { r.sid };
                    }
                    let p = ui.painter_at(rect);
                    if r.sid == self.sel_sid {
                        p.rect_filled(rect, Rounding::same(4.0), BG_ROW_SEL);
                        p.rect_filled(
                            Rect::from_min_size(rect.min, Vec2::new(3.0, rect.height())),
                            Rounding::ZERO,
                            NEON_PINK,
                        );
                    } else if resp.hovered() {
                        p.rect_filled(rect, Rounding::same(4.0), BG_ROW_HOVER);
                    } else if i % 2 == 1 {
                        p.rect_filled(rect, Rounding::ZERO, Color32::from_rgb(12, 13, 20));
                    }
                    // heat edge: anything under 3 days carries a red rim
                    if r.has_fuel && r.days < 3.0 && r.sid != self.sel_sid {
                        p.rect_filled(
                            Rect::from_min_size(rect.min, Vec2::new(2.0, rect.height())),
                            Rounding::ZERO,
                            RED.gamma_multiply(0.7),
                        );
                    }
                    // portrait
                    let icon_rect = Rect::from_min_size(
                        Pos2::new(rect.min.x + x_icon, rect.min.y + 4.0),
                        Vec2::splat(ROW_H - 8.0),
                    );
                    if let Some(tex) = self.icon_for(r) {
                        p.image(
                            tex.id(),
                            icon_rect,
                            Rect::from_min_max(Pos2::new(0.0, 0.0), Pos2::new(1.0, 1.0)),
                            Color32::WHITE,
                        );
                    } else {
                        p.rect_filled(icon_rect, Rounding::same(5.0), Color32::from_rgb(24, 28, 40));
                        p.text(
                            icon_rect.center(),
                            Align2::CENTER_CENTER,
                            r.typ.chars().next().unwrap_or('?').to_string(),
                            FontId::proportional(16.0),
                            INK_GRAY,
                        );
                    }
                    // stacked meters
                    let meter_rect = Rect::from_min_size(
                        Pos2::new(rect.min.x + x_meter, rect.min.y + 6.0),
                        Vec2::new(meter_w, ROW_H - 12.0),
                    );
                    dual_gauge(&p, meter_rect, r);
                    // text columns
                    let y1 = rect.min.y + 15.0;
                    let y2 = rect.min.y + 32.0;
                    p.text(
                        Pos2::new(rect.min.x + x_type, y1),
                        Align2::LEFT_CENTER,
                        &r.typ,
                        mono(12.0),
                        INK_GRAY,
                    );
                    p.text(
                        Pos2::new(rect.min.x + x_sys, y1),
                        Align2::LEFT_CENTER,
                        &r.system,
                        mono(12.5),
                        NEON_CYAN,
                    );
                    p.text(
                        Pos2::new(rect.min.x + x_name, y1),
                        Align2::LEFT_CENTER,
                        &r.name,
                        bold(13.0),
                        Color32::from_rgb(230, 234, 245),
                    );
                    let mut x = rect.min.x + x_name;
                    for (txt, col) in state_bits(r, notifs, flash_on) {
                        let painted = p.text(
                            Pos2::new(x, y2),
                            Align2::LEFT_CENTER,
                            txt.to_uppercase(),
                            disp(8.5),
                            col,
                        );
                        x = painted.max.x + 10.0;
                    }
                }
            });
    }

    fn refuels_table(&mut self, ui: &mut egui::Ui, g: &Shared) {
        const ROW_H: f32 = 22.0;
        let x_when = 6.0;
        let x_days = 96.0;
        let x_blocks = 156.0;
        let x_by = 260.0;
        let x_sys = 390.0;
        let x_name = 470.0;
        ui.horizontal(|ui| {
            let p = ui.painter();
            let y = ui.cursor().min.y + 8.0;
            let base = ui.cursor().min.x;
            let font = disp(9.5);
            for (x, t) in [
                (x_when, "WHEN"),
                (x_days, "+DAYS"),
                (x_blocks, "BLOCKS"),
                (x_by, "BY"),
                (x_sys, "SYSTEM"),
                (x_name, "NAME"),
            ] {
                p.text(Pos2::new(base + x, y), Align2::LEFT_CENTER, t, font.clone(), NEON_DIM_CYAN);
            }
            ui.allocate_space(Vec2::new(ui.available_width(), 16.0));
        });
        let refuels = &g.refuels;
        if refuels.is_empty() {
            ui.label(
                egui::RichText::new("(no refuels seen yet - the log fills in as polls catch fuel jumps)")
                    .color(GREY),
            );
            return;
        }
        egui::ScrollArea::vertical()
            .auto_shrink([false; 2])
            .show_rows(ui, ROW_H, refuels.len(), |ui, range| {
                for i in range {
                    let v = &refuels[i];
                    let (rect, resp) =
                        ui.allocate_exact_size(Vec2::new(ui.available_width(), ROW_H), Sense::click());
                    if resp.clicked() {
                        self.sel_refuel = Some(i);
                        self.sel_sid = v.sid;
                    }
                    let p = ui.painter_at(rect);
                    if self.sel_refuel == Some(i) {
                        p.rect_filled(rect, Rounding::same(3.0), BG_ROW_SEL);
                    } else if resp.hovered() {
                        p.rect_filled(rect, Rounding::same(3.0), BG_ROW_HOVER);
                    }
                    let y = rect.center().y;
                    let font = FontId::monospace(12.0);
                    let cell = |x: f32, t: &str, c: Color32| {
                        p.text(Pos2::new(rect.min.x + x, y), Align2::LEFT_CENTER, t, font.clone(), c);
                    };
                    cell(x_when, &rel_age(&v.seen_at), LIGHT);
                    if v.pending {
                        cell(x_days, "...", YELLOW);
                        cell(x_blocks, "awaiting ESI", YELLOW);
                    } else {
                        cell(x_days, &format!("+{:.1}d", v.days_added), GREEN);
                        cell(
                            x_blocks,
                            &(if v.blocks >= 0.0 { commas(v.blocks) } else { "--".into() }),
                            if v.blocks >= 0.0 { GREEN } else { GREY },
                        );
                    }
                    cell(
                        x_by,
                        if v.by.is_empty() { "?" } else { &v.by },
                        if v.by.is_empty() { GREY } else { NEON_PINK },
                    );
                    cell(x_sys, &v.system, NEON_CYAN);
                    cell(x_name, &v.name, Color32::from_rgb(230, 234, 245));
                }
            });
    }
}

fn main() -> eframe::Result<()> {
    let app = match net::make_app() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{e}");
            // show the config error in a window too (icon launches have no stderr)
            let opts = eframe::NativeOptions {
                viewport: egui::ViewportBuilder::default().with_inner_size([560.0, 220.0]),
                ..Default::default()
            };
            let msg = e.clone();
            return eframe::run_native(
                "STOKER (Rust)",
                opts,
                Box::new(move |_| {
                    Ok(Box::new(ErrApp { msg }) as Box<dyn eframe::App>)
                }),
            );
        }
    };

    let icon = image::load_from_memory(include_bytes!("../../stoker-icon-128.png"))
        .ok()
        .map(|i| {
            let rgba = i.to_rgba8();
            let (w, h) = rgba.dimensions();
            egui::IconData {
                rgba: rgba.into_raw(),
                width: w,
                height: h,
            }
        });

    let mut viewport = egui::ViewportBuilder::default()
        .with_inner_size([1280.0, 820.0])
        .with_min_inner_size([900.0, 560.0])
        .with_app_id("stoker-rs")
        .with_title("STOKER (Rust)");
    if let Some(icon) = icon {
        viewport = viewport.with_icon(Arc::new(icon));
    }
    let opts = eframe::NativeOptions {
        viewport,
        ..Default::default()
    };

    let app2 = app.clone();
    let res = eframe::run_native(
        "STOKER (Rust)",
        opts,
        Box::new(move |cc| {
            let c = cc.egui_ctx.clone();
            net::spawn_poller(&app2, move || c.request_repaint());
            Ok(Box::new(Gui::new(cc, app2.clone())) as Box<dyn eframe::App>)
        }),
    );
    app.run.store(false, Ordering::SeqCst);
    res
}

struct ErrApp {
    msg: String,
}

impl eframe::App for ErrApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::CentralPanel::default().show(ctx, |ui| {
            ui.heading("STOKER cannot start");
            ui.add_space(8.0);
            ui.label(&self.msg);
        });
    }
}
