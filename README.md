# irixdayna — DaynaPort SCSI/Link Ethernet Driver for IRIX

## What is it

A native IRIX kernel driver for the DaynaPort SCSI/Link Ethernet adapter
(DP0801/DP0802) and compatible emulators including ZuluSCSI, BlueSCSI V2,
PiSCSI.

Two drivers share one protocol implementation: **IRIX 6.5** in the repository
root, and an **IRIX 5.3** port under `irix5.3/`. The code between the
`BEGIN SHARED`/`END SHARED` markers is byte-for-byte identical in both, and
`irix5.3/drift.sh` enforces that - a protocol fix belongs in both files.

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

The DaynaPort protocol code is shared verbatim between the two — `sh
irix5.3/drift.sh` verifies the two files still agree, and protocol fixes belong
in both.

**Status: 5.3 moves packets under emulation, and has not yet been run on
hardware.** In [IRIS](https://github.com/techomancer/iris) with its emulated
DaynaPort target (`--features daynaport`), `dp0` attaches, reads its MAC from
the device, resolves ARP, and pings the gateway 4/4 with no loss. Everything
the emulator can exercise, it passes; a real BlueSCSI/ZuluSCSI on a real bus is
the remaining unknown.

## How to build

Requires smake and the IRIX kernel build environment (`/var/sysgen`).

```sh
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

Then add the driver to `/var/sysgen/system/irix.sm`:

```
INCLUDE: dp
```

(5.3 uses `INCLUDE:`, not the `USE:` that 6.5 takes.) Then:

```sh
autoconfig -f && reboot
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
