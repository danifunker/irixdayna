# RESUME — IRIX 5.3 port, troubleshooting handoff

Pick-up notes for continuing work on the IRIX 5.3 port. Written 2026-08-12 on
branch `irix-53`. If you are an AI assistant resuming this work, read this file
in full before touching anything — it records decisions and their reasons, and
several obvious-looking "improvements" are wrong for reasons captured here.

---

## 1. State of the work

**The 5.3 port has never been compiled and never been run.** It has not been
near an IRIX machine. Everything in `irix5.3/` is written against SGI's *IRIX
5.3 Device Driver Programming Guide* (007-0911-050), not against real headers.

What is genuinely verified:

- The drift checker works in both directions (tested by perturbing `dp_do_rx`
  and confirming it produces a diff, then restoring).
- 501 lines are byte-identical between `irix5.3/if_dp.c` and `../if_dp.c`.
- The 6.5 driver is untouched — `git diff main -- if_dp.c Makefile master.d/`
  is empty.

What is **not** verified: that any of it compiles, links, boots, or moves a
packet. Assume nothing.

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

The trade: `sgi_ether.h` becomes a reconstruction, which is now the port's
single largest risk. See §5.1. If `ether_attach` turns out not to exist in the
5.3 kernel, this decision has to be revisited and the port gets much bigger.

### 2.3 The 6.5 API shim

`irix5.3/if_dp.c` opens with ~30 lines mapping 6.5 spellings onto 5.3
primitives rather than scattering `#ifdef`s through the protocol code:

| 6.5 | 5.3 |
|---|---|
| `mutex_lock(m, pri)` | `psema(m, pri)` |
| `mutex_unlock(m)` | `vsema(m)` |
| `mutex_trylock(m)` | `cpsema(m)` |
| `mutex_init(m, type, name)` | `initnsema(m, 1, name)` |
| `mutex_destroy(m)` | `freesema(m)` |
| `init_sema(s, v, name, unit)` | `initnsema(s, v, name)` |

The signatures line up almost exactly, which is *why* 501 lines can stay
byte-identical — including all five `etherif` handlers, which a naive port
would have had to fork.

Two subtleties that were fixed and should not be undone:

- There is **no `typedef sema_t mutex_t`**. 5.3 may define its own `mutex_t`
  and a typedef would collide. The softc declares `sema_t` directly.
- Each macro is preceded by `#undef`. If 5.3's `sys/sema.h` provides any of
  these names as a macro or prototype, ours must win without a redefinition
  diagnostic.

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

### 3.1 You cannot build this on the Mac

There is no practical cross-toolchain. IRIX 5.3 kernel objects need o32 COFF
from SGI's own `cc`; GCC's `mips-sgi-irix5` target cannot produce a usable
`-coff` kernel object, and the kernel headers are not redistributable. Build
natively.

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

### 3.3 Under emulation

MAME emulates the Indy (IP22) well enough to run IRIX 5.3, and it is a
legitimate way to shake out compile and boot failures without risking real
hardware or waiting on reboots. It is slow but the debug loop is no worse than
the real machine's, and snapshots let you recover from an unbootable kernel
instantly — which matters a lot here (§4.2). QEMU does not do SGI.

### 3.4 Keep a known-good kernel

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

### 5.2 `cpsema()` return sense

`mutex_trylock` maps to `cpsema`, used in exactly one place —
`dp_eio_transmit`'s opportunistic queue kick. It must return non-zero **only**
when the lock was actually taken.

The 5.3 manual describes the failure case as "semaphore count is already less
than 0". For a mutex initialised to 1, the would-block case is count `<= 0`,
so the manual's wording is either imprecise or describes different semantics
than assumed. If `cpsema` has the other sense, `dp_eio_transmit` re-enters
`dp_runqueue` concurrently and corrupts the TX ring — expect random panics
under load rather than a clean failure.

Check `sys/sema.h`. **If in any doubt, build with `-DDP_NO_TRYLOCK`.** That
disables the kick entirely; it is a latency optimisation only, and the 10 ms
poll timer still drains the ring. Slightly slower, zero correctness risk. This
is the right default until someone has read the header.

### 5.3 Everything else

| Item | Where | If wrong |
|---|---|---|
| `kmem_zalloc` flags | shim + `dp_do_attach` | `KM_SLEEP` is a 6.x spelling; 5.3 may want `0` for sleep and `VM_NOSLEEP` for the opposite. Likely a compile error — cheap to find. |
| `plbase` | `dp_runqueue`, 4th arg to `itimeout` | Compile error. `#define plbase 0` is the substitute. |
| `toid_t`, `itimeout`, `untimeout` | softc, `dp_runqueue` | Compile error. `int` substitutes for `toid_t`. |
| `m_vget` | `dp_do_rx` | Compile/link error. It is an SGI extension; if absent, `m_get` + cluster attach is the fallback and `dp_do_rx` leaves the shared region. |
| `scsi_driver_table` sentinel | `dp_probe_one` | Assumed 0 means "no adapter". If a real `SCSIDRIVER_*` constant is 0 on your platform, valid adapters get skipped — symptom is "no DaynaPort found" with a device that is definitely present. |
| master.d flags `cs` | `master.d/dp` | lboot rejects the file, or drops the driver. Check `master(4)` and `/var/sysgen/master.c`. |
| `sprintf` in kernel | MAC/log formatting | Link error. |
| `//` comments | — | The 5.3 compiler is pre-C99. The file currently uses only `/* */`. Keep it that way; `grep '//' irix5.3/if_dp.c` should find nothing outside block comments. |

A compile or link error here is the **good** outcome: it names the wrong
assumption precisely. The dangerous items are §5.1 and §5.2, which fail
silently.

---

## 6. Failure-mode triage

| Symptom | Most likely cause | First move |
|---|---|---|
| Undefined symbol at `autoconfig`/lboot time | One of §5.3 — the named symbol does not exist in 5.3 | Fix that one item; the linker told you exactly which |
| lboot drops the driver, no error | master file flags, or missing `s` (software) flag — lboot cannot probe SCSI and concludes absent | `grep dp_ /var/sysgen/master.c` |
| Kernel builds, panics during boot | `dp_init()` running before the SCSI subsystem is ready, or `scsi_driver_table[]` indexed out of range | `boot /unix.works`; narrow the `SCSI_SGISTART` range |
| Boots fine, no `dp0`, no messages | `USE:` used instead of `INCLUDE:` — `dp_init()` never called | Fix `irix.sm`, re-`autoconfig` |
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
