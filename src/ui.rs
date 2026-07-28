// rendering: a span-for-span port of the FTXUI renderer in bpos-dash.cpp
// onto ratatui Lines. One Paragraph inside a rounded border per frame.

use crate::data::*;
use crate::util::*;
use ratatui::prelude::*;
use ratatui::widgets::{Block, BorderType, Paragraph};
use std::collections::HashSet;

// Hot Neon palette (matches the desktop rice + app icon).
pub const NEON_PINK: Color = Color::Rgb(255, 43, 214);
pub const NEON_CYAN: Color = Color::Rgb(0, 229, 255);
pub const NEON_DIM_CYAN: Color = Color::Rgb(0, 130, 150);
pub const INK_GRAY: Color = Color::Rgb(110, 118, 138);
pub const GREY: Color = Color::Rgb(128, 136, 150);
pub const LIGHT: Color = Color::Rgb(198, 206, 222);
pub const GREEN: Color = Color::Rgb(90, 225, 130);
pub const YELLOW: Color = Color::Rgb(250, 215, 70);
pub const RED: Color = Color::Rgb(255, 70, 70);

pub fn band_color(band: i32, secondary: bool) -> Color {
    match band {
        0 => RED,
        1 => Color::Rgb(255, 140, 0),
        2 => YELLOW,
        3 => {
            if secondary {
                Color::Rgb(172, 128, 255)
            } else {
                Color::Rgb(56, 216, 232)
            }
        }
        _ => GREY,
    }
}

pub fn days_color(days: f64) -> Color {
    band_color(urgency_band(days), false)
}

pub fn fuel_color(r: &Row) -> Color {
    if !r.has_fuel {
        GREY
    } else {
        days_color(r.days)
    }
}

pub fn fuel2_color(r: &Row) -> Color {
    band_color(fuel2_band(r), true)
}

pub fn need_color(r: &Row) -> Color {
    if r.need < 0.0 {
        GREY
    } else if r.need == 0.0 {
        GREEN
    } else {
        days_color(if r.has_fuel { r.days } else { -1.0 })
    }
}

fn sp(s: impl Into<String>, c: Color) -> Span<'static> {
    Span::styled(s.into(), Style::default().fg(c))
}

fn spb(s: impl Into<String>, c: Color) -> Span<'static> {
    Span::styled(s.into(), Style::default().fg(c).add_modifier(Modifier::BOLD))
}

fn spd(s: impl Into<String>, c: Color) -> Span<'static> {
    Span::styled(s.into(), Style::default().fg(c).add_modifier(Modifier::DIM))
}

fn spans_width(spans: &[Span]) -> usize {
    spans.iter().map(|s| cells(&s.content)).sum()
}

// left spans + filler + right spans composed to `width` cells
fn line_lr(mut left: Vec<Span<'static>>, right: Vec<Span<'static>>, width: usize) -> Line<'static> {
    let used = spans_width(&left) + spans_width(&right);
    if used < width {
        left.push(Span::raw(" ".repeat(width - used)));
    }
    left.extend(right);
    Line::from(left)
}

// A gauge with its value written INSIDE: label chars ride the bar, showing
// inverted on the filled part and colored on the empty part. frac < 0 means
// "no gauge": the label renders centered and grey.
pub fn bar_with_text(label: &str, frac: f64, col: Color, width: usize) -> Vec<Span<'static>> {
    let label: String = if cells(label) > width {
        cells_prefix(label, width)
    } else {
        label.to_string()
    };
    if frac < 0.0 {
        let lp = width.saturating_sub(cells(&label)) / 2;
        return vec![sp(pad(&format!("{}{}", " ".repeat(lp), label), width, false), col)];
    }
    let frac = frac.min(1.0);
    // at most four style runs: label-on-fill, label-past-fill, fill, empty tail
    let fill = (frac * width as f64).round() as usize;
    let lab = cells(&label);
    let on_fill = lab.min(fill);
    let past_fill = lab - on_fill;
    let blank_fill = fill.saturating_sub(lab);
    let tail = width - fill.max(lab);
    let head = cells_prefix(&label, on_fill);
    let rest: String = label.chars().skip(on_fill).collect();
    let mut runs = Vec::new();
    if on_fill > 0 {
        runs.push(Span::styled(
            head,
            Style::default()
                .bg(col)
                .fg(Color::Rgb(10, 10, 16))
                .add_modifier(Modifier::BOLD),
        ));
    }
    if past_fill > 0 {
        runs.push(spb(rest, col));
    }
    if blank_fill > 0 {
        runs.push(Span::styled(" ".repeat(blank_fill), Style::default().bg(col)));
    }
    if tail > 0 {
        runs.push(spd("░".repeat(tail), col));
    }
    runs
}

