// STOKER, Rust edition: live fuel-watch dashboard for Brave Pos Boys' Upwell
// structures. Port of bpos-dash.cpp (corp mode + the full TUI); standalone
// SSO mode and the GUI build are not in this port yet.
//
// Corp mode fetches the hosted STOKER backend (key-gated JSON endpoint): a
// cached snapshot refreshed server-side by cron. A background thread
// re-fetches every 30s; 'r' kicks an ASYNC live ESI pull on the box.
//
// Build: cargo build --release   Run: target/release/stoker

mod data;
mod ui;
mod util;

use crossterm::event::{
    self, Event, KeyCode, KeyEventKind, KeyModifiers, MouseEventKind,
};
use crossterm::{execute, terminal};
use data::{ingest, Shared};
use ratatui::prelude::*;
use std::collections::HashSet;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use ui::UiState;
use util::now;

const REFRESH_SECONDS: u64 = 30; // corp endpoint poll

struct App {
    shared: Mutex<Shared>,
    run: AtomicBool,
    busy: AtomicBool,
    fetch_url: String,
    refresh_url: String,
    claim_url: String,
}

fn config_dir() -> PathBuf {
    #[cfg(windows)]
    {
        PathBuf::from(std::env::var("APPDATA").unwrap_or_else(|_| ".".into())).join("stoker")
    }
    #[cfg(not(windows))]
    {
        let base = std::env::var("XDG_CONFIG_HOME")
            .ok()
            .filter(|s| !s.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| ".".into())).join(".config")
            });
        base.join("stoker")
    }
}

fn urlenc(s: &str) -> String {
    let mut o = String::new();
    for b in s.bytes() {
        match b {
            b'0'..=b'9' | b'a'..=b'z' | b'A'..=b'Z' | b'-' | b'_' | b'.' | b'~' => o.push(b as char),
            _ => o.push_str(&format!("%{b:02X}")),
        }
    }
    o
}

fn agent() -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout(Duration::from_secs(60))
        .user_agent(concat!("stoker-rs/", env!("CARGO_PKG_VERSION")))
        .build()
}

fn http_get_body(url: &str) -> String {
    match agent().get(url).call() {
        Ok(r) => {
            let mut body = String::new();
            use std::io::Read;
            let _ = r.into_reader().read_to_string(&mut body);
            body
        }
        Err(_) => String::new(),
    }
}

fn http_post_form(url: &str) -> String {
    match agent()
        .post(url)
        .set("Content-Type", "application/x-www-form-urlencoded")
        .send_string("")
    {
        Ok(r) => {
            let mut body = String::new();
            use std::io::Read;
            let _ = r.into_reader().read_to_string(&mut body);
            body
        }
        Err(ureq::Error::Status(_, r)) => {
            let mut body = String::new();
            use std::io::Read;
            let _ = r.into_reader().read_to_string(&mut body);
            body
        }
        Err(_) => String::new(),
    }
}

// Resolve the data source: env vars > config file. Corp mode only in the
// Rust port; standalone SSO mode still lives in the C++ build.
fn load_config() -> Result<(String, String), String> {
    let cfg_path = config_dir().join("config.json");
    let cfg: serde_json::Value = std::fs::read_to_string(&cfg_path)
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or(serde_json::Value::Null);
    let get = |k: &str| -> String {
        cfg.get(k)
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string()
    };
    let mut endpoint = get("endpoint");
    let mut key = get("key");
    if let Ok(e) = std::env::var("STOKER_ENDPOINT") {
        if !e.is_empty() {
            endpoint = e;
        }
    }
    if let Ok(k) = std::env::var("STOKER_KEY") {
        if !k.is_empty() {
            key = k;
        }
    }
    if key.is_empty() {
        return Err(format!(
            "no corp access key found.\n\n\
             The Rust port runs corp mode only (standalone EVE-SSO mode is still\n\
             C++-build territory). Put an \"endpoint\" and \"key\" in {},\n\
             or set STOKER_ENDPOINT / STOKER_KEY.",
            cfg_path.display()
        ));
    }
    if endpoint.is_empty() {
        return Err(format!("corp mode needs an \"endpoint\" in {}", cfg_path.display()));
    }
    Ok((endpoint, key))
}

fn set_note(app: &App, note: String) {
    let mut g = app.shared.lock().unwrap();
    g.note = note;
    g.note_at = now();
}

fn force_refresh(app: &Arc<App>) {
    if app.busy.swap(true, Ordering::SeqCst) {
        return;
    }
    app.shared.lock().unwrap().status = "kicking a live ESI pull on the box...".into();
    let app = app.clone();
    std::thread::spawn(move || {
        ingest(&app.shared, &http_get_body(&app.refresh_url));
        app.busy.store(false, Ordering::SeqCst);
    });
}

