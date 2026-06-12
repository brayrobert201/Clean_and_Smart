# CLAUDE.md — Bob's Transport (Pebble watchface)

A Pebble Time 2 (emery) watchface: a fork of "Clean & Smart" that replaces the
day-of-week line with the next Sydney train departures. Installed on Bob's real watch.

> For the Pebble SDK, build/install, and the **emery emulator runbook + recovery**, use the
> **`pebble` skill** — don't re-derive the emulator bring-up. This file is the project-specific layer.

## Repo

- Fork: `origin` = `github.com/brayrobert201/Clean_and_Smart`; upstream author = `ygalanter`
  (`upstream` remote). Rebase from `upstream/master`, push to `origin`.
- ~1,500 LOC (C + JS). Single watchface, one developer. Keep it light — no heavy process.
- Commit per Bob's standards: specific file names (no `git add -A`), Conventional Commits,
  split feat/chore.

## Architecture (see the `pebble` skill for the general model)

- `src/c/main.c` (+ `main.h`) — watch side. Renders time/date/weather/battery + the train row.
  Train rendering: `update_train_display()` (clock times, direction icon, express `*`, shift-on-pass,
  day-of-week fallback). Refresh cadence is watch-driven in `tick_handler` (peak-aware interval).
- `src/pkjs/app.js` — phone side. `fetchTrain()` → `resolveStop()` (name→stop-id) → `queryTrip()`
  (TfNSW Trip Planner) → sends two departures. Weather throttled hourly. Direction by time-of-day
  cutoff or alternate-each-refresh.
- `src/pkjs/config.json` — Clay config (visual settings + Sydney Trains section).
- `src/pkjs/secrets.json` — **gitignored**; `{ tfnswApiKey, homeStationName/Id, workStationName/Id,
  afternoonCutoffHour }`. Bundled via `require()`; Clay overrides when set. Template: `secrets.json.example`.
- `html/clean_smart_config.htm` — legacy Slate page, **unused** (Clay replaced it). Don't touch.
- `run-emulator.sh` — canonical emery emulator launcher. **Use this, not `pebble install --emulator`.**

## Message keys (mirror in `main.h` + `package.json`; JS uses string names)

| # | key | dir | meaning |
|---|-----|-----|---------|
| 0–12 | weather/visual/colour/language | — | inherited from Clean & Smart |
| 13/14/15 | TRAIN_DEPARTURE / PLATFORM / DIRECTION | →watch | dep1 epoch, "P4", 0=work/1=home |
| 16/17/18 | TRAIN_DEPARTURE2 / PLATFORM2 / EXPRESS | →watch | dep2, bitmask (bit0/1 = dep1/2 fastest) |
| 19–24 | PEAK1_START/END, PEAK2_START/END, PEAK_INTERVAL, OFFPEAK_INTERVAL | →watch | refresh schedule |
| 30–35 | TFNSW_API_KEY, HOME/WORK_STATION_NAME, DIRECTION_MODE, CUTOFF_HOUR, FASTEST_MARKER | phone-only | Clay persistence only |

## TfNSW Trip Planner API (phone side only)

```
GET https://api.transport.nsw.gov.au/v1/tp/trip?outputFormat=rapidJSON
  &coordOutputFormat=EPSG:4326&depArrMacro=dep
  &type_origin=any&name_origin=<stopId>&type_destination=any&name_destination=<stopId>
  &calcNumberOfTrips=6&excludedMeans=checkbox&exclMOT_4=1&exclMOT_5=1&exclMOT_7=1&exclMOT_9=1&exclMOT_11=1
  &TfNSWTR=true&version=10.2.1.42
Header: Authorization: apikey <KEY>
```
- Free key from opendata.transport.nsw.gov.au (60k req/day). Keep Metro included (`product.class` 2)
  alongside train (1); skip walking legs (class 99/100).
- Departure = first rail leg `origin.departureTimeEstimated || departureTimePlanned` (ISO UTC).
  Platform via `/Platform (\w+)/` on `origin.disassembledName`. End-to-end arrival = last rail leg
  `destination.arrival*` (for the express marker).
- **No CORS** → station-name search must run in pkjs (`stop_finder`), never the Clay webview.
- Stop IDs are global numeric (find via transportnsw.info URL or `stop_finder`).
  Bob's commute: **Gordon `207210` → Wynyard `200080`**.

## Run / verify / install

```
pebble build                       # all 5 platforms
./run-emulator.sh                  # build + launch emery + install (handles the flaky bring-up)
pebble screenshot --emulator emery /tmp/x.png
pebble install --cloudpebble       # to Bob's watch (Dev Connection on; `pebble login` once).
                                   #   Fallback: --phone 172.23.69.121 (LAN)
```
Inject train data to test C-side rendering (all `--uint` in ONE flag):
`pebble send-app-message --emulator emery --uint 13=<epoch> 16=<epoch2> 15=0 18=2 --string 14=P2 17=P5`

## Design decisions (why it's built this way)

- **Watch-driven refresh**, not a JS timer — the phone OS kills/restarts pkjs; the tick handler is the
  only dependable clock.
- **Secrets phone-side only** — the API key never reaches the watch in any design; `secrets.json` keeps
  it out of git and the emulator zero-config.
- **Keep the UUID** across renames — persist storage is keyed by UUID, so settings survive reinstalls.
- **Express `*` = genuine overtake** (a later train arriving before an earlier one), not "earliest
  arrival" — otherwise every first train gets marked and the `*` is noise.
- **Single-row departures + one direction icon** (house=home / building=work, emery only) — direction is
  shared across shown trains, so showing it once frees width for both departures at a larger font.
