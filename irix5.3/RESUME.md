# RESUME — IRIX 5.3 port, troubleshooting handoff

Pick-up notes for continuing work on the IRIX 5.3 port. Written 2026-08-12 on
branch `irix-53`. If you are an AI assistant resuming this work, read this file
in full before touching anything — it records decisions and their reasons, and
several obvious-looking "improvements" are wrong for reasons captured here.

---

## 1. State of the work

**The driver compiles and links into a working kernel on IRIX 5.3/IP22. It has
never moved a packet.**

Verified, by actually running it:

- Compiles clean with SGI's own `cc` and the exact IP22 kernel CFLAGS, inside
  an emulated Indy (`scripts/iris-build.sh --release 5.3`).
- `autoconfig -f` links a real kernel with the driver in it — every symbol
  resolves (`--autoconfig`).
- That kernel **boots**, and `dp_init()` runs its SCSI bus scan against the
  emulated WD33C93 without panicking, printing
  `NOTICE: dp: DaynaPort SCSI/Link driver (IRIX 5.3)` (`--boot-test`).
- lboot accepts `master.d/dp`: the generated `/var/sysgen/master.c` carries
  `dp_init` in the boot init table and `dp_open`/`dp_close`/`dp_ioctl` in the
  cdevsw entry. That settles the master-file flags question.
- The drift checker works in both directions (tested by perturbing `dp_do_rx`
  and confirming it produces a diff, then restoring).
- 501 lines are byte-identical between `irix5.3/if_dp.c` and `../if_dp.c`.
- The 6.5 driver is untouched — `git diff main -- if_dp.c Makefile master.d/`
  is empty.

**Not** verified: that it moves a packet, or that `struct etherif` is right.
IRIS has no DaynaPort SCSI target, so `dp_do_attach()` — and therefore
`ether_attach()` and the `DP_CHECK_ETHERIF` canary — is never reached. A green
pipeline run is not "it works".

### Repo layout

```
if_dp.c            6.5 driver — DO NOT EDIT for 5.3 reasons
Makefile           6.5
master.d/dp        6.5
sgi_ether.h        6.5 (revision 1.15, has INET6)
irix5.3/
  if_dp.c          5.3 driver, ~1090 lines
  sgi_ether.h      5.3 reconstruction (INET6 member removed)
  Makefile         o32/-coff, static only
  master.d/dp      flags "cs", no +thread_class
  drift.sh         shared-region checker
  README.md        build + install + pre-boot checklist
  RESUME.md        this file
```

### Git / PR state

- Branch `irix-53`, forked from `danifunker/irixdayna`, upstream is
  `techomancer/irixdayna` (remote `upstream` is configured).
- Fork was in sync with upstream at the time of writing.
- **Nothing has been pushed. No PR has been opened.**
- Intended PR: `danifunker:irix-53` → `techomancer:main`.
- Recommendation was to file an upstream issue first to check appetite, since
  upstream is only four commits old and actively developed.

---

## 2. Design decisions and why (do not silently reverse these)

### 2.1 Two trees, not a compat layer

An earlier plan proposed a shared `if_dp.c` plus `dp_compat.h` and per-OS
backends. It was rejected after measuring the actual split: only about a third
of the 6.5 driver is shareable, the rest is discovery/attach/locking/module
plumbing with no counterpart across releases. Abstracting ~600 divergent lines
to save ~500 shared ones would be more `#ifdef` than code.

The 6.5 tree deliberately stays at the repo root rather than moving to
`irix6.5/`. This makes the PR touch zero existing files except `README.md`,
which matters because upstream is mid-development and a relocation would
conflict with whatever they are editing.

### 2.2 `etherif`, not raw `ifnet`

SGI's 5.3 documentation only ever describes the raw `ifnet` interface, and an
earlier draft of the plan recommended targeting it. **That recommendation was
reversed after reading the 5.3 skeleton's `sk_output`.** Going raw means
hand-writing `if_output` with `ip_arpresolve` across `AF_INET` / `AF_UNSPEC` /
`AF_RAW` / `AF_SDL`, plus manual mbuf header prepending and `if_snd` queue
management — roughly 250 lines of intricate pointer arithmetic that cannot be
tested without hardware. `ether_attach` provides all of it.

**`ether_attach` is confirmed to exist** in a stock 5.3 IP22 `/unix`, along
with `ether_input`, `ether_stop` and `ether_selfsnoop` — which is what
validated this decision. (`ether_detach`/`ether_reattach` are absent, as
expected: they exist only for 6.x loadable drivers.)