// One 30-day gauge scale for every days-based bar on the desk
fn days_gauge(days: f64, right: &str, col: Color, width: usize) -> Vec<Span<'static>> {
    let label = two_sided(&format!("{days:.1}d"), right, width);
    bar_with_text(&label, (days / GAUGE_DAYS).max(0.0), col, width)
}

// F cell: fuel gauge, days inside on the left, blocks-to-30d on the right
pub fn fuel_cell(r: &Row, width: usize) -> Vec<Span<'static>> {
    if !r.has_fuel {
        return bar_with_text("--", -1.0, fuel_color(r), width);
    }
    days_gauge(r.days, &units_raw(r), fuel_color(r), width)
}

// F² cell: secondary fuel gauge
pub fn fuel2_cell(r: &Row, width: usize) -> Vec<Span<'static>> {
    if r.fuel2_name.is_empty() {
        return bar_with_text("-", -1.0, fuel2_color(r), width);
    }
    if r.fuel2 < 0.0 {
        return bar_with_text("?", -1.0, fuel2_color(r), width);
    }
    if r.gas_day > 0.0 {
        return days_gauge(fuel2_days(r), &gas30_raw(r), fuel2_color(r), width);
    }
    let frac = if r.lo_target > 0.0 { r.fuel2 / r.lo_target } else { -1.0 };
    bar_with_text(
        &two_sided(&compact_units(r.fuel2), &gas30_raw(r), width),
        frac,
        fuel2_color(r),
        width,
    )
}

// view/interaction state owned by the main loop
pub struct UiState {
    pub sort_mode: usize, // 0 fuel, 1 type, 2 system, 3 name, 4 need
    pub type_idx: usize,  // 0 = All
    pub text_filter: String,
    pub filter_mode: bool,
    pub log_mode: bool,    // [f]: refuel-event log instead of structures
    pub detail_mode: bool, // [->]: single-structure detail page
    pub detail_sid: i64,
    pub offset: usize,
    pub sel: usize,
    pub marked: HashSet<i64>, // [space] tally marks
}

pub const SORTN: [&str; 5] = ["fuel", "type", "system", "name", "need"];

// distinct type list (for label + cycling bound)
pub fn type_list(rows: &[Row]) -> Vec<String> {
    let mut types: Vec<String> = Vec::new();
    for r in rows {
        if !types.contains(&r.typ) {
            types.push(r.typ.clone());
        }
    }
    types.sort();
    types
}

// filter + sort the raw rows into what the table shows
pub fn view(rows: &[Row], ui: &UiState) -> Vec<Row> {
    let types = type_list(rows);
    let want: &str = if ui.type_idx > 0 && ui.type_idx <= types.len() {
        &types[ui.type_idx - 1]
    } else {
        ""
    };
    let mut f: Vec<Row> = Vec::new();
    for r in rows {
        // skyhooks live on their own tab only, never in All
        if want.is_empty() && r.is_skyhook {
            continue;
        }
        if !want.is_empty() && r.typ != want {
            continue;
        }
        if !ui.text_filter.is_empty() {
            let hay = format!("{} {}", r.name, r.system).to_lowercase();
            if !hay.contains(&ui.text_filter.to_lowercase()) {
                continue;
            }
        }
        f.push(r.clone());
    }
    f.sort_by(|a, b| match ui.sort_mode {
        1 => a.typ.cmp(&b.typ),
        2 => a.system.cmp(&b.system),
        3 => a.name.cmp(&b.name),
        4 => b.need.partial_cmp(&a.need).unwrap_or(std::cmp::Ordering::Equal),
        _ => {
            // soonest usage-adjusted runout first
            let da = if a.has_fuel { effective_days(a) } else { 1e9 };
            let db = if b.has_fuel { effective_days(b) } else { 1e9 };
            da.partial_cmp(&db).unwrap_or(std::cmp::Ordering::Equal)
        }
    });
    f
}