// [1] claim: "I fueled this". With seen_at it stamps that exact logged
// event; without, the server stamps the newest recent refuel on the
// structure or parks a pending claim until ESI shows the jump.
fn send_claim(app: &Arc<App>, sid: i64, seen_at: String) {
    if sid == 0 || app.busy.swap(true, Ordering::SeqCst) {
        return;
    }
    let who = std::env::var("STOKER_NAME").unwrap_or_default();
    app.shared.lock().unwrap().status = "filing claim...".into();
    let app = app.clone();
    std::thread::spawn(move || {
        let mut url = format!("{}&structure_id={}&by={}", app.claim_url, sid, urlenc(&who));
        if !seen_at.is_empty() {
            url += &format!("&seen_at={}", urlenc(&seen_at));
        }
        let res = http_post_form(&url);
        let mut note = "claim failed: no reply from the box".to_string();
        if let Ok(j) = serde_json::from_str::<serde_json::Value>(&res) {
            if j.get("ok").and_then(|v| v.as_bool()).unwrap_or(false) {
                let stamped = j.get("status").and_then(|v| v.as_str()) == Some("stamped");
                note = format!(
                    "{}{}  fueled by {}{}",
                    if stamped { "stamped: " } else { "claim pending: " },
                    j.get("structure").and_then(|v| v.as_str()).unwrap_or("?"),
                    who,
                    if stamped { "" } else { "  (lands when ESI shows the jump, up to ~2h)" }
                );
            } else {
                note = format!(
                    "claim rejected: {}",
                    j.get("error").and_then(|v| v.as_str()).unwrap_or("?")
                );
            }
        }
        {
            let mut g = app.shared.lock().unwrap();
            g.note = note;
            g.note_at = now();
            g.status.clear();
        }
        ingest(&app.shared, &http_get_body(&app.fetch_url));
        app.busy.store(false, Ordering::SeqCst);
    });
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() > 1 && args[1] == "--version" {
        println!("STOKER v{} (rust)", env!("CARGO_PKG_VERSION"));
        return;
    }

    let (endpoint, key) = match load_config() {
        Ok(v) => v,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    };
    let url = format!("{endpoint}?k={}", urlenc(&key));
    let app = Arc::new(App {
        shared: Mutex::new(Shared {
            status: "connecting to the box...".into(),
            ..Default::default()
        }),
        run: AtomicBool::new(true),
        busy: AtomicBool::new(false),
        fetch_url: url.clone(),
        refresh_url: format!("{url}&refresh=1"),
        claim_url: format!("{endpoint}/claim?k={}", urlenc(&key)),
    });

    // --dump: one fetch cycle to stdout, no TUI (debugging / scripting)
    if args.len() > 1 && args[1] == "--dump" {
        ingest(&app.shared, &http_get_body(&app.fetch_url));
        let g = app.shared.lock().unwrap();
        println!("corp endpoint: {} structures", g.rows.len());
        if !g.status.is_empty() {
            println!("status: {}", g.status);
        }
        return;
    }

    // background poll loop
    {
        let app = app.clone();
        std::thread::spawn(move || {
            while app.run.load(Ordering::SeqCst) {
                ingest(&app.shared, &http_get_body(&app.fetch_url));
                for _ in 0..REFRESH_SECONDS * 4 {
                    if !app.run.load(Ordering::SeqCst) {
                        return;
                    }
                    std::thread::sleep(Duration::from_millis(250));
                }
            }
        });
    }

    // terminal up
    terminal::enable_raw_mode().expect("raw mode");
    let mut stdout = std::io::stdout();
    execute!(
        stdout,
        terminal::EnterAlternateScreen,
        event::EnableMouseCapture,
        crossterm::cursor::Hide
    )
    .expect("terminal setup");
    let mut term = Terminal::new(CrosstermBackend::new(stdout)).expect("terminal");

    let mut st = UiState {
        sort_mode: 0,
        type_idx: 0,
        text_filter: String::new(),
        filter_mode: false,
        log_mode: false,
        detail_mode: false,
        detail_sid: 0,
        offset: 0,
        sel: 0,
        marked: HashSet::new(),
    };

    while app.run.load(Ordering::SeqCst) {
        {
            let g = app.shared.lock().unwrap();
            let busy = app.busy.load(Ordering::SeqCst);
            term.draw(|f| ui::draw(f, &g, &mut st, busy)).ok();
        }

        if !event::poll(Duration::from_millis(100)).unwrap_or(false) {
            continue;
        }
        let ev = match event::read() {
            Ok(e) => e,
            Err(_) => break,
        };

        // current view size + selection target, recomputed per event
        let (n, sel_sid, sel_seen_at) = {
            let g = app.shared.lock().unwrap();
            if st.log_mode {
                let n = g.refuels.len();
                let (sid, seen) = if st.sel < n && !g.refuels[st.sel].pending {
                    (g.refuels[st.sel].sid, g.refuels[st.sel].seen_at.clone())
                } else {
                    (0, String::new())
                };
                (n, sid, seen)
            } else {
                let rows = ui::view(&g.rows, &st);
                let sid = if st.sel < rows.len() { rows[st.sel].sid } else { 0 };
                (rows.len(), sid, String::new())
            }
        };

        match ev {
            Event::Mouse(m) => match m.kind {
                MouseEventKind::ScrollDown => {
                    if st.sel + 1 < n {
                        st.sel += 1;
                    }
                }
                MouseEventKind::ScrollUp => {
                    st.sel = st.sel.saturating_sub(1);
                }
                _ => {}
            },
            Event::Key(k) if k.kind == KeyEventKind::Press => {
                let ctrl_c = k.modifiers.contains(KeyModifiers::CONTROL)
                    && k.code == KeyCode::Char('c');
                let alt_c =
                    k.modifiers.contains(KeyModifiers::ALT) && k.code == KeyCode::Char('c');
                if ctrl_c {
                    break;
                }
                if alt_c {
                    set_note(
                        &app,
                        "character logins are standalone-mode only (this is corp mode)".into(),
                    );
                    continue;
                }

                if st.filter_mode {
                    match k.code {
                        KeyCode::Enter => st.filter_mode = false,
                        KeyCode::Esc => {
                            st.text_filter.clear();
                            st.filter_mode = false;
                        }
                        KeyCode::Backspace => {
                            st.text_filter.pop();
                        }
                        KeyCode::Char(c) => {
                            st.text_filter.push(c);
                            st.sel = 0;
                            st.offset = 0;
                        }
                        _ => {}
                    }
                    continue;
                }

                if st.detail_mode {
                    match k.code {
                        KeyCode::Left | KeyCode::Esc => st.detail_mode = false,
                        KeyCode::Char('q') => break,
                        KeyCode::Char('r') => force_refresh(&app),
                        KeyCode::Char('1') => send_claim(&app, st.detail_sid, String::new()),
                        _ => {} // swallow list-view keys while on the detail page
                    }
                    continue;
                }

                match k.code {
                    KeyCode::Char('q') => break,
                    KeyCode::Esc => {
                        if st.log_mode {
                            st.log_mode = false;
                            st.sel = 0;
                            st.offset = 0;
                        } else if !st.marked.is_empty() {
                            st.marked.clear(); // drop tally first
                        } else {
                            break;
                        }
                    }
                    KeyCode::Char('f') => {
                        st.log_mode = !st.log_mode;
                        st.sel = 0;
                        st.offset = 0;
                    }
                    KeyCode::Right => {
                        if sel_sid != 0 {
                            st.detail_mode = true;
                            st.detail_sid = sel_sid;
                        }
                    }
                    KeyCode::Char('1') => {
                        if sel_sid != 0 {
                            send_claim(&app, sel_sid, sel_seen_at);
                        }
                    }
                    KeyCode::Char(' ') => {
                        // toggle tally mark on the highlighted row
                        if !st.log_mode && sel_sid != 0 && !st.marked.remove(&sel_sid) {
                            st.marked.insert(sel_sid);
                        }
                    }
                    KeyCode::Char('/') => st.filter_mode = true,
                    KeyCode::Char('u') => {
                        set_note(&app, "self-update isn't in the Rust build yet".into())
                    }
                    KeyCode::Char('c') => set_note(
                        &app,
                        "corp tabs are standalone-mode only (this is corp mode)".into(),
                    ),
                    KeyCode::Char('s') => {
                        st.sort_mode = (st.sort_mode + 1) % 5;
                        st.sel = 0;
                        st.offset = 0;
                    }
                    KeyCode::Char('r') => force_refresh(&app),
                    KeyCode::Tab => {
                        let tc = {
                            let g = app.shared.lock().unwrap();
                            ui::type_list(&g.rows).len()
                        };
                        st.type_idx = (st.type_idx + 1) % (tc + 1);
                        st.sel = 0;
                        st.offset = 0;
                    }
                    KeyCode::Down | KeyCode::Char('j') => {
                        if st.sel + 1 < n {
                            st.sel += 1;
                        }
                    }
                    KeyCode::Up | KeyCode::Char('k') => st.sel = st.sel.saturating_sub(1),
                    KeyCode::PageDown => st.sel = (st.sel + 15).min(n.saturating_sub(1)),
                    KeyCode::PageUp => st.sel = st.sel.saturating_sub(15),
                    KeyCode::Home => st.sel = 0,
                    KeyCode::End => st.sel = n.saturating_sub(1),
                    _ => {}
                }
            }
            _ => {}
        }
    }

    app.run.store(false, Ordering::SeqCst);
    let mut stdout = std::io::stdout();
    execute!(
        stdout,
        event::DisableMouseCapture,
        terminal::LeaveAlternateScreen,
        crossterm::cursor::Show
    )
    .ok();
    terminal::disable_raw_mode().ok();
}
