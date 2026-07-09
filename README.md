# STOKER

Live terminal fuel-watch dashboard for EVE Online Upwell structures.
A stoker keeps the furnaces fed; so does this.

![terminal app](bpos-dash.svg)

Fuel-sorted by default so whatever runs dry first is on top.

## Running

**Windows:** unzip `stoker-windows-x64.zip` and double-click `stoker.exe`.
No terminal or PowerShell needed - Windows opens its own console window
(Windows Terminal on 11 looks best, the legacy console works too).
SmartScreen will warn about the unsigned download the first time: choose
"More info" then "Run anyway" (open-source build, no code-signing cert).
Needs Windows 10 version 1803+ (`curl.exe` ships with the OS).

**Linux:** untar `stoker-linux-x86_64.tar.gz` and run `./stoker`. Static
binary, needs only `curl` installed.

First run opens your browser for an EVE login; approve the single
(read-only) structure permission and the dashboard appears. That's the
whole setup: the refresh token is kept locally so it's a one-time dance.
Your character needs the **Station_Manager** (or Director) in-game role,
or ESI refuses the corp structure list.

Standalone (EVE login) mode shows name / system / type / state / services /
fuel days for every corp structure. The 2ND FUEL column (Magmatic Gas on
Metenox drills, Liquid Ozone on Ansiblex gates and Pharolux beacons) reads
the structure fuel bay from corp assets. The login requests both read-only
scopes by default; the column needs the in-game Director role and shows
"?" without it. Set `"scopes"` in config.json to trim the request back
to structures-only if you prefer. Burn rates, bay estimates and the
refuel log are computed server-side by a hosted STOKER backend and show as
unknown without one.

**Persistent refuel log (standalone):** ESI has no "someone fueled this"
event and the app keeps no database, so each poll reports the structure
list + fuel clocks it just pulled to a small history service, which diffs
snapshots over time and serves back your corp's refuel log (the `f` view).
The report contains exactly what your own ESI view returned: structure
ids, names, systems, and fuel expiry times, nothing else, no tokens.
Privacy switch: set `"history_api": ""` in config.json to disable, or
point it at your own server (two endpoints: `POST /report`,
`GET /refuels?corp_id=`; see the STOKER backend source).

**Updates:** on startup STOKER checks the newest GitHub release once; if
a newer version exists the header offers it and `u` downloads and swaps
the binary in place (restart to run it). Set `"update_check": false` in
config.json to disable.

**Multiple corps:** press `alt+c` in the app (or run `stoker --add`) to log
in another character. Every corp your characters can read gets its own tab
(labeled by corp ticker); press `c` to switch. With access to just one corp there is no tab bar,
the dashboard looks exactly as before. Two characters in the same corp
share one tab, and a character without the in-game role simply
contributes nothing.

### Corp mode

If whoever hosts a STOKER backend gave you an endpoint URL and access key,
run `stoker --corp` once and paste them in. Full feature set: usage-based
burn rates, bay estimates, refuel log, refuel claims, doctrine ozone tiers.
Settings live in `~/.config/stoker/config.json` (Windows:
`%APPDATA%\stoker\config.json`); env overrides `STOKER_ENDPOINT`,
`STOKER_KEY`, `STOKER_CLIENT_ID`.

Set the `STOKER_NAME` environment variable so `[1]` refuel claims are
stamped as you (corp mode only).

## Keys

| Key | Action |
| --- | --- |
| up/down, pgup/pgdn, wheel | scroll |
| tab | cycle structure-type filter |
| s | cycle sort (fuel / type / system / name / need) |
| / | text filter (Enter apply, Esc clear) |
| f | refuel log |
| c | switch corp tab (when more than one corp is visible) |
| alt+c | add another character (standalone) |
| right arrow | detail page for selected structure (left/Esc back) |
| 1 | "I fueled this" claim on the selected refuel event (corp mode) |
| r | request a fresh pull |
| q | quit |

## Building

Portable build via CMake (fetches FTXUI v5.0.0 automatically):

    cmake -S . -B build && cmake --build build -j

Release packages for both platforms (needs `g++-mingw-w64-x86-64-posix`
for the Windows cross build):

    ./build-release.sh

## Notes

- No secrets ship in the source or release binaries. Corp-mode keys live in your
  local config file; standalone uses EVE SSO with PKCE (public client, no
  app secret) and stores tokens with owner-only permissions.
- ESI has no "who fueled it" event, so BY comes from `[1]` claims and
  refuel block counts are estimates (days added x the structure's service
  burn rate). Fuel-bay contents (the F² column) are only visible through
  Director-scoped corp assets; without that they show "?".
