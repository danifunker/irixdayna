# irixdayna — DaynaPort SCSI/Link Ethernet Driver for IRIX 6.5

## What is it

A native IRIX 6.5 kernel driver for the DaynaPort SCSI/Link Ethernet adapter
(DP0801/DP0802) and compatible emulators including ZuluSCSI, BlueSCSI V2,
PiSCSI.

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

Requires IRIX 6.5. Tested on IP30 (Octane).

### IRIX 5.3

An IRIX 5.3 port lives in [`irix5.3/`](irix5.3/) — Indy, Indigo2, Challenge S
(IP22), Indigo (IP20/IP12) and Challenge/Onyx (IP19/IP21). 5.3 has no hwgraph,
no CDL, no loadable modules and is o32-only, so it needs its own copy of the
driver rather than a build flag; see [`irix5.3/README.md`](irix5.3/README.md).

The DaynaPort protocol code is shared verbatim between the two — `sh
irix5.3/drift.sh` verifies the two files still agree, and protocol fixes belong
in both.

**The 5.3 port has not yet been run on hardware.** Design rationale, the
pre-boot verification checklist and failure-mode triage are in
[`irix5.3/RESUME.md`](irix5.3/RESUME.md).

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

```sh
smake MYCFLAGS="-DDP_LOG"            # init/attach/enable events
smake MYCFLAGS="-DDP_LOG_SCSI"       # SCSI command traces
smake MYCFLAGS="-DDP_LOG_NET"        # RX/TX packet traces
```

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
