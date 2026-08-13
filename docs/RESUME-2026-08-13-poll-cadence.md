# Resume: DaynaPort on IRIX 5.3 — the poll fires ~1/sec on real hardware

Handoff written 2026-08-13, mid-investigation. Read this in full before
touching anything; several plausible-looking theories are already dead and are
recorded here so nobody re-derives them.

## Where things stand

**It works.** An Indigo R4000 (IP20) running IRIX 5.3 with a **BlueSCSI V2** at
SCSI id 3 attaches `dp0`, reads its MAC from the device, and passes traffic in
both directions with no loss. It also works in the IRIS emulator against the
emulated DaynaPort target, on both IRIX 5.3 and 6.5.

**The open problem is latency, not function**, and it is one-sided:

| direction | measured on hardware |
|---|---|
| Indigo → router (guest transmits) | consistently **< 100 ms** |
| PC → Indigo (guest idle) | **~500 ms** |

## The live hypothesis

Outbound is fast because a transmit *kicks the packet engine*, so the reply is
collected by an already-active poll. Inbound is slow because nothing kicks it:
the frame waits for a poll to run on its own.

~500 ms is the average wait if collections happen **once per second** and
pings arrive at random phase (range 0–1000 ms). So:

> `itimeout((void (*)())dp_timer_kick, (void *)sc, HZ / 100, plbase)` is not
> delivering a 10 ms tick on this hardware. It appears to deliver roughly one
> per second.

This also explains the very first hardware symptom (replies batched
1900/900/15 ms, repeating, 0% loss) and why **the emulator has never
reproduced any of it** — the same code gives 15–28 ms RTT under IRIS, so IRIS
dispatches these timeouts far more promptly than the real machine.

Suspect `plbase`: if base-level callouts are serviced by a slow or
low-priority path on that kernel, the requested delay is irrelevant.

### The measurement that settles it

No reinstall needed. From a Mac/Linux box on the same LAN:

```sh
sudo ping -i 0.2 <indigo>
```

- RTTs spread roughly evenly over 0–1000 ms → collections are 1/sec →
  hypothesis confirmed.
- All RTTs under ~50 ms → the poll is fine and the 500 ms is something else.

**This measurement had not been taken when this note was written.**

### If confirmed, candidate fixes (none tried)

1. Raise the priority level in the `itimeout()` call. `dp_timer_kick` in async
   mode only *submits* a command — it never sleeps — so it is legal at a
   higher pl than `plbase`.
2. Stop depending on tick granularity: chain another READ straight from the
   completion whenever traffic is flowing, and fall back to the timer only
   when genuinely idle. Costs SCSI bus bandwidth; the Mac drivers do something
   like this.
3. Drive the poll from the interface watchdog as well (already done, but it is
   1/sec so it cannot beat a 1/sec problem).

## Dead theories — do not re-derive

Read `../BlueSCSI-v2/lib/SCSI2SD/src/firmware/network.c` before theorising
about the device; it is checked out locally and it is authoritative.

- **"BlueSCSI blocks a READ when idle."** It does not. With an empty queue it
  writes six zero bytes and returns GOOD immediately.
- **"Completions are being lost."** BlueSCSI always finishes the command and
  sets GOOD. (A stall guard was added anyway — see below — and its log line
  has not been observed firing.)
- **"Device latency breaks the driver."** IRIS can now model per-command
  latency (`latency_us`). The ladder passes at 0 ms, 3 ms and 20 ms, with RTT
  scaling as expected. Latency alone is not the trigger.

## What was fixed getting here (all committed)

`irixdayna`, in order — see each commit message for the evidence:

| commit | fix |
|---|---|
| `3b9ab26` | bus scan moved to `dp_start()`/`io_start[]`; watchdog no longer uses `ifptoeif()` (that cast panicked the kernel: `Bad addr 0x0`, 16 bytes into `dp_eio_watchdog`) |
| `c4b328d` | `mutex_trylock` in the poll tick |
| `a3f5f47` | the asynchronous packet engine — 5.3 cannot sleep in an `itimeout` callback |
| `d1c3cf5` | `DP_ASYNC_RX` on by default in `irix5.3/Makefile` |
| `026abec` | stray `static int` that made the **6.5** driver uncompilable |
| `40c9060` | armed the poll from timeout context only — **this one went silent on hardware** |
| `8470494` | stall recovery + both re-arms restored + probe logging behind `-DDP_LOG_PROBE` |
| `bc51125` | per-run CHD overlays for the test scripts |