fn note_or_status(note: &str, status: &str, busy: bool) -> Vec<Span<'static>> {
    if !note.is_empty() {
        vec![sp(format!(" {note} "), GREEN)]
    } else if !status.is_empty() {
        vec![sp(format!(" {status} "), YELLOW)]
    } else if busy {
        vec![Span::styled(
            " refreshing ",
            Style::default().fg(YELLOW).add_modifier(Modifier::SLOW_BLINK),
        )]
    } else {
        vec![]
    }
}

fn separator(width: usize) -> Line<'static> {
    Line::from(Span::raw("─".repeat(width)))
}

pub const VERSION: &str = env!("CARGO_PKG_VERSION");

pub fn draw(f: &mut Frame, g: &Shared, ui: &mut UiState, busy: bool) {
    let area = f.area();
    let block = Block::bordered().border_type(BorderType::Rounded);
    let inner_w = area.width.saturating_sub(2) as usize;
    let inner_h = area.height.saturating_sub(2) as usize;
    let total = g.rows.len();
    let note = if g.note_at != 0 && now() - g.note_at < 20 {
        g.note.clone()
    } else {
        String::new()
    };

    // no data yet: centered status instead of the table
    if total == 0 {
        let status = &g.status;
        let msg_col = if status.is_empty() || status == "connecting to the box..." {
            INK_GRAY
        } else {
            RED
        };
        let mut lines: Vec<Line> = Vec::new();
        for _ in 0..inner_h / 2 {
            lines.push(Line::from(""));
        }
        let mid = vec![
            spb("STOKER", NEON_PINK),
            sp(
                format!("  {}", if status.is_empty() { "loading..." } else { status }),
                msg_col,
            ),
        ];
        let w = spans_width(&mid);
        let lp = inner_w.saturating_sub(w) / 2;
        let mut spans = vec![Span::raw(" ".repeat(lp))];
        spans.extend(mid);
        lines.push(Line::from(spans));
        f.render_widget(Paragraph::new(Text::from(lines)).block(block), area);
        return;
    }

    // CCP republishes this dataset hourly; show its age + next drop
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
        "--".to_string()
    } else {
        rel_age(&g.pulled_at)
    };

    // ---- structure detail page ([->] on a selection, [<-]/esc back) ----
    if ui.detail_mode {
        let d = g.rows.iter().find(|r| r.sid == ui.detail_sid);
        let kh = |k: &str, label: &str| -> Vec<Span<'static>> {
            vec![sp(k.to_string(), NEON_PINK), sp(format!(" {label}  "), INK_GRAY)]
        };
        let line = |label: &str, mut v: Vec<Span<'static>>| -> Line<'static> {
            let mut spans = vec![sp(format!("  {}", pad(label, 13, false)), NEON_DIM_CYAN)];
            spans.append(&mut v);
            Line::from(spans)
        };
        let dhead = line_lr(
            vec![
                sp(" ▌", NEON_PINK),
                spb("STOKER", NEON_PINK),
                sp(" » structure detail ", NEON_DIM_CYAN),
            ],
            vec![sp(format!(" {fresh} "), INK_GRAY)],
            inner_w,
        );
        let mut b: Vec<Line> = Vec::new();
        match d {
            None => b.push(Line::from(sp("  structure not in the current feed", GREY))),
            Some(d) => {
                b.push(Line::from(""));
                b.push(Line::from(vec![Span::raw("  "), spb(d.name.clone(), NEON_PINK)]));
                b.push(Line::from(vec![
                    Span::raw("  "),
                    sp(d.system.clone(), NEON_CYAN),
                    sp(format!("  {}  ", d.typ), LIGHT),
                    sp(d.state.clone(), INK_GRAY),
                ]));
                b.push(Line::from(vec![
                    Span::raw("  "),
                    spd(
                        format!(
                            "services: {}",
                            if d.services.is_empty() { "none" } else { &d.services }
                        ),
                        Color::Reset,
                    ),
                ]));
                b.push(Line::from(""));
                if d.has_fuel {
                    let ex = if d.fuel_expires.is_empty() {
                        String::new()
                    } else {
                        format!("  runs out {} UTC", cells_prefix(&d.fuel_expires, 16))
                    };
                    let mut v = vec![spb(format!("{:.1} days", d.days), days_color(d.days)), Span::raw("  ")];
                    v.extend(bar_with_text("", (d.days / GAUGE_DAYS).max(0.0), days_color(d.days), 6));
                    v.push(spd(ex, Color::Reset));
                    b.push(line("FUEL", v));
                } else {
                    b.push(line("FUEL", vec![sp("no fuel data", GREY)]));
                }
                if d.burn7 >= 0.0 || d.burn30 >= 0.0 {
                    let mut bt = String::new();
                    if d.burn7 >= 0.0 {
                        bt += &format!("x{:.2} (7d)", d.burn7);
                    }
                    if d.burn30 >= 0.0 {
                        bt += &format!("{}x{:.2} (30d)", if bt.is_empty() { "" } else { "   " }, d.burn30);
                    }
                    if d.est >= 0.0 {
                        bt += &format!("   est {:.1}d at observed burn", d.est);
                    }
                    b.push(line("BURN", vec![sp(bt, LIGHT)]));
                } else {
                    b.push(line("BURN", vec![sp("not enough history yet (~1 day needed)", GREY)]));
                }
                if d.bpd >= 0.0 {
                    b.push(line(
                        "RATE",
                        vec![sp(format!("{} blocks/day from online services", commas(d.bpd)), LIGHT)],
                    ));
                }
                if d.blocks_now >= 0.0 {
                    b.push(line(
                        "IN BAY",
                        vec![sp(
                            format!(
                                "~{} blocks ({} m3), est from fuel clock x rate",
                                commas(d.blocks_now),
                                commas(d.m3_now)
                            ),
                            GREEN,
                        )],
                    ));
                }
                if !d.fuel2_name.is_empty() {
                    let f2label = if d.fuel2_name == "Magmatic Gas" { "MAGMATIC GAS" } else { "OZONE" };
                    let why = match g.fuel2_status.as_str() {
                        "relogin" => "unknown - this login predates the corp-assets permission: alt+c and log in again",
                        "director" => "unknown - your character needs the in-game Director role",
                        "error" => "unknown - the corp-assets pull failed, retrying next poll",
                        _ => "unknown (needs a Director-role data source)",
                    };
                    b.push(line(
                        f2label,
                        if d.fuel2 < 0.0 {
                            vec![sp(why, GREY)]
                        } else {
                            let extra = if fuel2_days(d) >= 0.0 {
                                format!("  (~{}d at drill rate)", commas(fuel2_days(d)))
                            } else {
                                String::new()
                            };
                            vec![sp(format!("{} units{extra}", commas(d.fuel2)), fuel2_color(d))]
                        },
                    ));
                }
                if d.need >= 0.0 {
                    b.push(line(
                        "FUEL BLOCKS",
                        if d.need == 0.0 {
                            vec![sp("topped past 30 days", GREEN)]
                        } else {
                            vec![sp(
                                format!("haul {} blocks ({} m3)", commas(d.need), commas(d.m3)),
                                need_color(d),
                            )]
                        },
                    ));
                }
                if d.lo_min >= 0.0 {
                    b.push(line(
                        "DOCTRINE",
                        vec![sp(
                            format!("ozone min {} / fill to {}", commas(d.lo_min), commas(d.lo_target)),
                            LIGHT,
                        )],
                    ));
                }
                if d.gas_day >= 0.0 {
                    b.push(line(
                        "GAS",
                        vec![sp(
                            format!(
                                "{} magmatic/day, month haul {} units ({} m3)",
                                commas(d.gas_day),
                                commas(d.gas_month),
                                commas(d.gas_m3)
                            ),
                            LIGHT,
                        )],
                    ));
                }
                if d.has_refuel {
                    b.push(line(
                        "LAST FUELED",
                        vec![sp(
                            format!("{} (+{:.1}d)", rel_age(&d.last_refuel), d.refuel_added),
                            GREEN,
                        )],
                    ));
                }
                b.push(Line::from(""));
                b.push(Line::from(vec![
                    Span::raw("  "),
                    spb("REFUEL LOG", NEON_DIM_CYAN),
                    sp("  (this structure, newest first)", INK_GRAY),
                ]));
                if d.log.is_empty() {
                    b.push(Line::from(sp("    (no refuels seen yet)", GREY)));
                } else {
                    b.push(Line::from(sp(
                        format!(
                            "    {}{}  {}  {}",
                            pad("WHEN", 10, false),
                            pad("+DAYS", 7, true),
                            pad("BLOCKS", 10, true),
                            pad("BY", 14, false)
                        ),
                        NEON_DIM_CYAN,
                    )));
                    for ev in &d.log {
                        let blk = if d.bpd > 0.0 { commas(ev.days_added * d.bpd) } else { "--".into() };
                        b.push(Line::from(vec![
                            Span::raw("    "),
                            sp(pad(&rel_age(&ev.seen_at), 10, false), LIGHT),
                            spb(pad(&format!("+{:.1}d", ev.days_added), 7, true), GREEN),
                            Span::raw("  "),
                            sp(pad(&blk, 10, true), GREEN),
                            Span::raw("  "),
                            sp(
                                pad(if ev.by.is_empty() { "?" } else { &ev.by }, 14, false),
                                if ev.by.is_empty() { GREY } else { NEON_PINK },
                            ),
                        ]));
                    }
                }
            }
        }
        let mut keyspans: Vec<Span<'static>> = vec![Span::raw(" ")];
        keyspans.extend(kh("←/esc", "back"));
        keyspans.extend(kh("1", "I fueled it"));
        keyspans.extend(kh("r", "refresh"));
        keyspans.extend(kh("q", "quit"));
        let dkeys = line_lr(keyspans, note_or_status(&note, &g.status, busy), inner_w);

        let body_h = inner_h.saturating_sub(4); // head + 2 separators + keys
        let mut lines = vec![dhead, separator(inner_w)];
        b.truncate(body_h);
        while b.len() < body_h {
            b.push(Line::from(""));
        }
        lines.extend(b);
        lines.push(separator(inner_w));
        lines.push(dkeys);
        f.render_widget(Paragraph::new(Text::from(lines)).block(block), area);
        return;
    }

    let rows = view(&g.rows, ui);
    let types = type_list(&g.rows);
    let tsel = if ui.type_idx > 0 && ui.type_idx <= types.len() {
        types[ui.type_idx - 1].clone()
    } else {
        "All".to_string()
    };

    // clamp selection + scroll to the visible window
    let visible = std::cmp::max(3, inner_h as isize - 7) as usize;
    let name_w = (if ui.log_mode { inner_w as isize - 60 } else { inner_w as isize - 58 })
        .clamp(12, 60) as usize;
    let wide = inner_w >= 105;
    let n = if ui.log_mode { g.refuels.len() } else { rows.len() };
    if n == 0 {
        ui.sel = 0;
    } else if ui.sel >= n {
        ui.sel = n - 1;
    }
    if ui.sel < ui.offset {
        ui.offset = ui.sel;
    }
    if ui.sel >= ui.offset + visible {
        ui.offset = ui.sel + 1 - visible;
    }
    ui.offset = ui.offset.min(n.saturating_sub(visible));

    let mut under14 = 0;
    let mut under7 = 0;
    for r in &rows {
        if r.has_fuel && r.days < 14.0 {
            under14 += 1;
        }
        if r.has_fuel && r.days < 7.0 {
            under7 += 1;
        }
    }

    // header
    let head = line_lr(
        vec![
            sp(" ▌", NEON_PINK),
            spb("STOKER", NEON_PINK),
            sp(format!(" v{VERSION} "), INK_GRAY),
            sp(
                if ui.log_mode {
                    "» refuel log ".to_string()
                } else {
                    format!(
                        "» {}fuel watch ",
                        if g.corp_name.is_empty() { String::new() } else { format!("{} ", g.corp_name) }
                    )
                },
                NEON_DIM_CYAN,
            ),
            sp("[corp] ", INK_GRAY),
            sp(format!("{n}/{total}"), INK_GRAY),
        ],
        vec![
            sp("under14d ", INK_GRAY),
            spb(under14.to_string(), if under14 > 0 { YELLOW } else { GREEN }),
            sp("  under7d ", INK_GRAY),
            spb(under7.to_string(), if under7 > 0 { RED } else { GREEN }),
            sp(if wide { format!("   {fresh} ") } else { " ".into() }, INK_GRAY),
        ],
        inner_w,
    );

    // tally line: totals over the [space]-marked rows, or everything visible
    let mut tally: Vec<Span<'static>> = Vec::new();
    if !ui.log_mode {
        let (mut blk, mut gas, mut oz, mut goo, mut goo_isk) = (0.0, 0.0, 0.0, 0.0, 0.0);
        let mut any_isk = false;
        let mut cnt = 0;
        for r in &rows {
            if !ui.marked.is_empty() && !ui.marked.contains(&r.sid) {
                continue;
            }
            cnt += 1;
            if r.blocks_now >= 0.0 {
                blk += r.blocks_now;
            }
            if r.fuel2 >= 0.0 {
                if r.fuel2_name == "Magmatic Gas" {
                    gas += r.fuel2;
                } else if r.fuel2_name == "Liquid Ozone" {
                    oz += r.fuel2;
                }
            }
            if r.goo_m3 >= 0.0 {
                goo += r.goo_m3;
            }
            if r.goo_isk >= 0.0 {
                goo_isk += r.goo_isk;
                any_isk = true;
            }
        }
        tally.push(if ui.marked.is_empty() {
            sp(format!("total {cnt}"), INK_GRAY)
        } else {
            spb(format!("tally {cnt}"), NEON_CYAN)
        });
        tally.push(sp("  fuel ", INK_GRAY));
        tally.push(sp(format!("{}/{} m3", commas(blk), commas(blk * 5.0)), GREEN));
        if gas > 0.0 {
            tally.push(sp("  gas ", INK_GRAY));
            tally.push(sp(format!("{}/{} m3", commas(gas), commas(gas * 0.01)), NEON_PINK));
        }
        if oz > 0.0 {
            tally.push(sp("  ozone ", INK_GRAY));
            tally.push(sp(format!("{}/{} m3", commas(oz), commas(oz * 0.4)), Color::Rgb(120, 190, 255)));
        }
        if goo > 0.0 {
            tally.push(sp("  goo ", INK_GRAY));
            tally.push(sp(
                format!(
                    "{} m3{}",
                    commas(goo),
                    if any_isk { format!(" ({})", isk_compact(goo_isk)) } else { String::new() }
                ),
                NEON_CYAN,
            ));
        }
        tally.push(Span::raw(" "));
    }
    let mut right = tally;
    right.extend(note_or_status(&note, &g.status, busy));
    let filt = line_lr(
        vec![
            sp(" type ", INK_GRAY),
            sp(tsel, NEON_CYAN),
            sp("  sort ", INK_GRAY),
            sp(SORTN[ui.sort_mode], NEON_CYAN),
            sp("  filter ", INK_GRAY),
            if ui.filter_mode {
                sp(format!("{}_", ui.text_filter), YELLOW)
            } else if ui.text_filter.is_empty() {
                sp("none", GREY)
            } else {
                sp(ui.text_filter.clone(), GREEN)
            },
        ],
        right,
        inner_w,
    );

    // column header + body + detail (structures or refuel log)
    let colhead: Line;
    let mut body: Vec<Line> = Vec::new();
    let mut detail: Line = Line::from("");
    if ui.log_mode {
        colhead = Line::from(vec![
            sp(pad(" WHEN", 10, false), NEON_DIM_CYAN),
            sp(pad("+DAYS", 7, true), NEON_DIM_CYAN),
            sp("  ", NEON_DIM_CYAN),
            sp(pad("BLOCKS", 12, true), NEON_DIM_CYAN),
            sp("  ", NEON_DIM_CYAN),
            sp(pad("BY", 14, false), NEON_DIM_CYAN),
            sp(pad("SYSTEM", 9, false), NEON_DIM_CYAN),
            sp("NAME", NEON_DIM_CYAN),
        ]);
        for i in ui.offset..n.min(ui.offset + visible) {
            let v = &g.refuels[i];
            let blk = if v.pending {
                "awaiting ESI".to_string()
            } else if v.blocks >= 0.0 {
                commas(v.blocks)
            } else {
                "--".into()
            };
            let blkc = if v.pending {
                YELLOW
            } else if v.blocks >= 0.0 {
                GREEN
            } else {
                GREY
            };
            let mut spans = vec![
                sp(pad(&format!(" {}", rel_age(&v.seen_at)), 10, false), LIGHT),
                if v.pending {
                    sp(pad("...", 7, true), YELLOW)
                } else {
                    spb(pad(&format!("+{:.1}d", v.days_added), 7, true), GREEN)
                },
                Span::raw("  "),
                sp(pad(&blk, 12, true), blkc),
                Span::raw("  "),
                sp(
                    pad(if v.by.is_empty() { "?" } else { &v.by }, 14, false),
                    if v.by.is_empty() { GREY } else { NEON_PINK },
                ),
                sp(pad(&v.system, 9, false), NEON_CYAN),
                Span::raw(pad(&v.name, name_w, false)),
            ];
            if i == ui.sel {
                for s in spans.iter_mut() {
                    s.style = s.style.add_modifier(Modifier::REVERSED);
                }
            }
            body.push(Line::from(spans));
        }
        if n == 0 {
            body.push(Line::from(sp(
                "  (no refuels seen yet - the log fills in as polls catch fuel jumps)",
                GREY,
            )));
        }
        if ui.sel < n {
            let v = &g.refuels[ui.sel];
            if v.pending {
                detail = Line::from(vec![
                    sp(" > ", NEON_PINK),
                    spb(v.name.clone(), Color::Reset),
                    sp(format!("  {}  ", v.system), NEON_CYAN),
                    spd("claim filed - stamps the next fuel jump ESI shows here", Color::Reset),
                ]);
            } else {
                let ex = if v.new_expires.is_empty() {
                    "?".to_string()
                } else {
                    format!("{} UTC", cells_prefix(&v.new_expires, 16))
                };
                let who = if v.by.is_empty() {
                    "unclaimed - press 1 = I fueled it".to_string()
                } else {
                    format!("fueled by {}", v.by)
                };
                detail = Line::from(vec![
                    sp(" > ", NEON_PINK),
                    spb(v.name.clone(), Color::Reset),
                    sp(format!("  {}  ", v.system), NEON_CYAN),
                    spd(format!("fuel now runs to {ex}  "), Color::Reset),
                    sp(who, if v.by.is_empty() { YELLOW } else { GREEN }),
                ]);
            }
        }
    } else {
        colhead = Line::from(vec![
            sp(" ", NEON_DIM_CYAN),
            sp("■", NEON_CYAN),
            sp(pad(" Blocks", 9, false), NEON_DIM_CYAN),
            sp("30d", INK_GRAY),
            sp(" ", NEON_DIM_CYAN),
            sp("●", Color::Rgb(235, 90, 60)),
            sp("◆", Color::Rgb(120, 190, 255)),
            sp(pad(" Gas/Oz", 8, false), NEON_DIM_CYAN),
            sp("30d", INK_GRAY),
            sp("  ", NEON_DIM_CYAN),
            sp(pad("TYPE", 12, false), NEON_DIM_CYAN),
            sp("  ", NEON_DIM_CYAN),
            sp(pad("SYSTEM", 9, false), NEON_DIM_CYAN),
            sp("NAME", NEON_DIM_CYAN),
        ]);
        for i in ui.offset..n.min(ui.offset + visible) {
            let r = &rows[i];
            let mut rest = vec![
                sp(pad(&r.typ, 12, false), LIGHT),
                Span::raw("  "),
                sp(pad(&r.system, 9, false), NEON_CYAN),
                Span::raw(pad(&r.name, name_w, false)),
            ];
            if i == ui.sel {
                for s in rest.iter_mut() {
                    s.style = s.style.add_modifier(Modifier::REVERSED);
                }
            }
            let mk = ui.marked.contains(&r.sid);
            let mut spans = fuel_cell(r, 14);
            spans.push(if i == ui.sel {
                spb(">", NEON_PINK)
            } else if mk {
                spb("*", NEON_CYAN)
            } else {
                Span::raw(" ")
            });
            spans.extend(fuel2_cell(r, 14));
            spans.push(Span::raw("  "));
            spans.extend(rest);
            body.push(Line::from(spans));
        }
        if n == 0 {
            body.push(Line::from(sp("  (no structures match)", GREY)));
        }
        if ui.sel < n {
            let r = &rows[ui.sel];
            let svc = if r.services.is_empty() { "none" } else { &r.services };
            let ex = if r.fuel_expires.is_empty() {
                "no fuel data".to_string()
            } else {
                format!("fuel out {} UTC", cells_prefix(&r.fuel_expires, 16))
            };
            let mut fueled = String::new();
            if r.has_refuel {
                fueled += &format!("  fueled {} (+{:.1}d)", rel_age(&r.last_refuel), r.refuel_added);
            }
            if r.burn7 >= 0.0 || r.burn30 >= 0.0 {
                if r.burn7 >= 0.0 && r.burn30 >= 0.0 {
                    fueled += &format!("  burn x{:.2} 7d / x{:.2} 30d", r.burn7, r.burn30);
                } else {
                    fueled += &format!("  burn x{:.2}", burn_of(r));
                }
                if r.est >= 0.0 {
                    fueled += &format!("  est {:.1}d at that rate", r.est);
                }
            }
            detail = Line::from(vec![
                sp(" > ", NEON_PINK),
                spb(r.name.clone(), Color::Reset),
                sp(format!("  {}  ", r.system), NEON_CYAN),
                sp(format!("{}  ", r.state), GREY),
                spd(format!("{ex}  services: {svc}"), Color::Reset),
                sp(fueled, GREEN),
            ]);
        }
    }

    // width-responsive footer: full labels when they fit, short ones when
    // not, keys alone when even those don't
    struct Hint(&'static str, &'static str, &'static str, bool);
    let hints = [
        Hint("tab", "type", "type", true),
        Hint("s", "sort", "sort", true),
        Hint("/", "filter", "filt", true),
        Hint("spc", "tally", "tly", !ui.log_mode),
        Hint("f", "refuel log", "log", true),
        Hint("→", "details", "info", true),
        Hint("1", "I fueled it", "claim", true),
        Hint("r", "refresh", "rfsh", true),
        Hint("q", "quit", "quit", true),
    ];
    let hints_width = |mini: bool, labels: bool| -> usize {
        let mut w = 1;
        for h in &hints {
            if !h.3 {
                continue;
            }
            w += cells(h.0) + if labels { cells(if mini { h.2 } else { h.1 }) + 1 } else { 0 } + 2;
        }
        w
    };
    let full_fit = hints_width(false, true) <= inner_w.saturating_sub(2);
    let mini_fit = hints_width(true, true) <= inner_w.saturating_sub(2);
    let mut ke: Vec<Span<'static>> = vec![Span::raw(" ")];
    for h in &hints {
        if !h.3 {
            continue;
        }
        let label = if full_fit { h.1 } else if mini_fit { h.2 } else { "" };
        ke.push(sp(h.0, NEON_PINK));
        ke.push(sp(format!(" {label}  "), INK_GRAY));
    }
    let expl = if ui.log_mode {
        "BLOCKS = deposit estimated from the fuel clock jump "
    } else {
        "gauges: days left | haul to 30d on the right (? = needs a Director token) "
    };
    let keys = if full_fit && inner_w.saturating_sub(hints_width(false, true)) > cells(expl) + 2 {
        line_lr(ke, vec![sp(expl, INK_GRAY)], inner_w)
    } else {
        Line::from(ke)
    };

    let mut lines = vec![head, filt, separator(inner_w), colhead];
    body.truncate(visible);
    while body.len() < visible {
        body.push(Line::from(""));
    }
    lines.extend(body);
    lines.push(separator(inner_w));
    lines.push(detail);
    lines.push(keys);
    f.render_widget(Paragraph::new(Text::from(lines)).block(block), area);
}
