# irixdayna — IRIX 5.3 port

IRIX 5.3 build of the DaynaPort SCSI/Link Ethernet driver. The 6.5 driver
lives in the parent directory and is unchanged by this port.

> **Status: compiles clean on IRIX 5.3/IP22; not yet run on hardware.** The
> driver is built and verified by `scripts/iris-build.sh`, which compiles it
> natively inside an emulated Indy. Every API question from the first draft has
> been settled against real 5.3 headers — see
> [Before first boot](#before-first-boot) — except the `struct etherif` layout,
> which cannot be resolved without running code. Nothing has moved a packet
> yet: the emulator has no DaynaPort target.

For picking this up cold — design rationale, failure-mode triage, recovery from
an unbootable kernel — see [RESUME.md](RESUME.md).

## Supported machines

IRIX 5.3 runs on:

- **IP22** — Indy, Indigo2, Challenge S (R4x00) — the expected target
- **IP20** — Indigo R4000
- **IP12** — Indigo R3000, Personal Iris
- **IP19 / IP21** — Challenge / Onyx

The board is selected by `CPUBOARD`, which picks the CFLAGS set out of
`/var/sysgen/Makefile.kernio`. It is **mandatory** — see
[§4](#4-cpuboard-is-mandatory).

For Octane (IP30), O2 (IP32), Fuel (IP35) or anything else running 6.5, use the
driver in the parent directory. IP26/IP28 need 6.2 or later, so they also use
the 6.5 build.

## Build and install

```sh
smake                  # Indy / Indigo2 / Challenge S (CPUBOARD=IP22 default)
smake CPUBOARD=IP20    # Indigo R4000
smake CPUBOARD=IP12    # Indigo R3000
smake install
```

Or build it on the host, inside the emulator, without touching a real machine:

```sh
scripts/iris-build.sh --release 5.3               # compile only
scripts/iris-build.sh --release 5.3 --autoconfig  # + link a real kernel
```

Then add to `/var/sysgen/system/irix.sm`:

```
INCLUDE: dp
```

and rebuild the kernel:

```sh
/etc/autoconfig -f && reboot
```

`dp0` appears on the next boot. Configure it the same way as on 6.5:

```sh
ifconfig dp0 <ip> netmask <mask> up
```

Two things differ from the 6.5 instructions and will bite if you skip them:

- **`INCLUDE:`, not `USE:`.** `USE:` is a 6.x spelling. 5.3's lboot calls
  `dp_init()` for `INCLUDE`'d drivers.
- **There is no `smake load`.** IRIX 5.3 has no loadable kernel modules — no
  `ml(1M)`, no `mload.h`, no `M_VERSION`. Every code change means
  `autoconfig` and a reboot. Budget for it; it makes the debug loop slow.

If the kernel does not build, check `/var/sysgen/master.c` — lboot writes the
generated switch tables there, and a missing `dp_` entry means the master file
was rejected.

## What differs from the 6.5 driver

| | IRIX 6.5 | IRIX 5.3 |
|---|---|---|
| Device addressing | hwgraph `vertex_hdl_t`, `scsi_lun_info_t` | `(adapter, target, lun)` integers |
| SCSI submission | `SLI_COMMAND(lun_info)(req)` | `(*scsi_command[drvnum])(req)` |
| Discovery | `scsi_driver_register(3)` + CDL callback, or hwgraph inventory walk | `dp_init()` scans the bus with `scsi_info[]()` |
| `scsi_alloc` success | `== SCSIALLOCOK` | **`!= 0`** (returns adapter type; 0 means failure) |
| Mutexes | `mutex_t`, `mutex_lock/trylock/unlock` | `sema_t` + `psema`/`cpsema`/`vsema` |
| Semaphore init | `init_sema(s, v, name, unit)` | `initnsema(s, v, name)` / `initnsema_mutex(m, name)` |
| Inventory | `device_inventory_add(vhdl, ...)` | `add_to_inventory(class, type, ctlr, unit, state)` |
| Loading | `ml` loadable or built-in | built-in only |
| ABI | n32 / 64-bit, mips3 / mips4 | **o32 only**, `-coff -non_shared -Wc,-pic0 -Wx,-G8` from `Makefile.kernio` |
| Detach / unload | `dp_detach`, `dp_unload` | none — driver is permanent |
| RX batching hint | `SN_MORETOCOME` | does not exist; shimmed to 0 (advisory only) |

The DaynaPort protocol itself is unchanged: same five vendor CDBs, same
ZuluSCSI multi-packet READ handling, same 10 ms poll, same TX ring.

### The 6.5 API shim

Rather than scatter `#ifdef`s through the protocol code, `if_dp.c` opens with a
short shim mapping the 6.5 spellings onto 5.3 primitives — `mutex_lock` →
`psema`, `mutex_trylock` → `cpsema`, `mutex_init` → `initnsema_mutex`,
`init_sema` → `initnsema`, `SN_MORETOCOME` → 0. The signatures line up almost
exactly, which is what lets the shared region stay byte-identical.

One shim entry is load-bearing rather than cosmetic: 5.3's `sys/sema.h` **does**
define `mutex_init`, with a different four-argument shape
(`mutex_init(m, nm, f, i)`, name second). Without the `#undef` the 6.5-style
three-argument call expands wrongly. `mutex_lock`/`mutex_unlock`/`mutex_trylock`
are not defined by 5.3 at all — it uses `mutex_enter`/`mutex_exit`.

### Why the protocol code is duplicated

`if_dp.c` here is a near-copy of `../if_dp.c`. That is deliberate. Only about a
third of the 6.5 driver is genuinely shareable — the rest is discovery, attach,
locking and module plumbing that has no counterpart across the two releases.
A compat layer abstracting ~600 divergent lines to save ~500 shared ones would
have been more `#ifdef` than code.

The duplication is kept honest mechanically:

```sh
smake drift      # or: sh drift.sh
```

This extracts the region between the `BEGIN SHARED` / `END SHARED` markers and
diffs it against the corresponding region of `../if_dp.c`. It is currently
**501 lines, byte-identical**, and covers `dp_do_rx`, `dp_do_tx`,
`dp_runqueue`, the CDB builders and all five `etherif` handlers.

**A protocol fix belongs in both files.** `dp_do_rx` in particular is where
emulator-specific bugs will show up as BlueSCSI / PiSCSI / SCSI2SD get tested,
and a fix landing in only one tree is exactly what `drift` exists to catch.

Once this port is confirmed working on hardware, the natural follow-up is to
hoist that shared region into a common `dp_proto.c` included by both and delete
`drift.sh`.

## Before first boot

Almost all of this is now settled. The remaining risk is concentrated in §1.

### 1. `struct etherif` layout — highest risk

The driver embeds `struct etherif` by value as the first member of
`struct dp_softc`, and `ether_attach()` writes through it. `ether.h` is a
private kernel header that IRIX does not ship, so `sgi_ether.h` here is a
reconstruction derived from the 6.5 copy. **If 5.3's real member list differs,
`ether_attach()` corrupts adjacent softc memory, silently** — no link error, no
warning.

Most of the struct is safe automatically: `eif_arpcom`, `eif_mfilter` and
`eif_rawif` come from 5.3's own shipped `net/if.h`, `net/multi.h` and
`net/raw.h`, so they pick up correct 5.3 layouts when compiled on the target.
The residual risk is `etherif`'s own members — principally whether
`eif_resets`, `eif_lostintrs` and `eif_sick` exist, and in this order.

**Build your first kernel with `-DDP_CHECK_ETHERIF.**` That places an
eight-word canary immediately after `dp_eif` and verifies it survives
`ether_attach()`, so a layout mismatch announces itself on the console instead
of surfacing as mystery corruption days later. Cost is 32 bytes per interface.
It is a diagnostic, not a safety net — if it fires the softc is already
corrupt, so do not bring the interface up.

**`ether_attach` itself is confirmed present**, along with `ether_input`,
`ether_stop` and `ether_selfsnoop`, in the `/unix` of a stock 5.3 IP22 install:

```sh
nm /unix | grep -E 'ether_attach|ether_input|ether_stop'
```

That was the finding that validated this whole approach — had it been absent,
the port would have needed a hand-written `if_output` with `ip_arpresolve`
across `AF_INET` / `AF_UNSPEC` / `AF_RAW` / `AF_SDL`, roughly 250 lines that
`ether_attach` provides for free. (`ether_detach` and `ether_reattach` are
absent, as expected — they exist only for 6.x loadable drivers, and this driver
has no unload path.)

What the symbol table cannot tell us is the *layout*, since the kernel carries
no struct debug info. Hence the canary.

### 2. `cpsema()` return sense — resolved

`mutex_trylock` maps to `cpsema`, and it must return non-zero **only** when the
lock was actually taken. `sys/sema.h` settles it: on a uniprocessor build it
aliases `apcpsema(x)` to `1` — "conditional acquire always succeeds" — which
only type-checks as a success indicator if non-zero means acquired. The mapping
is correct.

`-DDP_NO_TRYLOCK` remains available if the TX ring is ever suspected. It
disables the transmit-side queue kick, which is a latency optimisation only —
the 10 ms poll timer still drains the ring.

### 3. Everything else — resolved

These were open in the first draft. All were settled by extracting headers and
the `/unix` symbol table straight out of an IRIX 5.3 IP22 disk image with
`rb-cli` — no booting required:

| Item | Finding |
|---|---|
| `kmem_zalloc` flags | `KM_SLEEP` is 0 in `sys/kmem.h`; `sys/immu.h` gives `VM_DIRECT` 0x0100, `VM_CACHEALIGN` 0x0800. All native. |
| `plbase`, `itimeout`, `untimeout`, `toid_t` | All in `sys/ddi.h`: `extern pl_t plbase`, `toid_t itimeout(void (*)(), void *, long, pl_t, ...)`. Correct as written. |
| `m_vget` | `extern struct mbuf *m_vget(int, int, int)` in `sys/mbuf.h`. |
| `scsi_driver_table` sentinel | `SCSIDRIVER_NULL` is 0 — the check was right, and now uses the constant. |
| `mutex_init` collision | 5.3 **does** define it, as `mutex_init(m, nm, f, i)` — four args, name second. The shim's `#undef` is load-bearing, not defensive. |
| `mutex_t` | 5.3 spells it `kmutex_t`. Nothing to collide with; the softc uses `sema_t`. |
| `//` comments | The 5.3 compiler is pre-C99. This file uses only `/* */`; keep it that way. |

Every kernel symbol the driver references was confirmed present in the 5.3
`/unix`: `ether_attach`, `ether_input`, `add_to_inventory`, `m_vget`,
`m_freem`, `psema`, `vsema`, `cpsema`, `initnsema`, `initnsema_mutex`,
`freesema`, `itimeout`, `untimeout`, `kmem_zalloc`, `kmem_free`, `sprintf`,
`cmn_err`, `delay`, `scsi_driver_table`, `scsi_info`, `scsi_alloc`,
`scsi_free`, `scsi_command`.

Still genuinely open: the `struct etherif` layout (§1), whether lboot accepts
the master.d flags, and of course whether it moves a packet.

### 4. `CPUBOARD` is mandatory

5.3's `/var/sysgen/Makefile.kernio` wraps its entire body in
`#if defined(CPUBOARD) && !empty(CPUBOARD)` and defines `CFLAGS` only inside
it. **With `CPUBOARD` unset you get an empty `CFLAGS`** — no `-D_KERNEL`, no
`-coff`, no R4000 errata workarounds — which compiles happily and produces an
object that would wreck the kernel. The Makefile defaults it to `IP22`; check
`hinv` and override if you are on something else.

The driver also deliberately does **not** pass `-D_MP_NETLOCKS -DMP`. The 5.3
driver guide mentions them for multi-threaded TCP/IP, but `Makefile.kernio`
passes them for no board at all, so a stock kernel is not built with them.
Setting them when the kernel was not changes the size of `struct ifnet` — and
therefore of the `struct etherif` embedded in the softc — which is exactly the
silent-corruption failure of §1.
## Known risks once it does boot

**Cache coherency.** The driver sets `SRF_FLUSH` and allocates its buffers
`VM_DIRECT|VM_CACHEALIGN`, which is the right shape — the 5.3 manual notes that
internally generated requests into kernel memory must handle their own cache
flushing. But the Indy/Indigo2 WD33C93 DMA path is very different from the
Octane's, and this is the most likely source of data corruption on first boot.
If frames arrive garbled but the SCSI status is good, start here.

**Throughput.** The 600 KB/s figure in the parent README came from an Octane2.
An R4000 Indy doing synchronous SCSI on a 10 ms poll will be well short of that.

**Boot-time probing.** `dp_init()` issues an INQUIRY to every target on every
integral adapter. On a bus with cranky devices this can be slow or noisy at
boot. Only LUN 0 is scanned.

## Reference

SGI, *IRIX 5.3 Device Driver Programming Guide* (007-0911-050):

- ch. 2 — configuration files, `INCLUDE:` vs `VECTOR:`, `drvinit()`, `-coff`
- ch. 5 — `scsi_info` / `scsi_alloc` / `scsi_free` / `scsi_command`,
  `scsi_request`, `SRF_*` and `SC_*` / `ST_*` status
- ch. 8 — `initnsema` / `psema` / `vsema` / `cpsema` / `freesema`
- ch. 9 — ifnet conventions, `-D_MP_NETLOCKS -DMP`

Protocol: SLINKCMD.TXT by Roger Burrows (rev 1.20).
