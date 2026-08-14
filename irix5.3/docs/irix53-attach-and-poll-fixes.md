# Task: make the IRIX 5.3 DaynaPort driver attach and survive `ifconfig up`

> **DONE — kept for the reasoning, not as a work item.** Everything described
> below was implemented in commits `3b9ab26`, `c4b328d`, `a3f5f47` and
> `d1c3cf5`. 5.3 now attaches, reads its MAC, resolves ARP and pings 4/4 under
> emulation. Two things turned out differently from the guesses in §4: the
> `ifconfig up` panic was `ifptoeif()` in the watchdog, not the shared
> `scsi_request_t` (splitting that fixed nothing and was reverted), and the
> poll had to become fully asynchronous because 5.3 ships no kernel-thread API.
> Current state: `irix5.3/RESUME.md` §1.

Hand this to an agent (or a human) working in the **`irixdayna`** repo, branch
`irix-53`. It is written against the driver as of 2026-08-12 and every claim
below was observed on a real run, with the console output quoted.

---

## 1. What changed since `irix5.3/RESUME.md` was written

RESUME says the driver "compiles and links into a working kernel… It has never
moved a packet", and that functional testing needs "either real hardware with a
BlueSCSI/ZuluSCSI, or a DaynaPort device model added to the emulator."

**That device model now exists.** IRIS has a DaynaPort SCSI/Link target, built
from `docs/iris-daynaport-target.md`:

```sh
cd ../iris && cargo build --release --features lightning,chd,daynaport
```

Select it per SCSI id in the machine config:

```toml
[scsi.3]
kind = "daynaport"            # optional: mac = "...", subnet = "192.168.10.0/24"
```

It runs its own NAT gateway on 192.168.10.0/24 (gateway `.1`, guest `.2`),
separate from `ec0`'s. See `docs/daynaport.md` in the IRIS repo. `scsi dayna` on the IRIS
monitor console (telnet 127.0.0.1:8888) shows its MAC, addresses and counters.

So the driver can now be exercised past `dp_init()` for the first time. Doing
that immediately found two bugs. **Neither is in the emulator** — both were
simply unreachable while nothing ever answered the INQUIRY.

## 2. What already works (don't re-litigate these)

Run with a DaynaPort at target 3, IRIX 5.3 booted single-user from a kernel
linked by `shared/scripts/iris-build.sh --release 5.3 --boot-test`:

- IRIX's own SCSI scan sees the target and types it correctly —
  `hinv` prints `Processor: unit 3 on SCSI controller 0`.
- With fix (1) below applied, the driver attaches:

  ```
  dp: 0/3/0 type 3 vendor='Dayna   ' product='SCSI/Link       '
  dp: found DaynaPort at 0/3/0, attaching
  dp0: calling ether_attach
  dp0: DaynaPort SCSI/Link at scsi(0) target 3 lun 0
  ```

- `ether_attach()` is reached with `-DDP_CHECK_ETHERIF` compiled in and the
  canary **does not trip**. The `sgi_ether.h` reconstruction is correct — that
  open question in RESUME §"struct etherif layout risk" can be closed.
- `RETRIEVE STATISTICS` works and the MAC is real, not the placeholder:

  ```
  dp0: get_mac → scsi_cmd op=0x9 buflen=18 → status=0
  dp0: MAC 0:80:19:44:50:3
  dp0: enabling interface → op=0x0e → status=0
  dp0: set_mode (broadcast) → op=0x0c → status=0
  dp0: eio_init done, rx poll started
  ```

  So `0x09`, `0x0C` and `0x0E` are all confirmed against a real bus, and
  `dp_scsi_cmd()`'s request setup is sound for the no-data and data-in cases.

## 3. Bug 1 — `dp_scan_bus()` runs before the SCSI layer can answer

**Symptom.** `dp_init()` prints its banner and the scan finds nothing, silently:
every early return in `dp_probe_one()` is unlogged, so the console shows only
the banner. It looks exactly like "no DaynaPort present."

**Cause.** Instrumenting each early return gives:

```
dp: probe 0/0/0: drvnum=1 but no target info
dp: probe 0/1/0: drvnum=1 but no target info
...
dp: probe 1/0/0: no adapter (drvnum NULL)
```

The adapter *is* registered (`drvnum=1`), but `scsi_info()` returns NULL for
every target: nothing has cached an INQUIRY yet. The generated
`/var/sysgen/master.c` says why —

```c
void (*io_init[])() = {
    qcntlinit, vino_init, vidinit, dp_init, ubusinit, ..., dsinit, dkscinit, ...
};
void (*io_start[])() = { 0 };
```

`dp_init` runs **4th**, ahead of `dsinit`/`dkscinit` and the host adapter's own
bus scan. The `DEPENDENCIES scsi` line in `master.d/dp` guarantees the driver
and its function-pointer arrays are *present*, not that the bus has been
*scanned* — the comment in `master.d/dp` overstates what it buys.

**Fix.** Scan from the `io_start[]` phase instead, which lboot populates from a
`<prefix>start` symbol and the kernel calls after all of `io_init[]` and after
device configuration — the first point where `scsi_info()` can answer and where
sleeping is legal:

```c
void
dp_start(void)
{
    if (dp_nunit == 0)
        dp_scan_bus();
}
```

