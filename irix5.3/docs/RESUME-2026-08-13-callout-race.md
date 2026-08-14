# Resume: the poll cadence is confirmed, and there is a live callout leak

Handoff written 2026-08-13, later the same day as
`RESUME-2026-08-13-poll-cadence.md`. **This note supersedes that one.** Read
this first; read that one after, for the history and the gotchas it lists
(they are all still true). Two of its conclusions are corrected below.

## What changed today

1. The measurement it was waiting for has been taken. **The hypothesis was
   right**: the poll fires about once a second on real hardware.
2. A **separate, pre-existing bug** turned up while testing: an unguarded
   test-and-set on `sc->dp_timer` leaks kernel callouts. Under IRIS it panics
   5.3 in about a second. It is present at HEAD, i.e. in the code running on
   the Indigo right now.
3. The stakes changed. **The Indigo has no working `ec0` — `dp0` is its only
   network interface.** Every remote workflow, including the Rust/RustDesk
   port that is the actual goal, rides on this driver. Throughput is now on
   the critical path rather than a curiosity.

## The measurement (settled — do not re-take)

Taken from a Mac on the same LAN against the Indigo at `192.168.99.45`
(DaynaPort MAC `0:80:19:53:96:e6`; the Mac was `192.168.99.104`).

| probe | result |
|---|---|
| `ping` (1/sec, fire-and-forget) | 549 / 560 / 571 ms, 0% loss — **avg 553 ms** |
| TCP connect to a closed port (SYN in, RST out) | **min 1017 ms**, median 1449, max 2797 |

The prior note predicted ~500 ms average if collections happen once per second
and packets arrive at random phase, and set "all RTTs under ~50 ms" as the
threshold that would exonerate the poll. Measured average 553 ms. The TCP
minimum of ~1.0 s shows the full period: an inbound SYN waits up to a whole
second for a collection, while the outbound RST leaves immediately.

**`itimeout(..., HZ/100, plbase)` is not delivering 10 ms on IP20.**

Two caveats on method, so nobody is misled by the numbers above:

- The TCP loop waits for each result before starting the next, so it
  self-synchronises to the collection cycle and clusters instead of spreading
  evenly. The clustering is an artifact of the sampler. The ping average is
  the clean number.
- The textbook version (`sudo ping -i 0.2`, RTTs spread evenly over 0–1000 ms)
  still has not been run — sub-second intervals need root. It would add
  confidence but is unlikely to change the conclusion.

**Corollary that kills a rival theory.** At HEAD the interface watchdog does
*not* collect packets — that is only in the `dangerous` branch below. So
nothing other than the poll could be servicing those packets at 1/sec. The
chain is alive and firing about once a second; it has not lapsed. That rules
out "the chain lapsed and something else revives it" and leaves the timer
itself.

## The callout leak (new, and it is on the Indigo)

`dp_async_arm()` is a test-and-set with nothing guarding it:

```c
if (sc->dp_enabled && sc->dp_timer == 0)
    sc->dp_timer = itimeout((void (*)())dp_timer_kick, (void *)sc, HZ/100, plbase);
```

At HEAD it is reached from two priority levels — timeout context via
`dp_timer_kick()`, and the SCSI completion interrupt via `dp_async_done()`.
`dp_timer_kick()` clears the handle *before* arming, so a completion landing in
that window sees zero, arms a second callout, and one of the two handles is
stored over the other. The orphan is still pending, and because every callout
re-arms itself, **each orphan becomes an independent poll chain. They multiply
rather than accumulate**, and the callout table is gone in seconds:

```
PANIC: Timeout table overflow.
 Tune ncallout, callout_himark and reserve_ncallout to higher values.
```

Reproduced under IRIS on both trees, probe on and probe off:

| tree | panics after |
|---|---|
| HEAD (`e75553b`) | 3 pings |
| `dangerous` (arm from every entry point) | 1 ping |

**Reproduction detail that matters:** `irix5.3/ci/iris-irix53-dayna.toml` has no
`latency_us`, so completions return immediately and land inside the window
constantly. Adding `latency_us = 3000` hides it — which is why every earlier
ladder run looked clean and why the previous note recorded the arm-everywhere
change as "passes the ladder". Zero latency is unlike hardware, but here it is
the setting that *exposes* a real bug rather than inventing one.

**The fix** is an `splhi()`/`splx()` around the test-and-set. Guarding only
that is sufficient: a completion either finishes its arm before we test (we
see non-zero and skip) or cannot run during it. Not yet written.

**Unknown, and worth treating as unknown:** whether the Indigo is leaking too.
Real hardware completions are far slower than 3 ms, so it should hit the window
much more rarely — but "rarely" over a multi-hour build is not "never", and
that machine has never been left up under sustained load. The honest check is
a soak, not an argument. Do not assume it is safe merely because it has not
panicked yet.

