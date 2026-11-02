# Design notes

## Why Wi-Fi, not NRF

The ESP32-S3 Wi-Fi promiscuous path exposes `rx_ctrl.rssi` for every received
802.11 frame.  That is the only signal in this hardware set that can drive a
usable hot/cold display.

The NRF24L01 only exposes `RPD`: a one-bit "above about -64 dBm" comparator.
It cannot read a MAC, a frame, or a dBm value.  It is useful as a same-channel
occupancy hint when the target is quiet, not as a locator.

## Data path

```
Wi-Fi task                         main task
----------                         ---------
promiscuous callback
  parse FC + addr1..addr4
  match target -> target stats
  push compact event -> queue
                                   drain queue
                                   candidate table
                                   BSS table
                                   median / fps / state
                                   channel policy
                                   UI at 10 Hz
```

Target statistics are updated directly in the callback under a tiny spinlock:
matching frames, last timestamp, RSSI, channel, BSSID and a five-value RSSI
ring.  This keeps the target counter accurate even if the event queue fills.

The shared event queue carries management and data frames only.  Parsing IEs,
candidate learning and SSID lookup happen in the main task.

## Target matching

The target MAC is compared against:

- `addr1` (receiver)
- `addr2` (transmitter)
- `addr3` (BSSID / DA / SA depending on ToDS/FromDS)
- `addr4` for four-address data frames

Matching receiver addresses means ACK, RTS and CTS frames count toward the
frame rate and RSSI even when the payload direction is not useful.

## Channel policy

Modes:

| Mode | Behaviour |
|---|---|
| `AUTO` | hop 1..13, 150ms per channel |
| `LOCKED` | stay on `target_ch` |
| `MANUAL` | stay on the channel selected with long K1 |

On a fresh target the firmware starts in `AUTO` with auto-lock enabled.  When a
matching frame is heard, the channel is recorded and the mode switches to
`LOCKED`.  A short K1 unlock disables auto-lock so the UI can actually hop.

## Candidate learning

Every management/data frame contributes a candidate from the STA address:

- ToDS data: `addr2`
- FromDS data: `addr1`
- probe request / assoc request: `addr2`

The 32-entry table tracks frames, last seen, RSSI, channel and BSSID.  Learning
scores candidates by `(rssi + 100) * 100 + min(frames, 99)`, so the strongest
device wins with activity as a tie-breaker.  A candidate must be seen during
the five-second window.

## BSS/SSID cache

Beacon and probe-response frames update a 32-entry BSS table.  The parser walks
the IE chain, stores IE 0 (SSID) and IE 3 (DS parameter).  Frames with no SSID
and a DS channel that disagrees with the receive channel are treated as beacon
flood traffic and ignored.  Real hidden APs are still accepted when their DS
channel matches.

The target's BSSID is derived from its own frames:

- management: `addr3`
- ToDS data: `addr1`
- FromDS data: `addr2`
- IBSS: `addr3`

The SSID shown on screen comes from that BSS entry, or from the target's own
probe-request IE if the AP is not in the cache.

## Persistence

Only the six-byte target MAC is stored, in NVS namespace `finder`, key `mac`.
It is written only when the target changes and erased on clear.  There is no
network stack connection and no telemetry.

## BSS Load

Beacon and probe-response IE 11 is parsed per BSS:

- station count (LE16)
- channel utilization (0..255, displayed as 0..100%)
- available admission capacity (LE16)

The latest value is stored in the BSS cache.  The INFO page appends
`AP <util>%/<stations>` to the link/RF line when the target's BSS is known,
and `bss` prints the raw values.  BSS Load is an AP-wide aggregate and is only
used as a congestion/diagnostic hint; it is never mixed into target RSSI or
used for hot/cold decisions.

## NRF side channel

One NRF24L01 is initialized opportunistically.  A task maps the current Wi-Fi
channel to the NRF channel at the same centre frequency, samples RPD eight
times, and publishes 0–100%.  The NRF runs independently and does not affect
the Wi-Fi sniffer.  If no module is found, the UI shows `RF --`.

## Service clock and WNM time

The header clock is real local time when a 2.4GHz AP advertises 802.11v WNM
time, otherwise it falls back to `UP HH:MM` uptime.

- IE **69** Time Advertisement with timing capability 2 contains a 10-byte
  broken-down UTC value.  In a standards-compliant AP the value is the UTC at
  which TSF was zero, so adding the 8-byte TSF gives current UTC.
- OpenWrt/hostapd 2.11 builds this element once and writes the current UTC into
  the "TSF 0" field (its source still carries a `FIX:` comment).  In that case
  the value must be used as a direct anchor and advanced with the local
  monotonic clock.
- `auto` mode collects samples from multiple BSSIDs and compares the spread of
  the two interpretations.  A small direct spread with a large TSF spread picks
  direct; the reverse picks TSF.  `timemode direct|standard` overrides it.
- `direct` keeps a stable anchor source so the display does not jump between APs
  with clocks a few seconds apart.  A source is replaced only when it is stale
  or its Time Value changes.
- IE **98** Time Zone contains a POSIX TZ string (`CST-8`,
  `CET-1CEST,M3.5.0,M10.5.0/3`, ...).  The parser handles the standard offset,
  optional DST offset, and `Mmonth.week.weekday` transition rules.
- The clock keeps running from `esp_timer_get_time()` between adverts and
  re-syncs on later frames from the same AP.
- If no time element is seen, the UI falls back to uptime.
- 67 is **not** the time element; it is BSS AVB Admission Capacity.  The `wnm`
  serial command prints counters for 67/69/98 for diagnostics.
- `docs/hostapd-time-refresh.patch` is an optional AP-side workaround that makes
  hostapd 2.11/2.12 rebuild the Time Advertisement on every beacon instead of
  caching the first one.

The header and the secondary PEAK/FPS/CAND counters use a dedicated 2x5
numeric font, so they fit without competing with the RSSI display.

## Memory

The display uses one 160x128 RGB565 framebuffer (40KB) in internal DMA RAM.  The
event queue is 96 entries; the candidate and BSS tables are fixed arrays.  The
app partition is 2MB, leaving ample room for the generated OUI table.