The trade: `sgi_ether.h` is still a reconstruction, and its *layout* remains
the port's single largest risk. See §5.1.

### 2.3 The 6.5 API shim

`irix5.3/if_dp.c` opens with ~30 lines mapping 6.5 spellings onto 5.3
primitives rather than scattering `#ifdef`s through the protocol code:

| 6.5 | 5.3 |
|---|---|
| `mutex_lock(m, pri)` | `psema(m, pri)` |
| `mutex_unlock(m)` | `vsema(m)` |
| `mutex_trylock(m)` | `cpsema(m)` |
| `mutex_init(m, type, name)` | `initnsema_mutex(m, name)` |
| `mutex_destroy(m)` | `freesema(m)` |
| `init_sema(s, v, name, unit)` | `initnsema(s, v, name)` |
| `SN_MORETOCOME` | 0 — does not exist on 5.3 |

The signatures line up almost exactly, which is *why* 501 lines can stay
byte-identical — including all five `etherif` handlers, which a naive port
would have had to fork.

Three subtleties, all confirmed against the real headers, none of which should
be undone:

- There is **no `typedef sema_t mutex_t`**. 5.3 spells the Sun-compat alias
  `kmutex_t`, not `mutex_t`. The softc declares `sema_t` directly.
- `#undef mutex_init` is **load-bearing, not defensive.** `sys/sema.h` really
  does define it, with a different four-argument shape
  `mutex_init(m, nm, f, i)` — name second, not third. Without the `#undef` the
  6.5-style three-argument call expands wrongly.
  (`mutex_lock`/`mutex_unlock`/`mutex_trylock` are *not* defined by 5.3 — it
  uses `mutex_enter`/`mutex_exit` — so those `#undef`s are belt-and-braces.)
- `SN_MORETOCOME` does not exist on 5.3; `net/raw.h` has only `SN_PROMISC`,
  `SN_ERROR`, `SN_TRAILER` and the `SNERR_*` codes. It is an advisory batching
  hint OR'd into the snoopheader, so 0 is the right equivalent — we lose an
  optimisation on multi-packet READs and nothing else.

### 2.4 The shared-region contract

`drift.sh` extracts the region between the `BEGIN SHARED` / `END SHARED`
markers and diffs it against the corresponding region of `../if_dp.c`. Both
extractions anchor on the same text (`/* dp_scsi_cmd_locked`), so the 6.5 file
needs no markers of its own — that is deliberate, to keep the PR from touching
it.

**A protocol fix belongs in both files.** `dp_do_rx` especially: it is where
emulator-specific bugs will surface as BlueSCSI / PiSCSI / SCSI2SD get tested.

If you edit the shared region, run `sh irix5.3/drift.sh` before committing. If
you *intend* to diverge, do not weaken the checker — move the code out of the
shared region and document why.

---

## 3. How to build

### 3.0 The fast path: build it in the emulator

```sh
scripts/iris-build.sh --release 5.3              # compile
scripts/iris-build.sh --release 5.3 --autoconfig # + link a kernel
scripts/iris-build.sh --release 5.3 --boot-test  # + boot it
```

This compiles the driver natively inside an emulated Indy and drops
`dist/dp-irix53.o` on the host. It is adapted from the identical pipeline in
`../irixscsitb` and shares its disk images via `ci/local.conf` (this repo's
copy, else `../irixscsitb/ci/local.conf`). Needs `iris` + `iris-ci` built at
`../iris`, `rb-cli`, and an installed 5.3 image with the dev tools.

The boot disk is never written: `ci/*.toml` set `overlay = true`, so guest
writes land in `<image>.chd.diff.chd`. `--fresh` deletes that and starts from
pristine — always use it when a previous run left a broken kernel behind.

`--release 6.5` builds the root 6.5 driver the same way, which is how you check
that a shared-region change did not regress it.

Use this for every iteration. Only go to real hardware for what it cannot do:
packet flow.

### 3.1 You cannot build this on the Mac directly

There is no practical cross-toolchain. IRIX 5.3 kernel objects need o32 COFF
from SGI's own `cc` with board-specific flags; GCC's `mips-sgi-irix5` target
cannot produce a usable `-coff` kernel object, and a mismatched kernel object
does not fail to link, it corrupts the kernel. Build natively — in the
emulator (§3.0) or on the machine itself.

