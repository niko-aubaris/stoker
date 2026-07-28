// feed model + ingest: a faithful port of the C++ Row/Refuel/Notif structs
// and ingest() from bpos-dash.cpp, on serde_json instead of nlohmann

use crate::util::{commas, compact_units, parse_iso, now};
use serde_json::Value;

#[derive(Clone, Default)]
pub struct RefuelEvent {
    pub seen_at: String,
    pub by: String,
    pub days_added: f64,
}

// several fields ride along un-rendered for now (the C++ GUI uses them);
// kept so the ingest stays a full-fidelity port of the wire format
#[allow(dead_code)]
#[derive(Clone, Default)]
pub struct GooItem {
    pub name: String,
    pub qty: f64,
    pub m3: f64,
    pub isk: f64,
}

#[derive(Clone)]
pub struct Row {
    pub sid: i64,
    pub name: String,
    pub system: String,
    pub typ: String,
    pub state: String,
    pub services: String,
    pub fuel_expires: String,
    pub last_refuel: String,
    pub has_fuel: bool,
    pub has_refuel: bool,
    pub days: f64,
    pub refuel_added: f64,
    pub burn7: f64,   // fuel-days/day; -1 = not enough history
    pub burn30: f64,
    pub est: f64,     // usage-adjusted days left; -1 = unknown
    pub need: f64,    // blocks to top up to 30d; -1 = rate unknown
    pub m3: f64,
    pub bpd: f64,     // blocks/day service burn; -1 = unknown
    pub blocks_now: f64,
    pub m3_now: f64,
    pub lo_min: f64,      // Ansiblex ozone doctrine tier
    pub lo_target: f64,
    pub gas_day: f64,     // Metenox magmatic
    pub gas_month: f64,
    pub gas_m3: f64,
    pub fuel2_name: String, // secondary fuel kind (gas/ozone), "" = none
    pub fuel2: f64,         // stock units; -1 = source can't see the bay
    pub rental: String,     // moon-rental class: corp|private|unknown, "" = n/a
    pub renter: String,
    pub goo_m3: f64,
    pub goo_cap: f64,
    pub goo_isk: f64,
    pub has_econ: bool,
    pub econ_goo: f64,
    pub econ_rent: f64,
    pub econ_fuel: f64,
    pub econ_gas: f64,
    pub econ_net: f64,
    pub goo: Vec<GooItem>,
    pub state_timer_start: String,
    pub state_timer_end: String,
    pub extraction_start: String,
    pub chunk_arrival: String,
    pub natural_decay: String,
    pub drill_rig: String,
    pub drill_rig_tier: i64,
    pub popped_at: i64,
    pub popped_manual: bool,
    pub popped_by: String,
    pub unanchors_at: String,
    pub sv_on: i64,
    pub sv_off: i64,
    pub is_skyhook: bool,
    pub sky_hourly: f64,
    pub sky_rent: f64,
    pub sky_streak: i64,
    pub sky_unraided: i64,
    pub sky_raided: i64,
    pub sky_wstart: String,
    pub sky_wend: String,
    pub sky_bays: bool,
    pub sky_est: bool,
    pub sky_unsec_m3: f64,
    pub sky_unsec_isk: f64,
    pub sky_sec_m3: f64,
    pub sky_sec_isk: f64,
    pub sky_last_raided: String,
    pub is_pos: bool,
    pub tower: String,
    pub pos_race: String,
    pub moon_id: i64,
    pub pos_sov: bool,
    pub stront: f64,
    pub stront_hours: f64,
    pub log: Vec<RefuelEvent>,
}

