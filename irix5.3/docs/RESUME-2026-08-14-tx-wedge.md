# Resume: the Indigo never crashed — it wedges transmit-only. Forensics and fixes.

Written 2026-08-14, from disk forensics on the Indigo's pulled BlueSCSI SD
card. **This corrects the hardware attribution in
`RESUME-2026-08-13-callout-race.md`**: the callout leak is real and stays
fixed-worthy (it panics IRIS in seconds at zero latency), but it is NOT what
was killing the Indigo. Read that note for the leak; read this one for what
the hardware actually does.

## What the disk proved (do not re-derive)

Method: SD card mounted on the Mac, `rb-cli` reads against
`HD1 R4000_Working_Fresh_Install.hda@1` (SGI EFS at partition index 1,
1-based). Extracted artifacts in the session scratchpad; key paths inside the
image: `/var/adm/SYSLOG`, `/var/adm/crash/`, `/usr/cpu/sysgen/IP20boot/dp.o`
(`/var/sysgen/boot` is a symlink there).

1. **No panic, ever.** `/var/adm/crash` has never held a dump (only README /
   bounds / minfree). No panic string anywhere in SYSLOG history.
2. **Exactly two incidents, both transmit-only wedges** on the same build
   (installed `dp.o` md5-identical to `dist/irix53-ip20/dp.o`, i.e. HEAD as
   of e75553b):
   - *Wedge #1*: boot Aug 13 19:03. Two-way TCP (ftp/telnet from the Mac,
     192.168.99.104) worked 19:22–20:15. After 20:15:21, no inbound TCP
     connection ever completed again. Hard reset 22:28 — **the only hard
     reset in the disk's entire history**; every other boot followed a clean
     shutdown.
   - *Wedge #2*: boot Aug 13 22:28, ran overnight, found dead-TX in the
     morning, clean shutdown Aug 14 09:59.
3. **RX stayed alive through both wedges.** `bootp[NNN]: gethostbyaddr(...)`
   lines repeat all night (last 09:46, 13 minutes before shutdown) — inetd
   spawns bootp per arriving broadcast datagram, so inbound packets were
   being collected and delivered continuously. Since only the driver's poll
   collects packets, the async engine, timer chain, dp_abusy cycling and
   dp_enabled were all demonstrably healthy. **Only TX died.** (This is also
   why the machine "looked crashed": dp0 is its only interface, and the
   screen had blanked.)
4. **The stall guard never fired** (string compiled into the installed
   object; zero SYSLOG hits). Consistent with 3: the engine was never stuck
   busy.
5. **Driver logged nothing abnormal, either wedge.** With DP_LOG compiled
   in, the last dp0 lines are the normal 22:30:09 bring-up bounce.

## Device-side evidence (BlueSCSI SD logs)

`log.txt` covers only the last power-on (the overnight session):
`SD.rename(log.txt, lastlog.txt)` fails when lastlog.txt already exists, so
`lastlog.txt` is frozen at the *first boot ever* (July, an older device
config) and each boot truncates log.txt. Wedge #1's device log is lost.

What the overnight log shows, and does not:

- **`scsi_accel_rp2040_finishRead timeout`** + PHY state dump: a
  host→device DATA-OUT phase (a WRITE data phase — for the DaynaPort that
  is a TX frame or multicast-add payload) stalled >5 s mid-transfer; the
  firmware then abandoned the command (`*resetFlag = 1`). A host that stops
  clocking mid-phase is exactly what the driver's double-submit race
  produces when it bzero()s the live request (see fix below).
- **No power distress**: firmware monitors VDD at ~2 kHz and warns below
  2800 mV after a debounce; baseline logged 3.250 V; **no warning ever**.
- **No radio-level link loss**: the firmware logs "Disassociated from
  Wi-Fi", "Attempting Wi-Fi reconnection (attempt N/M)", and per-status
  failure messages — **none appear**.
- **No failed sends**: `platform_network_send()` logs
  `cyw43_send_ethernet failed:` on any nonzero return — never appears. Also
  note the SCSI side **ignores the send return value**: a dropped TX frame
  still returns SCSI GOOD, so the IRIX driver cannot observe device-side TX
  loss. `netstat` Oerrs stays 0 either way.
- **One open oddity**: a second `Connecting to Wi-Fi SSID "DaniNet" with
  WPA/WPA2 PSK` + `Successfully connected` pair appears right after the
  finishRead dump. In the v2026.04.27 source, that logging fires only from
  boot init (did not re-run — no "=== Network Initialization ===" banner)
  or from a host-issued vendor Wi-Fi JOIN (opcode 0x1c, credentials taken
  from the host payload — nothing on IRIX sends one, and a garbled CDB
  could not have supplied the correct SSID). Unexplained; worth attaching
  the log to an upstream BlueSCSI issue if it recurs.

