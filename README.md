# irixdayna — DaynaPort SCSI/Link Ethernet Driver for IRIX

## What is it

A native IRIX kernel driver for the DaynaPort SCSI/Link Ethernet adapter
(DP0801/DP0802) and compatible emulators including ZuluSCSI, BlueSCSI V2,
PiSCSI.

Two drivers share one protocol implementation: **IRIX 6.5** under `irix6.5/`
(tested on IP30/Octane), and an **IRIX 5.3** port under `irix5.3/`
(tested on IP20/Indigo R4000). The DaynaPort protocol code — the RX/TX path,
`dp_runqueue`, the CDB builders and the etherif handlers — lives once in
[`shared/dp_proto.c`](shared/dp_proto.c) and is `#include`d by both drivers,
so a protocol fix lands in both at once. Each driver keeps only its own
discovery, SCSI submission and locking.

Repository layout:

- `irix6.5/` — the 6.5 driver: `if_dp.c`, `Makefile`, `master.d/dp`,
  `sgi_ether.h`, plus its emulator configs under `ci/`
- `irix5.3/` — the 5.3 port: same shape, plus `installer/` (the CD install
  scripts) and `docs/` (5.3 work notes)
- `shared/` — everything both use: `scripts/` (emulator build, acceptance
  ladder, CD mastering), `ci/local.conf.example`, and protocol/architecture
  docs under `docs/`
- `dist/` — build output (gitignored), written by the scripts

The DaynaPort SCSI/Link is a SCSI-attached Ethernet adapter that was originally
sold for vintage Macs. It presents as a SCSI type-3 (Processor) device and
moves Ethernet frames using vendor-specific CDBs. This driver bridges the IRIX
SCSI subsystem and the BSD-derived Ethernet stack, giving any IRIX machine with
a SCSI bus a WiFi or Ethernet connection via a modern emulator.

Achieved ~600 KB/s sustained throughput on my Octane2.

## Supported platforms

Architecture-independent: no DMA, no PCI, no cache flush required.

- IP22 (Indigo2, Challenge S) — n32/MIPS3
- IP30 (Octane) — 64-bit/MIPS4
- IP32 (O2) — n32/MIPS3
- IP35 (Fuel, Origin 350) — 64-bit/MIPS4

Tested on IP30 (Octane).

### IRIX 5.3