impl Default for Row {
    fn default() -> Self {
        Row {
            sid: 0,
            name: String::new(),
            system: String::new(),
            typ: String::new(),
            state: String::new(),
            services: String::new(),
            fuel_expires: String::new(),
            last_refuel: String::new(),
            has_fuel: false,
            has_refuel: false,
            days: 0.0,
            refuel_added: 0.0,
            burn7: -1.0,
            burn30: -1.0,
            est: -1.0,
            need: -1.0,
            m3: -1.0,
            bpd: -1.0,
            blocks_now: -1.0,
            m3_now: -1.0,
            lo_min: -1.0,
            lo_target: -1.0,
            gas_day: -1.0,
            gas_month: -1.0,
            gas_m3: -1.0,
            fuel2_name: String::new(),
            fuel2: -1.0,
            rental: String::new(),
            renter: String::new(),
            goo_m3: -1.0,
            goo_cap: -1.0,
            goo_isk: -1.0,
            has_econ: false,
            econ_goo: 0.0,
            econ_rent: 0.0,
            econ_fuel: 0.0,
            econ_gas: 0.0,
            econ_net: 0.0,
            goo: Vec::new(),
            state_timer_start: String::new(),
            state_timer_end: String::new(),
            extraction_start: String::new(),
            chunk_arrival: String::new(),
            natural_decay: String::new(),
            drill_rig: String::new(),
            drill_rig_tier: -1,
            popped_at: 0,
            popped_manual: false,
            popped_by: String::new(),
            unanchors_at: String::new(),
            sv_on: -1,
            sv_off: -1,
            is_skyhook: false,
            sky_hourly: -1.0,
            sky_rent: -1.0,
            sky_streak: -1,
            sky_unraided: -1,
            sky_raided: -1,
            sky_wstart: String::new(),
            sky_wend: String::new(),
            sky_bays: false,
            sky_est: false,
            sky_unsec_m3: -1.0,
            sky_unsec_isk: -1.0,
            sky_sec_m3: -1.0,
            sky_sec_isk: -1.0,
            sky_last_raided: String::new(),
            is_pos: false,
            tower: String::new(),
            pos_race: String::new(),
            moon_id: 0,
            pos_sov: false,
            stront: -1.0,
            stront_hours: -1.0,
            log: Vec::new(),
        }
    }
}

#[derive(Clone, Default)]
pub struct Refuel {
    pub sid: i64,
    pub seen_at: String,
    pub name: String,
    pub system: String,
    pub new_expires: String,
    pub by: String,
    pub days_added: f64,
    pub blocks: f64,   // fuel-log estimate of the deposit; -1 = rate unknown
    pub pending: bool, // a filed claim ESI has not shown the jump for yet
}

#[allow(dead_code)]
#[derive(Clone, Default)]
pub struct Notif {
    pub typ: String,
    pub at: i64,
    pub sid: i64,
    pub system_id: i64,
    pub moon_id: i64,
    pub by: String, // MoonminingLaserFired: who pressed the button
}

// everything the worker thread writes and the UI reads, one lock
#[derive(Clone, Default)]
pub struct Shared {
    pub rows: Vec<Row>,
    pub refuels: Vec<Refuel>,
    pub notifs: Vec<Notif>,
    pub pulled_at: String,
    pub corp_name: String,
    pub fuel2_status: String,
    pub starbases_status: String,
    pub rentals_online: bool,
    pub extractions_ok: bool,
    pub timers_ok: bool,
    pub esi_lastmod: String,
    pub esi_expires: String,
    pub status: String,
    pub note: String,
    pub note_at: i64,
    pub data_gen: u64,
}

// Display names: the game's mouthfuls shortened for the table (raw names
// still drive the fuel2 logic and the wire format).
pub fn display_type(t: &str) -> String {
    if t == "Metenox Moon Drill" {
        return "Metenox".into();
    }
    if t == "Orbital Skyhook" {
        return "Skyhook".into();
    }
    if t.contains("Control Tower") {
        return "POS".into();
    }
    if t.starts_with("Ansiblex") {
        return "Jump-Bridge".into();
    }
    if t.starts_with("Pharolux") {
        return "Cyno Bacon".into(); // yes, bacon
    }
    t.into()
}

pub fn fuel2_kind(typ: &str) -> &'static str {
    if typ == "Metenox Moon Drill" {
        return "Magmatic Gas";
    }
    if typ.starts_with("Ansiblex") || typ.starts_with("Pharolux") {
        return "Liquid Ozone";
    }
    ""
}

pub const GAUGE_DAYS: f64 = 30.0; // every days gauge renders on this scale

pub fn fuel2_days(r: &Row) -> f64 {
    if r.gas_day > 0.0 && r.fuel2 >= 0.0 {
        r.fuel2 / r.gas_day
    } else {
        -1.0
    }
}