**Do not connect the leak to the 1/sec poll without evidence.** A leak makes
*more* callouts fire, not fewer, so on its face it does not explain a slow
poll. Keep them separate until the fix lands and the tick-rate probe reports a
real number from the Indigo.

## Corrections to `RESUME-2026-08-13-poll-cadence.md`

- **"Completions are being lost" is not as dead as recorded.** It was ruled out
  partly because the stall-guard log line has never been seen. But `dp_stall`
  is counted in *poll ticks* (`DP_STALL_TICKS 200`), and it resets on every
  non-busy poll. If the poll really runs at ~1/sec the guard needs ~200 seconds
  of continuous stall to fire, not 2. Its silence is circular evidence — the
  thing under suspicion is the very clock the guard is calibrated in.
- **Candidate fix #1 needs restating.** "Raise the priority level in the
  `itimeout()` call" is worth trying, but the mechanism is not "higher priority
  is serviced sooner": the `pl` argument is the level the callback *runs at*.
  The reason it might help is that it can move the callout off a deferred
  base-level queue onto the clock-interrupt dispatch path. Same experiment,
  different reasoning — and it means the experiment could fail for reasons that
  say nothing about `plbase`.

## Branch `dangerous` (unpushed)

Created today off `irix-53`, holding the work that was uncommitted in the tree
plus a new diagnostic:

```
e4a11e2  irix5.3: measure the poll's real rate against the interface watchdog
652ecc1  irix5.3: arm the poll from every entry point — makes an existing panic worse
e75553b  (irix-53) docs: resume note - the poll fires ~1/sec on real hardware
```

- `652ecc1` is the old "arm from every entry point" change. It does **not**
  introduce the leak; it roughly triples the rate of death. Do not install it.
- `e4a11e2` is a tick-rate probe, inert unless built with `-DDP_LOG_TICKRATE`.
  It counts poll callbacks, submissions and completions and prints them once a
  second from `dp_eio_watchdog()`, which the ifnet layer drives at
  `IFNET_SLOWHZ` regardless of our own timer — the one clock in the driver
  whose rate can be trusted:

  ```
  dp0: 1s tick=100 sub=100 done=100 rx=2 busy=0 fg=0 stall=0
  ```

  `tick=~100` means the poll is healthy and the latency is elsewhere; `tick=~1`
  means `itimeout()` is not delivering; `tick=0` means the chain has lapsed. A
  ping test cannot separate those three and they have different fixes. **This
  probe has never printed a line yet** — every run so far panicked before the
  first watchdog tick. It cherry-picks cleanly onto `irix-53` without dragging
  the panic along.

Both commits compile: 5.3 native o32 built + booted, 6.5 native n32 MIPSpro
compiled, `drift.sh` clean at each.

## The plan

In order. 1 is correctness; 2 is the one that makes the link usable.

1. **`splhi()`/`splx()` around the test-and-set in `dp_async_arm()`.** Small,
   and a prerequisite for trusting any further hardware run.