An IRIX 5.3 port lives in [`irix5.3/`](irix5.3/) — Indy, Indigo2, Challenge S
(IP22), Indigo (IP20/IP12) and Challenge/Onyx (IP19/IP21). 5.3 has no hwgraph,
no CDL, no loadable modules and is o32-only, so it needs its own copy of the
driver rather than a build flag. Build instructions: [IRIX 5.3](#irix-53)
below; design rationale and failure-mode triage in
[`irix5.3/RESUME.md`](irix5.3/RESUME.md).

The DaynaPort protocol code is shared as one file, `shared/dp_proto.c`,
`#include`d by both drivers, so a protocol fix lands in both automatically.

**Status: working on real hardware.** Confirmed on an Indigo R4000 (IP20)
running IRIX 5.3 with a DaynaPort-compatible target at SCSI id 3: `dp0`
attaches and carries traffic.

Before that it was brought up in [IRIS](https://github.com/techomancer/iris)
against its emulated DaynaPort target (`--features daynaport`), where `dp0`
attaches, reads its MAC from the device, resolves ARP, pings 4/4 with no loss
and completes a TCP handshake — that is where all the bugs were found and
fixed. IP22 (Indy/Indigo2) and IP12 (Indigo R3000) compile but have only been
run under emulation and not at all, respectively.

## How to build

Requires smake and the IRIX kernel build environment (`/var/sysgen`).

```sh
cd irix6.5

# Build for Octane (default)
smake CPUBOARD=IP30

# Build for O2
smake CPUBOARD=IP32
```

### Loadable module (no reboot required)

```sh
smake load       # load into running kernel
smake unload     # unload
smake reload     # unload + load
```

### Built-in kernel (permanent)

```sh
smake BUILTIN=1 install
```

Then add to `/var/sysgen/system/irix.sm`:
```
USE: dp
```

Then run:
```sh
autoconfig && reboot
```

On next boot `dp0` appears automatically.

### Debug builds

Use `XCFLAGS`, which is appended. A command-line `MYCFLAGS=` REPLACES the
Makefile's own definition and silently drops the flags that select the
built-in/loadable variant (and, on 5.3, `-DDP_ASYNC_RX`), which produces a
driver that either never attaches or panics on the first poll.

```sh
smake XCFLAGS="-DDP_LOG"            # init/attach/enable events
smake XCFLAGS="-DDP_LOG_SCSI"       # SCSI command traces (noisy)
smake XCFLAGS="-DDP_LOG_NET"        # RX/TX packet traces (very noisy)
smake XCFLAGS="-DDP_CHECK_ETHERIF"  # struct etherif layout canary
```

## IRIX 5.3

The 5.3 port lives in `irix5.3/`. It is a separate Makefile and a separate
`sgi_ether.h`, because 5.3 has no loadable modules (no `ml(1M)`), a different
SCSI interface (`scsi_command[]` indexed by adapter driver number rather than
lun vertex handles), and no hwgraph.

```sh
cd irix5.3
smake                      # CPUBOARD=IP22 (Indy / Indigo2 / Challenge S)
smake CPUBOARD=IP20        # Indigo R4000
smake CPUBOARD=IP19        # Challenge / Onyx
smake install              # -> /var/sysgen/{boot/dp.o,master.d/dp}
```

`CPUBOARD` is mandatory and defaults to IP22: 5.3's `/var/sysgen/Makefile.kernio`
defines `CFLAGS` only inside `#if defined(CPUBOARD)`, so an unset value yields
an EMPTY `CFLAGS` - no `-D_KERNEL`, no `-coff`, no R4000 errata workarounds. It
compiles, and produces an object that will wreck the kernel.

### Other boards: CPUBOARD must match `hinv`

`CPUBOARD` is the only thing that changes between boards - the Makefile
deliberately sets no ABI flags of its own, so `Makefile.kernio` supplies the
right ones. **Check what you actually have first**, because the names are not
intuitive: an Indigo R4000 is IP20, an Indigo R3000 is IP12, and an Indy is
IP22.

```sh
hinv | head -1              # e.g. "1 100 MHZ IP20 Processor"
```

| Machine | CPUBOARD |
|---|---|
| Indy, Indigo2 (R4000/R4400), Challenge S | `IP22` |
| **Indigo R4000** | **`IP20`** |
| Indigo R3000 | `IP12` |
| Challenge / Onyx | `IP19` |

```sh
cd irix5.3
smake CPUBOARD=IP20             # <- your board here
smake CPUBOARD=IP20 install     # installs dp.o, master.d/dp AND dp.sm
/etc/autoconfig -f
grep -c dp_start /var/sysgen/master.c   # >= 1 means lboot took it
/etc/shutdown -y -g0 -i6                # clean: promotes /unix.install
```

`install` writes `/var/sysgen/system/dp.sm` containing `INCLUDE: dp`. lboot
reads every `*.sm` in that directory, so the driver gets a file of its own -
`irix.sm` ships mode 444 and is rewritten by OS patches, and an append to it
fails silently for anyone not checking. Shut down cleanly rather than
resetting: `autoconfig` stages the kernel as `/unix.install` and only a clean
shutdown renames it, so a hard reset boots the old one and looks identical to
"the driver doesn't work".

The prebuilt objects under `dist/` are named for their board
(`dist/irix53-ip20/`, `dist/irix53-ip22/`). Installing one built for a
different board is not a small mistake - see the CFLAGS note below.

Check the compile line to confirm `CPUBOARD` took:

```
IP20  -D_K32U32 -D_KERNEL -DSTATIC=static -DJUMP_WAR -DPROBE_WAR -DBADVA_WAR -DIP20 -DR4000 ...
IP22  ... same, with -DIP22
IP12  -D_K32U32 -D_KERNEL -DSTATIC=static -DIP12 -DR3000 -Wx,-G8 -non_shared -coff -Wc,-pic0
```

`JUMP_WAR`/`PROBE_WAR`/`BADVA_WAR` are R4000 silicon errata workarounds: they
appear for IP20 and IP22, and must NOT appear for an R3000 IP12. An empty
CFLAGS means `CPUBOARD` did not take at all — stop, because that object has no
`-D_KERNEL` and no `-coff` and will wreck the kernel.

An object built for the wrong board does not fail to link and does not
necessarily crash: the most likely symptom is simply **no `dp0`**.

**Cross-building on a faster machine.** The object is o32 COFF and the flags
come from the board, so an IP12 object can be produced on any 5.3 host - for
instance inside the emulator, which is far faster than an Indigo:

```sh
shared/scripts/iris-build.sh --release 5.3 --cpuboard IP12 \
    --cflags "-DDP_LOG -DDP_CHECK_ETHERIF"
# -> dist/dp-irix53.o, built with -DIP12 -DR3000
```

Copy that to the Indigo as `/var/sysgen/boot/dp.o` together with
`master.d/dp`, then run `autoconfig -f` **there** — the kernel must be linked
on the machine that will boot it. `--cpuboard` refuses `--autoconfig` and
`--boot-test` for exactly this reason: the emulated guest is an IP22 and would
otherwise link a foreign object into its own kernel.

**Per-board status.** IP20 (Indigo R4000) is confirmed on hardware. IP22 is
verified under emulation only. IP12 (R3000) compiles and has never been
booted. The driver contains no assembly, no DMA setup and no cache maintenance
of its own, so there is nothing obviously board-specific in it — but build the
first kernel on any untried board with `-DDP_CHECK_ETHERIF`: `struct etherif`
comes from a reconstructed header, and a different kernel is a different
`struct ifnet`. An R3000 IP12 is the least-charted of these.

`smake install` also writes `/var/sysgen/system/dp.sm` containing
`INCLUDE: dp` — 5.3 uses `INCLUDE:`, not the `USE:` that 6.5 takes. Then:

```sh
/etc/autoconfig -f
/etc/shutdown -y -g0 -i6
```

On the next boot `dp0` appears, and `ifconfig` works as on 6.5:

```sh
ifconfig dp0 <ip> netmask <mask> up
```

### What differs on 5.3

- **The bus scan runs in the `io_start[]` phase**, from `dp_start()`, not in
  `dp_init()`. `master.c` calls `dp_init` fourth in `io_init[]`, before the
  host adapter has scanned the bus, so `scsi_info()` has nothing to return
  yet and the scan silently finds nothing.
- **The packet path is asynchronous** (`-DDP_ASYNC_RX`, on by default in
  `irix5.3/Makefile`). It has to be: the portable path sleeps in `psema()`
  from an `itimeout()` callback, and 5.3 runs those on the interrupt stack,
  where the sleep resumes at address 0 and panics the kernel. The tick
  submits one command and returns; the completion parses the response, hands
  frames up, and submits the next. 6.5 runs timeout callbacks on a thread and
  keeps the simpler synchronous poll.
- **`sgi_ether.h` is a reconstruction.** `ether.h` is a private kernel header
  IRIX does not ship. Build with `-DDP_CHECK_ETHERIF` to arm a canary that
  checks `ether_attach()` did not write past `struct etherif`. Note the canary
  cannot catch a wrong *offset* inside the struct - that class of bug is why
  the watchdog looks its softc up by `ifp` instead of using `ifptoeif()`.

## Usage

```sh
ifconfig dp0 <ip> netmask <mask> up
# or to make it the primary interface:
ifconfig eg0 down
ifconfig dp0 up primary
```

## How it works

- SCSI type-3 device registration via `scsi_driver_register()` and inventory
  walk at boot
- RX polled via a self-rescheduling 10ms `itimeout()` callback
- TX queued into a `VM_DIRECT` pool, drained interleaved with RX in `dp_runqueue`
- ZuluSCSI multi-packet READ mode: up to 2 Ethernet frames packed per SCSI READ
  response, parsed in a loop
- Two mutexes: `dp_qlock` serialises the queue drain loop; `dp_taillock`
  protects the TX queue tail for lockless enqueue from `eio_transmit`

## Protocol reference

SLINKCMD.TXT by Roger Burrows (rev 1.20) — authoritative DaynaPort SCSI
command set documentation.

## Authors

Dominik Behr with Claude (Anthropic) as co-author and rubber duck.

## License

BSD 3-Clause

## Why?

Because this Indy laptop from Twister needs WiFi when you stop at Starbucks.
