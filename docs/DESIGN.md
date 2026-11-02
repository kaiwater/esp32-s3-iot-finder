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
| `MANUAL` | stay on the channel selected with long `>` / `<` |

Targets are always collected in `AUTO` cruise mode.  Selecting a candidate
records the target and its last channel but does not stop the hop.  A short
`#` explicitly switches to `LOCKED` on the target's last-seen channel; another
short `#` returns to `AUTO` and continues collecting.  This keeps the candidate
table populated from all 13 channels instead of pinning it to the first STA
that happens to appear.

## Candidate learning

Every management/data frame contributes a candidate from the STA address:

- ToDS data: `addr2`
- FromDS data: `addr1`
- probe request / assoc request: `addr2`

The 32-entry table tracks frames, last seen, RSSI, channel and BSSID.  Probe
requests also store the SSID being sought, so an unassociated STA can still be
classified by the network name it is probing for.  `list` prints the AP BSSID
and the best-known SSID; `stas` walks the BSS table (including hidden-SSID
entries) and attaches the active candidates seen with each BSSID.  It prints
the AP's advertised station count alongside `seen N`, so an AP that reports
five stations but only exposes two data-frame MACs is visible as such.
Learning
scores candidates by `(rssi + 100) * 100 + min(frames, 99)`, so the strongest
device wins with activity as a tie-breaker.  A candidate must be seen during
the five-second window.  Short `>` / `<` move forward/backward through the
recent candidates and lock to the selected candidate's channel.

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
probe-request IE if the AP is not in the cache.  Beacons with an empty SSID are
still stored as `<hidden>` (provided they carry a matching DS channel), so a
STA can be grouped by a known hidden AP.  Only when no beacon for the BSSID was
seen does the screen say `AP BEACON NOT HEARD`; a 2.4GHz sniffer cannot recover
the SSID of a hidden/mesh AP, or of a 5GHz-only AP, from encrypted STA data
frames.

## Persistence

Two records live in the `finder` NVS namespace:

- `mac` — the six-byte target MAC.  Written only when the target changes and
  erased on clear.
- `time_anchor` — a packed `(magic, version, BSSID, TSF, UTC, ad_base, TZ)`
  record used by the WNM clock.

The time anchor makes the clock survive a finder reboot even though hostapd
caches its Time Advertisement:

```
utc_now = stored_utc + (tsf_now - stored_tsf) / 1e6
```

The TSF delta is only accepted when the AP's current TSF is not smaller than
the stored one (an AP reboot invalidates the record).  The record is rewritten
only when the source BSSID, Time Value, or time zone changes; there is no
periodic flash churn.  After a sync, adverts from other APs that are older than
the running clock are ignored, so a stale cached value cannot drag the display
backwards; a `timeclear` serial command erases the anchor.

There is no network connection and no telemetry.  The sniffer is receive-only:
it never starts a scan or transmits a probe request.  Any active probe mode
must be an explicit, opt-in command; the default stays passive.

## UI and keys

| Key | Short | Long |
|---|---|---|
| `>` (K1) | next candidate | next channel |
| `<` (K2) | previous candidate | previous channel |
| `#` (K3) | lock/hop, or learn when no target | learn 5 s |
| `*` (K4) | menu / back | clear target |

Pages are a small state machine: FINDER, MENU, INFO, AP LIST, AP DETAIL, STA
LIST (and the older flat SSID page is kept for the target detail).  The menu
contains learn, clear, peak clear, channel, AP LIST, RF SCAN, info and reboot.  Inside
a list/detail page `>`/`<` moves the selection, `#` enters the selected item
and `*` returns one level.  The AP list is sorted by associated STA count then
RSSI, includes hidden APs under `<AABBCC>` names, and the STA list sets a
volatile target without locking the channel.  The FINDER page stays on the
green-on-dark PHO palette.

## SSID encoding and CJK font

An SSID is an arbitrary byte string.  The firmware:

