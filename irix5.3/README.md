# irixdayna — IRIX 5.3 port

IRIX 5.3 build of the DaynaPort SCSI/Link Ethernet driver. The 6.5 driver
lives in the parent directory and is unchanged by this port.

> **Status: not yet run on hardware.** Every IRIX 5.3 interface used here was
> taken from SGI's *IRIX 5.3 Device Driver Programming Guide*, but a handful of
> calls could not be confirmed without a real 5.3 `/usr/include`. They are
> marked `XXX53` in `if_dp.c` and listed under
> [Before first boot](#before-first-boot). Work through that list first.

For picking this up cold — design rationale, failure-mode triage, recovery from
an unbootable kernel — see [RESUME.md](RESUME.md).

## Supported machines

IRIX 5.3 runs on:

- **IP22** — Indy, Indigo2, Challenge S (R4x00) — the expected target, `-mips2`
- **IP20** — Indigo R4000
- **IP12** — Indigo R3000, Personal Iris — `-mips1`
- **IP19 / IP21** — Challenge / Onyx

For Octane (IP30), O2 (IP32), Fuel (IP35) or anything else running 6.5, use the
driver in the parent directory. IP26/IP28 need 6.2 or later, so they also use
the 6.5 build.

## Build and install

```sh
smake                 # Indy / Indigo2 (default, -mips2)
smake MIPSOPT=-mips1  # Indigo R3000 / Personal Iris
smake install
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
| Semaphore init | `init_sema(s, v, name, unit)` | `initnsema(s, v, name)` |
| Inventory | `device_inventory_add(vhdl, ...)` | `add_to_inventory(class, type, ctlr, unit, state)` |
| Loading | `ml` loadable or built-in | built-in only |
| ABI | n32 / 64-bit, mips3 / mips4 | **o32 only**, `-coff`, mips1 / mips2 |
| Detach / unload | `dp_detach`, `dp_unload` | none — driver is permanent |

The DaynaPort protocol itself is unchanged: same five vendor CDBs, same
ZuluSCSI multi-packet READ handling, same 10 ms poll, same TX ring.

### The 6.5 API shim

Rather than scatter `#ifdef`s through the protocol code, `if_dp.c` opens with a
~25-line shim that maps the 6.5 spellings onto 5.3 primitives —
`mutex_lock` → `psema`, `mutex_trylock` → `cpsema`, `init_sema` → `initnsema`,
and so on. The signatures line up almost exactly, which is what lets the shared
region stay byte-identical.

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

These could not be confirmed without a 5.3 system. Each is marked `XXX53` in
the source.

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

Confirm the entry points exist at all:

```sh
nm /unix | grep -E 'ether_attach|ether_input|ether_stop'
```

If `ether_attach` is absent from the 5.3 kernel, this port needs rethinking:
SGI's 5.3 documentation only ever describes the raw `ifnet` interface, and
falling back to it means hand-writing `if_output` with `ip_arpresolve` across
`AF_INET` / `AF_UNSPEC` / `AF_RAW` / `AF_SDL` — roughly 250 lines that
`ether_attach` currently provides for free.

### 2. `cpsema()` return sense

`mutex_trylock` maps to `cpsema`. It must return non-zero **only** when the lock
was actually taken. The 5.3 manual describes the failure case as "semaphore
count is already less than 0", but for a mutex initialised to 1 the would-block
case is count `<= 0`. If `cpsema` has the other sense, `dp_eio_transmit()` can
re-enter `dp_runqueue()` concurrently and corrupt the TX ring.

Check `sys/sema.h`. If unsure, build with `-DDP_NO_TRYLOCK`: that disables the
transmit-side queue kick, which is only an optimisation — the 10 ms poll timer
still drains the ring. Slightly higher latency, no correctness risk.

### 3. Everything else

| Item | Check |
|---|---|
| `kmem_zalloc` flags | Does 5.3 accept `KM_SLEEP`? It may want `0` for sleep and `VM_NOSLEEP` for the opposite. `VM_DIRECT`/`VM_CACHEALIGN` are needed so the adapter can DMA straight into the buffer. |
| `plbase` | Fourth argument to `itimeout()`. If absent, `#define plbase 0`. |
| `toid_t`, `itimeout`, `untimeout` | Confirm in `sys/ddi.h`. If `toid_t` does not exist, `int` is the substitute. |
| `m_vget` | RX allocates with `m_vget(M_DONTWAIT, len, MT_DATA)`. Confirm in `sys/mbuf.h`. |
| `scsi_driver_table` sentinel | `dp_probe_one()` treats 0 as "no adapter". Confirm no real `SCSIDRIVER_*` constant is 0 on your platform. |
| master.d flags | `cs` follows the 5.3 manual. If lboot rejects it, check `master(4)`. |
| `sprintf` in kernel | Used for the MAC and log strings. |
| `//` comments | The 5.3 compiler is pre-C99. This file uses only `/* */`; keep it that way. |

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