### 3.2 On a real IRIX 5.3 machine

Prerequisites — verify all four before starting:

```sh
uname -aR                        # confirm IRIX 5.3, and the IP number
which cc                         # IRIX Development Option must be installed
ls /var/sysgen/Makefile.kernio   # kernel build environment
ls -d /usr/cpu/sysgen/IP*boot    # kernel object directory for your CPU
```

If `cc` is missing you need the IRIX Development Option (`dev.sw.*`) from the
5.3 media. `gcc` is not a substitute for kernel objects.

Get the tree onto the machine — NFS mount, or FTP, or `tar cf - irix5.3 | rsh
indy 'cd /usr/local/src && tar xf -'`. Then:

```sh
cd irix5.3
smake                     # Indy / Indigo2 / Challenge S (IP22, R4x00)
smake MIPSOPT=-mips1      # Indigo R3000 (IP12), Personal Iris
```

For the **first** build on any machine, turn on everything:

```sh
smake MYCFLAGS="-coff -mips2 -D_MP_NETLOCKS -DMP -I. -DDP_LOG -DDP_LOG_SCSI -DDP_CHECK_ETHERIF"
```

Then install and rebuild the kernel:

```sh
smake install
# add "INCLUDE: dp" to /var/sysgen/system/irix.sm
/etc/autoconfig -f
reboot
```

Note `INCLUDE:`, **not** `USE:` — `USE:` is a 6.x spelling, and 5.3's lboot
calls `dp_init()` for `INCLUDE`'d drivers. Getting this wrong means the driver
is compiled into the kernel but never initialised, which looks exactly like
"the device wasn't found".

### 3.4 Under emulation (manual)

MAME emulates the Indy (IP22) well enough to run IRIX 5.3, and it is a
legitimate way to shake out compile and boot failures without risking real
hardware or waiting on reboots. It is slow but the debug loop is no worse than
the real machine's, and snapshots let you recover from an unbootable kernel
instantly — which matters a lot here (§4.2). QEMU does not do SGI.

### 3.5 Keep a known-good kernel

Before the first `autoconfig`, save a bootable fallback:

```sh
cp /unix /unix.works
```

If the new kernel panics, interrupt PROM boot and `boot /unix.works`. Do this
first. A driver that panics in `dp_init()` will panic on every boot, and
without a fallback the machine needs external media to recover.

---

## 4. Debug workflow and its constraints

### 4.1 No loadable modules

IRIX 5.3 has no `ml(1M)`, no `mload.h`, no `M_VERSION`. Zero mentions of
loadable modules exist in the entire 5.3 driver guide. Every code change is
`smake install` + `autoconfig -f` + reboot. On real hardware that is minutes
per iteration.

Practical consequence: **front-load the logging.** Prefer one boot that prints
everything to five boots that each print one thing. `DP_LOG`, `DP_LOG_SCSI`
and `DP_LOG_NET` all exist; `DP_LOG_NET` is very noisy once traffic flows but
is the right first choice when frames are arriving garbled.

### 4.2 Recovering an unbootable kernel

`boot /unix.works` from the PROM monitor (see §3.4). If you did not save one,
`boot -f dksc(0,1,8)unix.save` or the install tools CD are the fallbacks.

### 4.3 Kernel debugger

5.3 supports a debuggable kernel; see ch.10 of the driver guide. Worth the
setup if you hit a panic you cannot read from the console message alone.

### 4.4 Reading what lboot generated

`/var/sysgen/master.c` is written by lboot and shows the generated switch
tables. If the driver silently fails to appear, check that `dp_` entries are
present there — a rejected master file is otherwise quiet.

---

## 5. The unknowns, ranked by how badly they bite

Each corresponds to an `XXX53` marker in the source.

### 5.1 `struct etherif` layout — HIGHEST RISK

`sgi_ether.h` here is a reconstruction derived from the 6.5 copy (revision
1.15) with the `INET6` member removed. If the real 5.3 member list differs,
`ether_attach()` writes past `dp_eif` into the softc. **Silently** — no link
error, no compile warning, no immediate crash. Symptoms appear later as
inexplicable corruption of `dp_txq`, `dp_rxbuf` pointers, or lock state.

Mitigating factor: most of the struct's size comes from `eif_arpcom`,
`eif_mfilter` and `eif_rawif`, which are declared in 5.3's own *shipped*
`net/if.h`, `net/multi.h` and `net/raw.h`. Those self-correct when compiled on
the target. The residual risk is confined to `etherif`'s own members —
principally whether `eif_resets`, `eif_lostintrs` and `eif_sick` exist and are
in this order.

