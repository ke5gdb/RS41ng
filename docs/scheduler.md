# How the RS41ng transmit scheduler works

This document explains how RS41ng decides *what* to transmit and *when*. It's
written for hams who want to understand or explain the timing behavior — for
example, why an APRS packet lands at a particular second past the minute, or why
Horus frames and APRS take turns.

All of the logic described here lives in [`src/radio.c`](../src/radio.c), mainly
in `radio_find_ready_entry()`, `radio_check_time_sync()`, and
`radio_handle_main_loop()`.

## The big picture

The firmware keeps a **transmit schedule**: a fixed table of "entries," one per
enabled mode (APRS, Horus V2/V3, CW, Pip, CATS, WSPR, FT8, etc.). The exact
table depends on your board (Si4032 on the RS41, Si4063 on the DFM-17, optional
Si5351), and it is built at compile time from your `config.h` — disabled modes
are left out entirely.

On every pass through the main loop, the scheduler asks one question:

> *Is there an entry that is ready to transmit right now?*

If yes, it transmits it. If no, it sleeps briefly and asks again. The interesting
part is how "ready" is decided, because entries fall into two camps:

- **Time-synced entries** (`*_TIME_SYNC_SECONDS > 0`): these must fire at a
  specific moment relative to GPS time — e.g. APRS once per minute, WSPR at the
  start of an even minute, FT8 every 15 s.
- **Free-running entries** (`*_TIME_SYNC_SECONDS == 0`): these have no required
  timing and just take turns whenever the radio is idle.

## The three-tier selection logic

`radio_find_ready_entry()` checks three tiers in order and returns the first
match. This ordering is the whole scheduler.

### Tier 1 — finish what you started

If an entry is partway through a repeat sequence (it's configured to send N
copies in a row via `RADIO_TX_*_COUNT`), the scheduler stays on that entry until
the sequence completes. Between repeats it honors the post-transmit delay (see
below), but it will not let any other mode jump in mid-sequence.

### Tier 2 — time-synced entries

This tier only runs when there is fresh GPS data. It walks every enabled entry
that has `time_sync_seconds > 0` and asks whether *now* falls inside that entry's
scheduled window. The math is described in the next section. If an entry's window
is open and it hasn't already fired for this period, it wins — time-synced modes
take priority over free-running ones, because their timing matters and a
free-running mode can always go later.

### Tier 3 — free-running entries (round-robin)

If nothing time-synced is due, the scheduler picks the next free-running entry in
rotation. A cursor (`radio_next_non_synced_index`) advances each time so the
modes share the air fairly instead of one always winning. This tier respects the
post-transmit delay, so free-running modes don't run back-to-back with no gap.

If no tier produces an entry, the loop waits 100 ms and tries again.

## Time-sync math (the part people ask about)

Two config values per mode control timing:

- `<MODE>_TIME_SYNC_SECONDS` — the **period**. "Transmit every N seconds,
  counted from the top of the hour in GPS time." `60` means once a minute; `120`
  means once every two minutes (e.g. WSPR); `15` means every 15 s (FT8).
- `<MODE>_TIME_SYNC_OFFSET_SECONDS` — a **delay** added after the scheduled
  instant. `APRS_TIME_SYNC_SECONDS 60` with `APRS_TIME_SYNC_OFFSET_SECONDS 45`
  means "once a minute, but 45 seconds past the minute."

There is also one global tolerance:

- `RADIO_TIME_SYNC_THRESHOLD_MS` — how wide the firing **window** is. GPS fixes
  arrive at discrete moments (typically 1 Hz), so the firmware can't catch the
  exact millisecond. The threshold says "if we're within this many ms *after*
  the scheduled instant, fire now." With a 1 Hz GPS, 5000 ms gives the scheduler
  up to five chances to catch the slot even if a couple of GPS updates are busy.

The check in `radio_check_time_sync()` is, in effect:

```
elapsed = gps_time_ms - offset_ms
position_in_period = elapsed % (period_s * 1000)
fire if position_in_period < RADIO_TIME_SYNC_THRESHOLD_MS
```

Worked example — APRS, period 60 s, offset 45 s, threshold 5 s:

- The window opens when `gps_time_ms % 60000` is in `[45000, 50000)`, i.e. **45
  to 50 seconds past every minute**.
- The first GPS update inside that window transmits; the rest are suppressed
  (see below). So you get one APRS packet at ~45 s past the minute, every minute.

### Fire exactly once per period (slot dedup)

Because the window is several seconds wide and GPS updates arrive ~once a second,
a naive check would fire the same APRS packet 4–5 times in a row. To prevent
that, each entry remembers the **slot** it last transmitted in:

```
slot = (gps_time_ms - offset_ms) / (period_s * 1000)
```

`slot` is constant across the entire tolerance window and increments by one each
period. The entry fires on the first GPS update of a new slot, records that slot,
and skips every later update in the same window. The slot is tracked
per-entry (`last_tx_slot`), so APRS and WSPR can each keep their own timing
correctly.

This is why the threshold is a *tolerance for catching the slot*, not a window
that produces multiple transmissions.

## Post-transmit delay

`RADIO_POST_TRANSMIT_DELAY_MS` inserts a quiet gap after a transmission. It is
enforced by Tier 1 (between repeats) and Tier 3 (between free-running modes), but
**Tier 2 ignores it** — a time-synced mode that is due must not be held back by a
cooldown, or it would miss its slot. (One special case: when Horus is configured
for continuous transmission, `radio_reset_transmit_delay_counter()` sets the
delay to zero so Horus streams back-to-back, unless Landed Mode is active.)

## Repeats and message rotation

When an entry transmits, `radio_next_transmit_entry()` advances two counters:

- `current_message_index` rotates through the entry's list of messages (if it has
  more than one), so successive transmissions can send different content.
- `current_transmit_index` counts copies up to `RADIO_TX_<MODE>_COUNT`. While
  this is non-zero the entry is mid-sequence and held by Tier 1. When it wraps
  back to 0 the pass is complete and the scheduler is free to pick something else.

## Important caveat: timing before a GPS fix

Time-synced scheduling keys off the GPS time-of-week. Before the receiver has a
valid fix, it may still report a time value that counts up from zero and is **not
UTC-aligned**. Tier 2 acts on GPS data as soon as it's marked "updated," and
`radio_check_time_sync()` is passed the fix quality but currently only logs it —
it does not gate on it. The practical effect: time-synced modes can transmit
against that pre-fix clock, so early packets may land at the wrong second past
the minute (often appearing right at 0 s). Once real GPS time is available,
`gps_time_ms` becomes UTC-aligned and the configured offset behaves as expected.

If you ever want to suppress transmissions until the clock is trustworthy, the
hook is `radio_check_time_sync()` (return `false` below a minimum fix) or the
`if (gps.updated)` guard in Tier 2 of `radio_find_ready_entry()`.