**Uncommitted in the working tree**: `if_dp.c` and `irix5.3/if_dp.c` carry an
"arm from every entry point" change (one idempotent `dp_async_arm()`, called
from tick/poll/completion, plus a watchdog kick). It builds and passes the
ladder at 3 ms latency (4/4, 71/190/424 ms) but was **not** installed on
hardware, and it will not help if the 1/sec hypothesis is right. Commit or
discard on the strength of the measurement above.

## IRIS (the emulator) — `~/repos/iris`

Branch `add-daynaport`. **PR #82 is merged**, and there are **two unpushed
commits** on top (`610b3e9` docs, `2c24394` fidelity). Agreed with Dani: hold
them, and when ready cut a **new branch off upstream main** rather than
reusing `add-daynaport`.

`2c24394` made the emulated target behave like BlueSCSI after reading its
firmware: pad short frames to 60 bytes before CRCing (a 42-byte ARP reply
reaches the guest as pktlen 64, not 46), write a real Ethernet FCS instead of
zeros, and support `latency_us` per target. Already-correct beforehand:
`pktlen` including the CRC, MORE as `0x00000010` big-endian in bytes 2–5, and
the six-zero-byte idle response.

**The emulator flatters the driver.** Every hardware bug in this list passed
the emulator first. Treat a green ladder as "not obviously broken", never as
"works".

## How to run things

```sh
# build + boot test (5.3), then walk the ladder
cd ~/repos/irixdayna
./scripts/iris-build.sh --release 5.3 --boot-test --fresh \
    --cflags "-DDP_LOG -DDP_CHECK_ETHERIF" \
    --config /Users/dani/repos/irixdayna/ci/iris-irix53-dayna.toml \
    --workdir /tmp/dpX --outdir /tmp/distX
./scripts/dp-ladder.sh --release 5.3 \
    --config /Users/dani/repos/irixdayna/ci/iris-irix53-dayna.toml \
    --work-hda /tmp/dpX/work.hda

# hardware kit: objects per board + an EFS CD to mount on the target
./scripts/mk-dp-cd.sh --rb-cli /Users/dani/bin/rb-cli     # -> dist/dp-irix53.iso
./scripts/iris-build.sh --release 5.3 --cpuboard IP20 ... # cross-build a board
```

Gotchas that have each cost a cycle:

- **`--config` must be absolute.** The script `cd`s to the workdir. IRIS now
  errors out instead of silently booting a default machine.
- **Model latency when testing timing**: add `latency_us = 3000` to the
  `[scsi.3]` block. Zero latency is unlike any hardware.
- **Never edit `/var/sysgen/system/irix.sm`** — mode 444, appends fail
  silently. `smake install` writes `/var/sysgen/system/dp.sm` instead.
- **The 5.3 master file is `irix5.3/master.d/dp`**, not the 6.5 one in the repo
  root. The 6.5 file (`nscR`, `+thread_class`) makes 5.3's `autoconfig` fail
  with parse errors.
- **Shut the guest down cleanly** (`/etc/shutdown -y -g0 -i6`); `autoconfig`
  stages `/unix.install` and only a clean shutdown promotes it. A hard reset
  boots the old kernel and looks exactly like a broken driver.
- **`drift.sh` proves the two drivers agree, not that either compiles.** 5.3's
  o32 `cc` is more permissive than 6.5's n32 MIPSpro — a shared-region change
  needs a 6.5 compile before it counts as done.
- Board names mislead: Indigo R4000 = **IP20**, Indigo R3000 = IP12, Indy =
  IP22. A wrong-board object neither fails to link nor necessarily crashes; it
  just gives you no interface.

## Also open, low priority

- **`?? dp0 ??`** in IRIX's Network Setup pulldown. Comes from
  `add_to_inventory(..., INV_ETHER_DP, ...)` with `INV_ETHER_DP = 43`, a made-up
  controller type IRIX has no name for. Unclear whether the name table is a
  data file (fixable) or compiled into the tools (only fixable by claiming to
  be hardware we are not — ask before doing that).
- **6.5 ping is 1/4** and always has been, including before any of this work.
  Same 10 ms poll design. The async engine that fixed 5.3 is in the tree behind
  `DP_ASYNC_RX` and is not enabled for 6.5.