1. returns ASCII when every byte is < 0x80;
2. validates UTF-8 first;
3. falls back to GBK through a generated GBK->Unicode table;
4. when both are valid, rejects a UTF-8 interpretation that decodes to an
   implausible script (Hebrew/Greek/private use) while GBK decodes cleanly.

Non-ASCII text is drawn from a 12x12 bitmap table for U+4E00-U+9FFF,
U+3000-U+303F and U+FF00-U+FFEF, generated from Noto Sans CJK SC by
`tools/gen-cjk-font.py`.  ASCII stays on the existing 8x8 font.  The FINDER line
shows the encoding tag; `* -> SSID` opens a five-line detail page, and `#`
switches between UTF8 and GBK when both interpretations are valid.

The table costs about 0.5MB of flash, leaving roughly 28% free in the 2MB app
partition.

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

Up to two NRF24L01 modules are initialized opportunistically.  Each CSN/CE
pair gets its own `nrf_t` and SPI device, so probing CSN 18 does not keep
talking to the device created for CSN 17.  Startup waits 3 s, then probes every
pair (17/38, 18/39, 21/40) several times, and each pair is read back after a
PWR_UP toggle.  The SPI clock is 1MHz to tolerate long jumper wires.  The second module is
probed 600ms after the first is found so their power-up current is staggered;
if it is still missing, a background hot-plug probe retries every 5s without
resetting the working radio.  When two modules are present, INFO/status
reports both CSNs.  Re-probing a different
CSN removes and re-adds the SPI device, so the probe really tests that CSN.
The task maps the current Wi-Fi channel to the NRF channel at the same centre
frequency, samples RPD eight times, and publishes 0–100%.  The NRF runs
independently and does not affect the Wi-Fi sniffer.  If no module is found,
the UI shows `RF --`; when found, INFO shows `RF OK C<csn> <duty>%`.

The RF SCAN page temporarily takes the NRF off the Wi-Fi channel and sweeps
1..13.  With two modules it scans 1..6 and 8..13 in parallel and channel 7 with
the first radio; with one module it walks all 13.  Each channel gets 16 RPD
samples (8 ms), and the peak is retained across sweeps until reset.  The screen draws the peak as the outer bar and the
smoothed average as the inner bar, which is the useful view for a frequency
hopper: a single visit to a channel leaves a visible peak.

AP SSIDs are marked binary when they contain control bytes or are not valid
UTF-8/GBK; those are drawn as `<AABBCC>` instead of attempting a lossy decode.
The AP pages carry an `AUTO / U8 / GBK` tag and long `#` cycles the encoding
preference for the rare byte sequence that is valid in both encodings.

## Service clock and WNM time

The header clock is real local time when a 2.4GHz AP advertises 802.11v WNM
time, otherwise it falls back to `UP HH:MM:SS` uptime.  The synchronized
header is a full-width `YYYY-MM-DD  HH:MM:SS` banner rendered at 2x scale;
there is no timezone label, and the old `2.4G` corner label was removed because
the whole board is 2.4GHz-only.

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
- A persistent anchor (`(BSSID, TSF, UTC, ad_base, TZ)` in NVS) extends direct
  mode across finder reboots; the elapsed time is recovered from the beacon TSF.
  After the first sync, an advert older than the running clock is ignored, and
  `timeclear` drops the anchor if the AP side changes drastically.
- A fresh advertisement (within 2 s of the running clock) is applied
  immediately when it differs by at least 3 s, so a hostapd/WNM rebuild is
  reflected without waiting for a source timeout; the dead-band prevents
  seconds jitter between mesh BSSIDs.
- The stale-advert guard matters because 2.4GHz meshes/extenders expose several
  BSSIDs whose hostapd instances cached their Time Advertisement at different
  times; without the guard the display can jump backwards when it roams between
  them.
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
CJK table is stored as `const` data in flash (about 511KB); the 2x5 and 3x5
fonts are tiny lookup tables.  The app partition is 2MB, leaving about 28% free
after the font tables and the generated OUI table.
