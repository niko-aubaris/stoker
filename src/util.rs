// small formatting + time helpers shared by data and ui (port of the C++
// helpers at the top of bpos-dash.cpp)

use std::time::{SystemTime, UNIX_EPOCH};

// display cells = Unicode code points, same yardstick the C++ used
pub fn cells(s: &str) -> usize {
    s.chars().count()
}

pub fn cells_prefix(s: &str, n: usize) -> String {
    s.chars().take(n).collect()
}

pub fn pad(s: &str, w: usize, right: bool) -> String {
    let c = cells(s);
    if c >= w {
        return cells_prefix(s, w);
    }
    let p = " ".repeat(w - c);
    if right {
        format!("{p}{s}")
    } else {
        format!("{s}{p}")
    }
}

// left-anchored + right-anchored text composed to exactly `width` cells
pub fn two_sided(l: &str, r: &str, width: usize) -> String {
    let (cl, cr) = (cells(l), cells(r));
    if cl + cr + 1 > width {
        return pad(l, width, false); // no room: keep the left value
    }
    format!("{l}{}{r}", " ".repeat(width - cl - cr))
}

pub fn commas(v: f64) -> String {
    let s = format!("{:.0}", v);
    let (sign, digits) = s.strip_prefix('-').map_or(("", s.as_str()), |d| ("-", d));
    let mut o = String::new();
    let lead = digits.len() % 3;
    for (i, ch) in digits.chars().enumerate() {
        if i > 0 && (i + 3 - lead) % 3 == 0 {
            o.push(',');
        }
        o.push(ch);
    }
    format!("{sign}{o}")
}

// ISK shorthand for inside the gauges: 1.24b / 830.5m / 12k
pub fn isk_compact(v: f64) -> String {
    if v >= 1e9 {
        format!("{:.2}b", v / 1e9)
    } else if v >= 1e6 {
        format!("{:.1}m", v / 1e6)
    } else if v >= 1e3 {
        format!("{:.0}k", v / 1e3)
    } else {
        format!("{:.0}", v)
    }
}

// 1,234,567 -> "1.2M"; 14,900 -> "14.9k"; keeps tiny numbers plain
pub fn compact_units(v: f64) -> String {
    if v >= 1e6 {
        format!("{:.1}M", v / 1e6)
    } else if v >= 1e4 {
        format!("{:.1}k", v / 1e3)
    } else {
        format!("{:.0}", v)
    }
}

pub fn now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

// days since the epoch for a civil date (Howard Hinnant's algorithm)
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

// Parse "2026-07-08T05:00:23..." to unix seconds (UTC), ignoring
// fractional/offset. 0 = not a date; callers treat 0 as unknown.
pub fn parse_iso(s: &str) -> i64 {
    if s.len() < 19 {
        return 0;
    }
    let num = |a: usize, b: usize| -> Option<i64> { s.get(a..b)?.parse().ok() };
    let (y, mo, d, h, mi, se) = match (
        num(0, 4),
        num(5, 7),
        num(8, 10),
        num(11, 13),
        num(14, 16),
        num(17, 19),
    ) {
        (Some(y), Some(mo), Some(d), Some(h), Some(mi), Some(se)) => (y, mo, d, h, mi, se),
        _ => return 0,
    };
    days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se
}

pub fn rel_age(iso: &str) -> String {
    let t = parse_iso(iso);
    if t == 0 {
        return "?".into();
    }
    let secs = (now() - t).max(0);
    if secs < 90 {
        return format!("{secs}s ago");
    }
    if secs < 3600 {
        return format!("{}m ago", secs / 60);
    }
    if secs < 86400 {
        let (h, m) = (secs / 3600, (secs % 3600) / 60);
        return format!("{h}h{} ago", if m > 0 { format!("{m}m") } else { String::new() });
    }
    let (d, h) = (secs / 86400, (secs % 86400) / 3600);
    format!("{d}d{} ago", if h > 0 { format!("{h}h") } else { String::new() })
}