2. **Keep chaining while the interface is busy**, not only while the device
   reports MORE (the prior note's candidate #2). A completion that immediately
   re-submits sidesteps the broken timer altogether, so throughput stops
   depending on a callout that fires once a second. Gate it on recent activity
   so an idle link does not burn SCSI bandwidth. **This is independent of ever
   understanding why `itimeout` misbehaves on IP20** — which may stay unsolved.
3. **Raise the `++sc->dp_chain < 8` bound.** One line, multiplies frames per
   poll until 2 lands.
4. Then, optionally, the `pl` experiment for the cadence itself, with
   `-DDP_LOG_TICKRATE` built in so one install cycle yields a real number.

## Why throughput now matters: the Rust port

The goal is a RustDesk port to IRIX, via `mrustc` (Rust → C, compiled natively
on the target) — the same approach as `~/repos/rust-ppc-tiger*` for PowerPC
Tiger. Most of the IRIX Rust std is already written.

That means moving hundreds of megabytes of generated C, repeatedly, to a
machine whose only wire is `dp0`. At ~1 poll/sec, with one READ per tick
chaining to at most 8 and `DP_RX_BUFLEN` sized for two packets per response,
inbound tops out around **10–25 KB/s**. Hundreds of megabytes is hours, and
TCP will be fighting retransmit timers throughout.

**So do not put bulk transfer on the network.** Use the BlueSCSI:

- `EnableUSBMassStorage` defaults to **true** in BlueSCSI V2 firmware
  (`src/BlueSCSI_settings.cpp`), so plugging its USB into the Mac mounts the SD
  card as mass storage with no config change.
- Build an EFS image on the Mac with `rb-cli new hd sgi-efs --from-dir` — the
  same call `shared/scripts/iris-build.sh` already uses as its transfer medium — drop
  it on the card, unplug, and the Indigo sees a new disk.
- Tradeoff: MSC mode takes the BlueSCSI off the SCSI bus, so the Indigo must be
  idle while loading. A batch loop, not a live mount. Still orders of magnitude
  better than 12 KB/s.

Network then only has to carry a shell, which lowers the bar for sshd a lot.

## Remote access into the Indigo (state as of today)

- **telnet works.** `brew install telnet` on the Mac (macOS has shipped no
  telnet since High Sierra; `nc` cannot do it — it does not answer the IAC
  option negotiation, which is the garbage you get instead of a login prompt).
  Verified reaching `IRIX System V.4 (IRIS)` / `login:`.
  Expect ~0.5 s per keystroke: `telnetd` echoes remotely, so every character is
  a round trip. Fine for short commands, miserable for `vi`.
- **Open ports:** 21 ftp, 23 telnet, 79 finger, 512/513/514 r-services, 6000 X.
  **22 is closed — there is no sshd.** The tgcware ssh package really is
  client-side only.
- **sshd, if wanted:** OpenSSH genuinely supports IRIX (`--with-irix-array`,
  `--with-irix-project`, `--with-irix-audit`) and OpenSSL has `irix-cc` /
  `irix-mips3-cc` targets, but current versions need C99 + OpenSSL 3.x. The
  realistic pairing is OpenSSH 4.x–5.x + OpenSSL 0.9.8, likely built with gcc
  rather than IDO `cc`, and modern clients then need
  `-oKexAlgorithms=+diffie-hellman-group1-sha1 -oHostKeyAlgorithms=+ssh-rsa`.
  **Dropbear** is the lighter answer — SSH-2, bundles its own crypto, no
  OpenSSL build; no `sftp-server`, which does not matter once bulk goes over
  SCSI. **Do not use ssh-1.2.33 as a server**: OpenSSH dropped SSH-1 in 7.0, so
  nothing modern could connect.
- **X11 / xdm:** the Indigo's xdm answers XDMCP and is `WILLING` ("IRIS",
  "Willing to manage"), so no `xdm-config` change is needed. But **XQuartz
  cannot host the session**: it runs rootless under `quartz-wm` and an XDMCP
  session paints its greeter onto a root window that does not exist, so
  `Xquartz :1 -query …` looks like a freeze. Use per-app forwarding instead
  (`DISPLAY=<mac>:0 xterm &`, do not start `4Dwm` — it would fight
  `quartz-wm`). For a real desktop, `Xvnc :1 -query localhost` on the Indigo
  viewed over VNC is the route; VNC also degrades far better than X11 on a slow
  link, since it is not round-trip-per-operation.
  **Already done on the Mac:** `defaults write org.xquartz.X11 nolisten_tcp
  -bool false` (XQuartz was refusing the TCP callback xdm needs). It requires
  an XQuartz restart to take effect.

## IRIS (the emulator) — `~/repos/iris`

Unchanged today. Branch `add-daynaport`, PR #82 merged, **two unpushed commits
on top** (`610b3e9` docs, `2c24394` fidelity). Agreed with Dani: hold them, and
cut a **new branch off upstream main** when ready rather than reusing
`add-daynaport`.

Standing warning, reinforced twice today: **the emulator flatters the driver.**
Every hardware bug has passed the ladder first. And note the inverse now also
holds — the zero-latency config exposed a real bug that 3 ms latency hides. A
green ladder means "not obviously broken", never "works".

## How to run things

See the prior note for the full command set and the gotchas (absolute
`--config`, the 5.3 master file, clean shutdown before reboot, board names,
`drift.sh` not being a compile check). All still accurate. The short version:

```sh
cd ~/repos/irixdayna
./shared/scripts/iris-build.sh --release 5.3 --boot-test --fresh \
    --cflags "-DDP_LOG -DDP_CHECK_ETHERIF -DDP_LOG_TICKRATE" \
    --config /Users/dani/repos/irixdayna/irix5.3/ci/iris-irix53-dayna.toml \
    --workdir /tmp/dpX --outdir /tmp/distX
./shared/scripts/dp-ladder.sh --release 5.3 \
    --config /Users/dani/repos/irixdayna/irix5.3/ci/iris-irix53-dayna.toml \
    --work-hda /tmp/dpX/work.hda
```

Console log of a ladder run lands in `/tmp/dpladder.*/console.log`. **Read the
whole log, not the tail around the ping output** — that is how "HEAD is clean"
got claimed today when HEAD in fact panicked three pings in.