// 0 red, 1 orange, 2 yellow, 3 green, -1 grey/no-data
pub fn urgency_band(days: f64) -> i32 {
    if days < 3.0 {
        0
    } else if days < 7.0 {
        1
    } else if days < 14.0 {
        2
    } else {
        3
    }
}

pub fn fuel2_band(r: &Row) -> i32 {
    if r.fuel2_name.is_empty() || r.fuel2 < 0.0 {
        return -1;
    }
    if r.fuel2 <= 0.0 {
        return 0;
    }
    let d = fuel2_days(r);
    if d >= 0.0 {
        return urgency_band(d);
    }
    if r.lo_min > 0.0 && r.fuel2 < r.lo_min {
        return 1;
    }
    if r.lo_target > 0.0 && r.fuel2 < r.lo_target {
        return 2;
    }
    3
}

// Usage-adjusted days left when burn history exists, else ESI's flat number.
pub fn effective_days(r: &Row) -> f64 {
    if r.est >= 0.0 {
        r.est
    } else {
        r.days
    }
}

// Effective burn: prefer the 7d window, fall back to 30d (detail line only).
pub fn burn_of(r: &Row) -> f64 {
    if r.burn7 >= 0.0 {
        r.burn7
    } else {
        r.burn30
    }
}

// Haul to 30d: UNITS = fuel blocks
pub fn units_raw(r: &Row) -> String {
    if r.need < 0.0 {
        return "--".into();
    }
    if r.need == 0.0 {
        return "ok".into();
    }
    commas(r.need)
}

// GAS-2-30D column: secondary fuel to haul
pub fn gas30_raw(r: &Row) -> String {
    let need = if r.gas_day > 0.0 {
        ((GAUGE_DAYS - fuel2_days(r)) * r.gas_day).round().max(0.0)
    } else if r.lo_target > 0.0 {
        (r.lo_target - r.fuel2).max(0.0)
    } else {
        return String::new();
    };
    if need == 0.0 {
        return "ok".into();
    }
    let s = commas(need);
    if s.len() > 7 {
        compact_units(need)
    } else {
        s
    }
}

// tolerant JSON getters, the .value()/-1 idiom from the C++ side
fn js(v: &Value, k: &str) -> String {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string()
}
fn jf(v: &Value, k: &str) -> f64 {
    v.get(k).and_then(|x| x.as_f64()).unwrap_or(-1.0)
}
fn ji(v: &Value, k: &str, def: i64) -> i64 {
    v.get(k).and_then(|x| x.as_i64()).unwrap_or(def)
}
fn jb(v: &Value, k: &str) -> bool {
    v.get(k).and_then(|x| x.as_bool()).unwrap_or(false)
}
fn jarr<'a>(v: &'a Value, k: &str) -> &'a [Value] {
    v.get(k).and_then(|x| x.as_array()).map(|a| a.as_slice()).unwrap_or(&[])
}

