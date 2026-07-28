// data-source plumbing shared by the TUI and GUI: config resolution, the
// key-gated corp endpoint, the background poll loop, refresh kicks and
// refuel claims. (Port of the corp-mode halves of load_or_setup() and the
// worker/claim lambdas in bpos-dash.cpp.)

use crate::data::{ingest, Shared};
use crate::util::now;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub const REFRESH_SECONDS: u64 = 30; // corp endpoint poll

pub struct App {
    pub shared: Mutex<Shared>,
    pub run: AtomicBool,
    pub busy: AtomicBool,
    pub fetch_url: String,
    pub refresh_url: String,
    pub claim_url: String,
}

pub fn config_dir() -> PathBuf {
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

pub fn urlenc(s: &str) -> String {
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

pub fn http_get_body(url: &str) -> String {
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

pub fn http_post_form(url: &str) -> String {
    let take = |r: ureq::Response| {
        let mut body = String::new();
        use std::io::Read;
        let _ = r.into_reader().read_to_string(&mut body);
        body
    };
    match agent()
        .post(url)
        .set("Content-Type", "application/x-www-form-urlencoded")
        .send_string("")
    {
        Ok(r) => take(r),
        Err(ureq::Error::Status(_, r)) => take(r),
        Err(_) => String::new(),
    }
}

// Resolve the data source: env vars > config file. Corp mode only in the
// Rust port; standalone SSO mode still lives in the C++ build.
pub fn load_config() -> Result<(String, String), String> {
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

pub fn make_app() -> Result<Arc<App>, String> {
    let (endpoint, key) = load_config()?;
    let url = format!("{endpoint}?k={}", urlenc(&key));
    Ok(Arc::new(App {
        shared: Mutex::new(Shared {
            status: "connecting to the box...".into(),
            ..Default::default()
        }),
        run: AtomicBool::new(true),
        busy: AtomicBool::new(false),
        fetch_url: url.clone(),
        refresh_url: format!("{url}&refresh=1"),
        claim_url: format!("{endpoint}/claim?k={}", urlenc(&key)),
    }))
}

pub fn set_note(app: &App, note: String) {
    let mut g = app.shared.lock().unwrap();
    g.note = note;
    g.note_at = now();
}

// background poll loop; `wake` pokes the UI when fresh data lands
pub fn spawn_poller(app: &Arc<App>, wake: impl Fn() + Send + 'static) {
    let app = app.clone();
    std::thread::spawn(move || {
        while app.run.load(Ordering::SeqCst) {
            ingest(&app.shared, &http_get_body(&app.fetch_url));
            wake();
            for _ in 0..REFRESH_SECONDS * 4 {
                if !app.run.load(Ordering::SeqCst) {
                    return;
                }
                std::thread::sleep(Duration::from_millis(250));
            }
        }
    });
}

pub fn force_refresh(app: &Arc<App>, wake: impl Fn() + Send + 'static) {
    if app.busy.swap(true, Ordering::SeqCst) {
        return;
    }
    app.shared.lock().unwrap().status = "kicking a live ESI pull on the box...".into();
    let app = app.clone();
    std::thread::spawn(move || {
        ingest(&app.shared, &http_get_body(&app.refresh_url));
        app.busy.store(false, Ordering::SeqCst);
        wake();
    });
}

// [1] claim: "I fueled this". With seen_at it stamps that exact logged
// event; without, the server stamps the newest recent refuel on the
// structure or parks a pending claim until ESI shows the jump.
pub fn send_claim(app: &Arc<App>, sid: i64, seen_at: String, wake: impl Fn() + Send + 'static) {
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
        wake();
    });
}