**Net judgment.** The power theory (Pico W + Wi-Fi DaynaPORT can out-draw
SCSI termination power; the wiki says "Power the BlueSCSI via a USB wall
plug (phone charger, etc) — the Pico-W may draw more power than available if
just powering via the SCSI bus") is *not supported by this device's own
telemetry* — USB power remains cheap insurance, not the fix. The one hard
anomaly is the abandoned mid-phase transfer, which points back at the
driver's unguarded engine claim. Where TX death *persisted* afterwards —
driver TX-queue bookkeeping desynced by the aborted command, or the radio
silently dropping while claiming success — could not be determined post-hoc.
The next-incident checklist below settles it in one minute.

## Fixes landed (this branch)

- `aa5524a` — tick-rate probe cherry-picked from `dangerous`
  (`-DDP_LOG_TICKRATE`, prints `dp0: 1s tick= sub= done= rx= busy= fg=
  stall=` once per second from the watchdog). **Compiled into the shipped
  objects on purpose** so the next hardware boot finally yields the poll
  cadence number; it costs one console/SYSLOG line per second — rebuild
  without the flag once the numbers are in.
- `d079250` — splhi()/splx() around both unguarded test-and-sets: the
  engine claim in `dp_async_tick()` (dp_abusy now claimed atomically, and
  held across chained submits instead of dipping to 0 between links) and
  the dp_timer arm in `dp_async_poll()` / `dp_async_done()` (kills the
  callout-multiplication panic reproduced under IRIS). Shared region
  untouched.
- **The watchdog never worked on 5.3 at all — now fixed.** An instrumented
  emulator run printed `dp: watchdog arg=0x0 u0ifp=0x885ee808`: 5.3's
  `if_slowtimo` calls `(*if_watchdog)(unit)` in the old BSD style, while
  `ether_attach()` wires the etherifops watchdog (which expects an ifp)
  straight into the ifnet. So `dp_eio_watchdog()` received unit 0 cast to
  a pointer: the ifp-matching loop never matched, `if_timer` was never
  re-armed, and the watchdog ran exactly once per ifconfig and died —
  which is why the tick-rate probe had "never printed a line", and which
  retroactively explains the original `ifptoeif()` panic (`Bad addr: 0x0`
  was unit 0 being dereferenced). Fix: `dp_do_attach()` installs a
  unit-style `dp_wdog53(int unit)` over the wired one; it re-arms the
  timer and carries the probe, and deliberately does not touch the packet
  engine. 5.3-only; the shared `dp_eio_watchdog()` stays for 6.5, where
  the ifp really is passed — note its probe block is now dead code on both
  releases (6.5 lacks DP_ASYNC_RX; 5.3 no longer calls it), a cleanup for
  the next shared-region edit.

## Next wedge: the one-minute triage (run BEFORE rebooting)

At the console (screen wakes on keypress — the machine is alive):

```sh
uptime                                   # long uptime = never rebooted
netstat -in                              # note Opkts and Ierrs/Oerrs
ping -c 3 <gateway>                      # let it fail
netstat -in                              # compare
```

- **Opkts incremented while pinging** → the driver is submitting TX fine;
  the device is eating frames → BlueSCSI/Wi-Fi side. (Oerrs stays 0 even
  then — the device acks dropped frames, see above.)
- **Opkts frozen** → the driver stopped submitting → driver TX path; grab
  `tail -50 /var/adm/SYSLOG` and the tick-rate lines (`tick=`) — `sub=`
  vs `done=` tells whether commands are being issued and completing.
- Then, and only then: `ifconfig dp0 down ; ifconfig dp0 up` — whether that
  revives it is itself diagnostic (device-side wedges usually survive an
  interface bounce; driver-side bookkeeping resets with it).

Also worth doing once regardless: power the BlueSCSI from a USB wall plug,
and check for firmware newer than 2026.04.27 (the 43 commits to bbace204
contain no DaynaPORT TX fixes, so an update is hygiene, not a cure).

## Gotchas that cost time today

- macOS `grep` and IRIX `grep` have no `\|` alternation — use `grep -E`.
  And SYSLOG contains binary klogpp debris: force text with `grep -a`.
- `rb-cli` partitions are **1-based**: the EFS root is `IMAGE@1`.
- BlueSCSI CD folders (`CD4/`) load images in name order; `1-dp-irix53.iso`
  sorts first. Multiple images cycle via the eject button.
- The Indigo's clock battery is dead: pre-Aug-13 SYSLOG entries claim
  "Sep 23" / "Jul 25". Date-order across boots accordingly.
- x11vnc dies repeatedly with "process or stack limit exceeded" (stack
  rlimit) — unrelated to the network, not yet investigated.