**Build the first kernel with `-DDP_CHECK_ETHERIF.**` That places an eight-word
`0x5A5AD9D9` canary immediately after `dp_eif` and checks it survives
`ether_attach()`. If it trips you get an explicit console message naming how
many words were overrun, instead of a mystery three days later. Cost is 32
bytes per interface. It is a diagnostic, not a safety net — if it fires, the
softc is *already* corrupt, so do not bring the interface up.

First thing to check on the target:

```sh
nm /unix | grep -E 'ether_attach|ether_input|ether_stop'
```

If `ether_attach` is absent from the kernel symbol table, §2.2 has to be
revisited and the port becomes substantially larger. That is the one finding
that would justify rethinking the whole approach.

### 5.2 `cpsema()` return sense — RESOLVED

`mutex_trylock` maps to `cpsema`, which must return non-zero **only** when the
lock was taken. `sys/sema.h` settles it: on a uniprocessor build it aliases

    #define apcpsema(x)  1

i.e. "conditional acquire always succeeds", which only type-checks as a success
indicator if non-zero means acquired. The mapping is correct.

`-DDP_NO_TRYLOCK` remains available if the TX ring is ever suspected: it
disables the transmit-side queue kick, a latency optimisation only — the 10 ms
poll timer still drains the ring.

### 5.3 Everything else — RESOLVED

All settled by pulling headers and `/unix` straight out of the 5.3 disk image
with `rb-cli` (no booting needed — `rb-cli get "$IMG@1" /usr/include/sys/x.h`),
then confirmed by an actual compile and kernel link:

| Item | Finding |
|---|---|
| `kmem_zalloc` flags | `KM_SLEEP` is 0 (`sys/kmem.h`); `VM_DIRECT` 0x0100 and `VM_CACHEALIGN` 0x0800 (`sys/immu.h`). All native. |
| `plbase`, `itimeout`, `untimeout`, `toid_t` | All in `sys/ddi.h`. `toid_t itimeout(void (*)(), void *, long, pl_t, ...)`. |
| `m_vget` | `extern struct mbuf *m_vget(int, int, int)` in `sys/mbuf.h`. |
| `scsi_driver_table` sentinel | `SCSIDRIVER_NULL` is 0; the code now uses the constant. |
| `D_MP` | In `sys/conf.h` — 5.3 needs that include explicitly; 6.5 gets it via another path. |
| `SN_MORETOCOME` | **Does not exist on 5.3.** Shimmed to 0; see §2.3. |
| master.d flags `cs` | **Accepted by lboot** — the generated `master.c` has `dp_init` in the init table and `dp_open`/`dp_close`/`dp_ioctl` in cdevsw. |
| `CPUBOARD` | **Mandatory.** `Makefile.kernio` defines `CFLAGS` only inside `#if defined(CPUBOARD)`; unset gives an EMPTY `CFLAGS` — no `-D_KERNEL`, no `-coff` — which compiles and produces an object that would wreck the kernel. |
| `-D_MP_NETLOCKS -DMP` | **Do not set.** `Makefile.kernio` passes them for no board, so a stock kernel is not built that way, and setting them changes `struct ifnet`'s size — hence `struct etherif`'s — which is precisely §5.1's failure mode. |

Every kernel symbol the driver references was confirmed present in the 5.3
`/unix`, and then proven by a successful `autoconfig -f` link.

## 6. Failure-mode triage

