# STOKER Rust port (branch: rust-port)

An experiment: the STOKER terminal dashboard rebuilt in pure Rust.
The C++ sources stay on this branch for reference; the Rust app lives in
`src/` + `Cargo.toml`.

## What's ported
- Corp mode (hosted BPOS endpoint, key-gated), reading the same
  `~/.config/stoker/config.json` / `STOKER_ENDPOINT` / `STOKER_KEY`
- `stoker` (TUI, ratatui): fuel/gas gauges, type filter, sort cycle, text
  filter, tally marks, refuel log, structure detail page, refuel claims
  [1], background 30s poll + [r] live-pull kick, `--dump` / `--version`
- `stoker-gui` (native window, egui): structure-portrait rows with
  stacked fuel + gas/ozone meters, type/sort/text filters, state, power
  badges (LOW FUEL / LOW POWER / OFFLINE / ABANDONED), UNDER ATTACK
  flash, reinforcement/unanchor/moon-pull timers, detail side panel with
  refuel log + one-click claims, Refuel Log tab, skyhook meters

## Not ported (yet)
- Standalone EVE-SSO mode (PKCE login, direct ESI sweep, corp tabs)
- Jump map, chat-intel overlay, splash
- Self-update ([u] shows a note; releases are still C++ builds)

## Differences by design
- HTTP is ureq + rustls in-process: no curl dependency at all, so the
  binary is fully self-contained
- FTXUI -> ratatui + crossterm; same layout, same Hot Neon palette

## Build
    cargo build --release
    ./target/release/stoker

## Design system (Rust GUI)
- Type: Departure Mono (pixel display face: wordmark, labels, badges) +
  JetBrains Mono (all data). All-mono on purpose; both OFL, embedded.
- Palette: Hot Neon (pink #FF2BD6 / cyan #00E5FF on ink #0A0A10) with the
  semantic urgency ramp red/orange/amber/teal, violet for secondary fuel.
- Signature: the BURN LINE under the masthead - every fueled structure is
  a tick on a 0-30d heat ruler, embers flicker under 3d, hover names a
  tick, click selects it.
