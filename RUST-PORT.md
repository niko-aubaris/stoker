# STOKER Rust port (branch: rust-port)

An experiment: the STOKER terminal dashboard rebuilt in pure Rust.
The C++ sources stay on this branch for reference; the Rust app lives in
`src/` + `Cargo.toml`.

## What's ported
- Corp mode (hosted BPOS endpoint, key-gated), reading the same
  `~/.config/stoker/config.json` / `STOKER_ENDPOINT` / `STOKER_KEY`
- The full TUI: fuel/gas gauges, type filter, sort cycle, text filter,
  tally marks, refuel log, structure detail page, refuel claims [1],
  background 30s poll + [r] live-pull kick, `--dump` / `--version`

## Not ported (yet)
- Standalone EVE-SSO mode (PKCE login, direct ESI sweep, corp tabs)
- The ImGui GUI build, jump map, intel overlay, splash
- Self-update ([u] shows a note; releases are still C++ builds)

## Differences by design
- HTTP is ureq + rustls in-process: no curl dependency at all, so the
  binary is fully self-contained
- FTXUI -> ratatui + crossterm; same layout, same Hot Neon palette

## Build
    cargo build --release
    ./target/release/stoker