// parse one feed snapshot into Shared (holds the lock only for the swap)
pub fn ingest(shared: &std::sync::Mutex<Shared>, raw: &str) {
    if raw.is_empty() {
        shared.lock().unwrap().status = "no data (network? auth?)".into();
        return;
    }
    let d: Value = match serde_json::from_str(raw) {
        Ok(v) => v,
        Err(_) => {
            shared.lock().unwrap().status = "bad JSON from box".into();
            return;
        }
    };

    let mut rows: Vec<Row> = Vec::new();
    for s in jarr(&d, "structures") {
        let mut r = Row::default();
        r.sid = ji(s, "structure_id", 0);
        r.name = js(s, "name");
        r.system = js(s, "system");
        r.typ = js(s, "type");
        r.state = js(s, "state");
        r.state_timer_start = js(s, "state_timer_start");
        r.state_timer_end = js(s, "state_timer_end");
        r.unanchors_at = js(s, "unanchors_at");
        r.sv_on = ji(s, "services_online", -1);
        r.sv_off = ji(s, "services_offline", -1);
        r.extraction_start = js(s, "extraction_start");
        r.chunk_arrival = js(s, "chunk_arrival");
        r.natural_decay = js(s, "natural_decay");
        r.drill_rig = js(s, "drill_rig");
        r.drill_rig_tier = ji(s, "drill_rig_tier", -1);
        r.services = js(s, "services");
        r.fuel_expires = js(s, "fuel_expires");
        if let Some(fd) = s.get("fuel_days_left").and_then(|x| x.as_f64()) {
            r.has_fuel = true;
            r.days = fd;
        }
        // recompute the clock locally: feeds bake fuel_days_left at fetch time
        if !r.fuel_expires.is_empty() {
            let fe = parse_iso(&r.fuel_expires);
            if fe != 0 {
                r.has_fuel = true;
                r.days = (fe - now()) as f64 / 86400.0;
            }
        }
        r.burn7 = jf(s, "burn_7d");
        r.burn30 = jf(s, "burn_30d");
        r.est = jf(s, "est_fuel_days");
        r.need = jf(s, "blocks_to_30d");
        r.m3 = jf(s, "m3_to_30d");
        r.fuel2_name = fuel2_kind(&r.typ).into();
        r.is_pos = jb(s, "is_pos");
        if r.is_pos {
            r.tower = r.typ.clone(); // keep the full hull name for the detail
        }
        r.pos_race = js(s, "pos_race");
        r.moon_id = ji(s, "moon_id", 0);
        r.pos_sov = jb(s, "pos_sov");
        r.typ = display_type(&r.typ);
        r.fuel2 = jf(s, "fuel2_units");
        r.rental = js(s, "rental");
        r.renter = js(s, "renter");
        r.goo_m3 = jf(s, "goo_m3");
        r.goo_cap = jf(s, "goo_capacity");
        r.goo_isk = jf(s, "goo_isk");
        if s.get("econ_net").and_then(|x| x.as_f64()).is_some() {
            r.has_econ = true;
            r.econ_goo = jf(s, "econ_goo");
            r.econ_rent = jf(s, "econ_rent");
            r.econ_fuel = jf(s, "econ_fuel");
            r.econ_gas = jf(s, "econ_gas");
            r.econ_net = jf(s, "econ_net");
        }
        for g in jarr(s, "goo") {
            r.goo.push(GooItem {
                name: js(g, "name"),
                qty: g.get("qty").and_then(|x| x.as_f64()).unwrap_or(0.0),
                m3: g.get("m3").and_then(|x| x.as_f64()).unwrap_or(0.0),
                isk: g.get("isk").and_then(|x| x.as_f64()).unwrap_or(0.0),
            });
        }
        r.bpd = jf(s, "blocks_per_day");
        r.blocks_now = jf(s, "blocks_now");
        r.m3_now = jf(s, "m3_now");
        r.lo_min = jf(s, "lo_min");
        r.lo_target = jf(s, "lo_target");
        r.gas_day = jf(s, "gas_per_day");
        r.gas_month = jf(s, "gas_month_units");
        r.gas_m3 = jf(s, "gas_month_m3");
        r.stront = jf(s, "pos_stront");
        r.stront_hours = jf(s, "pos_stront_hours");
        for e in jarr(s, "refuel_log") {
            r.log.push(RefuelEvent {
                seen_at: js(e, "seen_at"),
                days_added: e.get("days_added").and_then(|x| x.as_f64()).unwrap_or(0.0),
                by: js(e, "by"),
            });
        }
        if let Some(lr) = s.get("last_refuel").and_then(|x| x.as_str()) {
            r.has_refuel = true;
            r.last_refuel = lr.to_string();
            r.refuel_added = s
                .get("last_refuel_days_added")
                .and_then(|x| x.as_f64())
                .unwrap_or(0.0);
        }
        rows.push(r);
    }

    let mut refuels: Vec<Refuel> = Vec::new();
    // filed claims still waiting for ESI to show the jump sit on top of the log
    for e in jarr(&d, "pending_claims") {
        let mut v = Refuel {
            pending: true,
            blocks: -1.0,
            ..Default::default()
        };
        v.seen_at = js(e, "created_at");
        v.name = if js(e, "name").is_empty() { "?".into() } else { js(e, "name") };
        v.system = if js(e, "system").is_empty() { "?".into() } else { js(e, "system") };
        v.sid = ji(e, "structure_id", 0);
        v.by = js(e, "by");
        refuels.push(v);
    }
    for e in jarr(&d, "refuels") {
        let mut v = Refuel {
            blocks: -1.0,
            ..Default::default()
        };
        v.seen_at = js(e, "seen_at");
        v.name = if js(e, "name").is_empty() { "?".into() } else { js(e, "name") };
        v.system = if js(e, "system").is_empty() { "?".into() } else { js(e, "system") };
        v.new_expires = js(e, "new_expires");
        v.sid = ji(e, "structure_id", 0);
        v.by = js(e, "by");
        v.blocks = jf(e, "blocks");
        v.days_added = e.get("days_added").and_then(|x| x.as_f64()).unwrap_or(0.0);
        refuels.push(v);
    }
    for v in refuels.iter_mut() {
        if v.blocks < 0.0 && v.days_added > 0.0 && v.sid != 0 {
            if let Some(r) = rows.iter().find(|r| r.sid == v.sid) {
                if r.bpd > 0.0 {
                    v.blocks = (v.days_added * r.bpd).round();
                }
            }
        }
    }

    // watched skyhooks join the table as fuel-less rows
    for s in jarr(&d, "skyhooks") {
        let mut r = Row::default();
        r.is_skyhook = true;
        r.sid = ji(s, "planet_id", 0);
        r.system = js(s, "system");
        r.typ = "Skyhook".into();
        let roman = js(s, "planet_roman");
        r.name = format!(
            "{}{} Skyhook",
            r.system,
            if roman.is_empty() { String::new() } else { format!(" {roman}") }
        );
        r.sky_hourly = jf(s, "hourly_isk");
        r.sky_rent = jf(s, "monthly_rent_isk");
        r.sky_streak = ji(s, "streak", -1);
        r.sky_unraided = ji(s, "unraided", -1);
        r.sky_raided = ji(s, "raided", -1);
        r.sky_wstart = js(s, "window_start");
        r.sky_wend = js(s, "window_end");
        r.state = js(s, "state");
        r.sky_unsec_m3 = jf(s, "unsec_m3");
        r.sky_unsec_isk = jf(s, "unsec_isk");
        r.sky_est = jb(s, "est");
        r.sky_last_raided = js(s, "last_raided_at");
        if jb(s, "bays_ok") {
            r.sky_bays = true;
            r.sky_sec_m3 = jf(s, "sec_m3");
            r.sky_sec_isk = jf(s, "sec_isk");
        }
        rows.push(r);
    }

    let mut notifs: Vec<Notif> = Vec::new();
    for e in jarr(&d, "notifications") {
        notifs.push(Notif {
            typ: js(e, "type"),
            at: parse_iso(&js(e, "timestamp")),
            sid: ji(e, "structure_id", 0),
            system_id: ji(e, "system_id", 0),
            moon_id: ji(e, "moon_id", 0),
            by: js(e, "fired_by_name"),
        });
    }
    // stamp the real chunk-fracture moment onto the refinery rows
    for n in &notifs {
        let man = n.typ == "MoonminingLaserFired";
        if !man && n.typ != "MoonminingAutomaticFracture" {
            continue;
        }
        for r in rows.iter_mut() {
            if r.sid == n.sid && n.at > r.popped_at {
                r.popped_at = n.at;
                r.popped_manual = man;
                r.popped_by = n.by.clone();
            }
        }
    }

    let mut g = shared.lock().unwrap();
    g.rows = rows;
    g.refuels = refuels;
    g.notifs = notifs;
    g.pulled_at = js(&d, "pulled_at");
    g.corp_name = js(&d, "corp");
    g.fuel2_status = js(&d, "fuel2_status");
    g.starbases_status = js(&d, "starbases_status");
    g.rentals_online = jb(&d, "rentals_online");
    g.extractions_ok = jb(&d, "extractions_ok");
    g.timers_ok = jb(&d, "timers_ok");
    g.data_gen += 1;
    g.esi_lastmod = js(&d, "esi_last_modified");
    g.esi_expires = js(&d, "esi_expires");
    g.status = if d.get("error").is_some() {
        format!("box error: {}", js(&d, "error"))
    } else if jb(&d, "refreshing") {
        "server pulling fresh ESI - data lands within ~1 min".into()
    } else {
        String::new()
    };
}