| Symptom | Most likely cause | First move |
|---|---|---|
| Undefined symbol at `autoconfig`/lboot time | One of §5.3 — the named symbol does not exist in 5.3 | Fix that one item; the linker told you exactly which |
| lboot drops the driver, no error | master file flags, or missing `s` (software) flag — lboot cannot probe SCSI and concludes absent | `grep dp_ /var/sysgen/master.c` |
| Kernel builds, panics during boot | `dp_init()` running before the SCSI subsystem is ready, or `scsi_driver_table[]` indexed out of range | `boot /unix.works`; narrow the `SCSI_SGISTART` range |
| Boots fine, no `dp0`, no messages | `USE:` used instead of `INCLUDE:` — `dp_init()` never called | Fix `irix.sm`, re-`autoconfig` |
| `autoconfig` succeeded but the booted kernel has no driver | You booted `/unix`, the OLD kernel. `autoconfig` only **stages** the new one as `/unix.install`; the rename happens during a clean shutdown | Reboot cleanly (`shutdown`/`init 6`), or boot `/unix.install` explicitly from the PROM. This bit the emulator boot test — see `scripts/iris-build.sh` |
| `multiply defined _irix5_mips4` from `ng1.a` during autoconfig | Pre-existing on stock 5.3 images, unrelated to this driver | Ignore; `autoconfig` still succeeds |
| Boots, `dp_init` runs, no device found | INQUIRY match failing, or adapter range wrong, or `drvnum == 0` sentinel wrong | `-DDP_LOG` prints every type-3 device's vendor/product; compare against `"Dayna"` / `"SCSI/Link"` |
| `dp0` exists, `ifconfig up` hangs forever | `psema(&sc->dp_sema)` never woken — `sr_notify` not called | `-DDP_LOG_SCSI`; check `scsi_command[]` index and that `sr_notify` is non-NULL (5.3 rejects NULL) |
| `ifconfig up` OK, zero traffic | RX poll not running — `itimeout`/`plbase` wrong, or `dp_enabled` never set | `-DDP_LOG_NET`; confirm `dp_runqueue` re-arms |
| Frames arrive but are garbled, SCSI status good | **Cache coherency** — the WD33C93 DMA path on IP22 differs a lot from the Octane's | See §7 |
| Random panics under load | §5.2 `cpsema` sense → TX ring corruption | Rebuild with `-DDP_NO_TRYLOCK` |
| Corrupt softc fields, nonsense pointers | §5.1 struct etherif overrun | Rebuild with `-DDP_CHECK_ETHERIF` |
| Works, but throughput is poor | Expected — see §7 | — |

---

## 7. Known risks once it boots

**Cache coherency is the most likely source of data corruption.** The driver
sets `SRF_FLUSH` and allocates buffers `VM_DIRECT|VM_CACHEALIGN`, which is the
right shape: ch.5 of the driver guide states that internally generated requests
transferring into kernel memory must handle their own cache flushing. But that
was tuned on an Octane. The Indy/Indigo2 WD33C93 path is different, and this is
where to look first if frames arrive garbled while SCSI status reports good.

**Throughput will be well below the 600 KB/s** in the parent README — that
figure is from an Octane2. An R4000 Indy doing synchronous SCSI on a 10 ms poll
is a different machine entirely. Do not treat a lower number as a bug.

**Boot-time probing is chatty.** `dp_init()` issues an INQUIRY to every target
on every integral adapter. On a bus with cranky devices this can be slow or
noisy. Only LUN 0 is scanned, which is fine for every known DaynaPort and
emulator.

**No detach path.** The 5.3 driver is permanent once linked in. There is no
`dp_detach`/`dp_unload` because there is nothing that could call them.

---

## 8. If it works

1. Report the real numbers — machine, IRIX release, emulator, throughput.
2. Update §1 of this file and drop the "never been compiled" warning from
   `irix5.3/README.md` and the root `README.md`.
3. Then consider the follow-up refactor: hoist the 501-line shared region into
   a common `dp_proto.c` included by both drivers and delete `drift.sh`. This
   was deliberately deferred — landing an unverified port *and* a refactor of
   working code in one PR is how PRs stall on "I can't test this."

---

## 9. References

SGI, *IRIX 5.3 Device Driver Programming Guide* (007-0911-050), at
`techpubs.jurassic.nl/manuals/0530/developer/DevDriver_PG/sgi_html/`. Note the
host 403s on some clients; fetching with a browser User-Agent works.

- ch. 2 — config files, `INCLUDE:` vs `VECTOR:`, `drvinit()`, `-coff`,
  `/var/sysgen` layout
- ch. 5 — `scsi_info`/`scsi_alloc`/`scsi_free`/`scsi_command`, `scsi_request`
  fields, `SRF_*`, `SC_*`/`ST_*`, and the worked `sdk` example
- ch. 8 — `initnsema`/`psema`/`vsema`/`cpsema`/`freesema`, `LOCK`/`TRYLOCK`
- ch. 9 — ifnet conventions, `-D_MP_NETLOCKS -DMP`, the `sk` skeleton driver
  (see `sk_output` for what §2.2 avoided)
- ch. 10 — debuggable kernels

Protocol: SLINKCMD.TXT by Roger Burrows (rev 1.20).
