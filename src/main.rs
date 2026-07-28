// STOKER, Rust edition (terminal build): live fuel-watch dashboard for
// Brave Pos Boys' Upwell structures. Port of bpos-dash.cpp corp mode + the
// full TUI; the native-window build is src/bin/stoker-gui.rs, standalone
// SSO mode is not in this port yet.
//
// Build: cargo build --release   Run: target/release/stoker

mod ui;

use crossterm::event::{self, Event, KeyCode, KeyEventKind, KeyModifiers, MouseEventKind};
use crossterm::{execute, terminal};
use ratatui::prelude::*;
use std::collections::HashSet;
use std::sync::atomic::Ordering;
use std::time::Duration;
use stoker::data::ingest;
use stoker::net;
use ui::UiState;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() > 1 && args[1] == "--version" {
        println!("STOKER v{} (rust)", env!("CARGO_PKG_VERSION"));
        return;
    }

    let app = match net::make_app() {
        Ok(v) => v,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    };

    // --dump: one fetch cycle to stdout, no TUI (debugging / scripting)
    if args.len() > 1 && args[1] == "--dump" {
        ingest(&app.shared, &net::http_get_body(&app.fetch_url));
        let g = app.shared.lock().unwrap();
        println!("corp endpoint: {} structures", g.rows.len());
        if !g.status.is_empty() {
            println!("status: {}", g.status);
        }
        return;
    }

    net::spawn_poller(&app, || {});

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
                    net::set_note(
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
                        KeyCode::Char('r') => net::force_refresh(&app, || {}),
                        KeyCode::Char('1') => {
                            net::send_claim(&app, st.detail_sid, String::new(), || {})
                        }
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
                            net::send_claim(&app, sel_sid, sel_seen_at, || {});
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
                        net::set_note(&app, "self-update isn't in the Rust build yet".into())
                    }
                    KeyCode::Char('c') => net::set_note(
                        &app,
                        "corp tabs are standalone-mode only (this is corp mode)".into(),
                    ),
                    KeyCode::Char('s') => {
                        st.sort_mode = (st.sort_mode + 1) % 5;
                        st.sel = 0;
                        st.offset = 0;
                    }
                    KeyCode::Char('r') => net::force_refresh(&app, || {}),
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