Confirmed: adding it makes lboot emit `io_start[] = { dp_start, 0 }`, and the
start-phase scan finds the target (output in §2). No `master.d` change needed.

Decide what `dp_init()` keeps doing — the banner alone is probably right; a scan
there can only ever find nothing.

**Also fix regardless:** make the early returns in `dp_probe_one()` log under
`DPLOG`. A scan that finds nothing and says nothing cost an entire debugging
session to tell apart from a scan that never ran.

## 4. Bug 2 — the RX poll races the control path, and panics the kernel

**Symptom.** With the driver attached, `ifconfig dp0 192.168.10.2 netmask
0xffffff00 up` panics the guest:

```
dp0: eio_init done, rx poll started
dp0: eio_reset
dp0: disabling interface
dp0: scsi_cmd op=0xe buflen=0 dir=0x0
PANIC: KERNEL FAULT
EXC code:128, `Software detected SEGV '
Bad addr: 0x0, cause: 0x10000008<CE=1,EXC=RMISS>
```

**Cause.** The ether layer calls `eio_reset()` right after a successful
`eio_init()`. `dp_eio_reset()` → `dp_runqueue_stop()`, which does:

```c
sc->dp_enabled = 0;
mutex_lock(&sc->dp_qlock, PZERO);
sc->dp_timer = 0;   /* by now timer has either fired or been cancelled by runqueue */
mutex_unlock(&sc->dp_qlock);
```

That comment is false. `dp_runqueue()` re-arms `sc->dp_timer` via `itimeout()`
at the end of *every* poll, so the timer is armed far more often than not, and
dropping the handle without `untimeout()` leaves a callback pending. Meanwhile
**every** command — foreground control and background poll alike — reuses the
single `sc->dp_req` and `sc->dp_sema`, and `dp_scsi_cmd()` opens with
`bzero(req, sizeof *req)`. A completion that lands on a request another caller
has just zeroed finds `sr_dev == NULL`, and `dp_scsi_done()` does
`vsema(&sc->dp_sema)` through it — a read at address 0, which is exactly the
fault above.

**This is not the emulator making things fast.** The window is wider here (an
emulated READ returns immediately, so the poll re-arms constantly) but the race
is structural and would bite on real hardware too, just more rarely.

**Fix, in two parts.**

1. Actually cancel the timer:

   ```c
   if (sc->dp_timer) { untimeout(sc->dp_timer); sc->dp_timer = 0; }
   ```

   Necessary, not sufficient — see below.

2. Deal with the deeper problem: **`dp_runqueue()` issues SCSI commands from an
   `itimeout()` callback, and `dp_scsi_cmd()` sleeps in `psema()`.** Sleeping in
   timeout context is not legal on 5.3. Suppressing the reset bounce to get past
   the first panic just moves it:

   ```
   dp0: eio_reset: already up, skipping bounce
   dp0: eio_init
   dp0: get_mac
   dp0: scsi_cmd op=0x9 buflen=18 dir=0x1
   Kernel/Interrupt Stack Overflow @0x0 sp:0x881aa5e8
   PANIC: stack underflow/overflow
   ```

   — a re-entered `eio_init` on top of the running poll overflowing the
   interrupt stack.

   The polling loop needs to run in a context that may sleep. Options worth
   weighing: a dedicated kernel thread (`sthread_create`) woken by the timer,
   with the timer callback doing nothing but the wake; or keeping the timer but
   making the poll fully asynchronous (no `psema` — drive everything from
   `sr_notify` completions, with a per-request `scsi_request_t` rather than one
   shared `sc->dp_req`). The shared request has to go either way: one in-flight
   request per softc cannot serve both a 10 ms poll and a foreground ioctl.

   Check what the 6.5 driver does here before choosing — the two are supposed to
   stay in sync across the `BEGIN/END SHARED` markers, and this code is inside
   them.

## 5. A patch you can start from

`../iris` produced a scratch patch while diagnosing. It contains fix (1) in §3,
part 1 of fix (2) in §4, the `DPLOG` additions, and a **test-only** hack that
skips the `eio_reset` bounce (labelled as such — it is not a fix). It was
applied to a copy, never to this repo. Ask for
`irixdayna-driver.patch` from that session, or just re-derive it — it is ~45
lines and every hunk is quoted above.

## 6. How to verify

```sh
shared/scripts/iris-build.sh --release 5.3 --boot-test --fresh \
    --config <a copy of irix5.3/ci/iris-irix53.toml with a [scsi.3] daynaport target>
```

Rung 1 is the `dp0: DaynaPort SCSI/Link at scsi(0) target 3 lun 0` line, and
rung 2 is the MAC. Past that, boot single-user and drive it by hand over the
serial console:

```
/usr/etc/ifconfig dp0 192.168.10.2 netmask 0xffffff00 up
/usr/etc/ping -c 4 192.168.10.1
/usr/etc/arp -a
/usr/etc/netstat -in
```

Rung 3 (ARP resolves) is where the emulator's RX record format gets its first
real test. If ARP resolves but ping does not, suspect `pktlen` off by the 4 CRC
bytes; if nothing resolves, suspect byte order in the record header or broadcast
being filtered. `-DDP_LOG_NET` plus IRIS's `eth_summary()` traces gives both
ends of every frame.

The boot disk is never written — the iris `*.toml` configs set `overlay = true`, so guest
writes land in `<image>.chd.diff.chd`; `--fresh` resets it.
