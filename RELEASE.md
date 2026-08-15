# Building and installing driver releases

A release ships, for each IRIX line, one kernel object per supported board and
one **inst tardist** — a Software Manager (`inst` / `swmgr`) installable
package that carries every board object and picks yours by `hinv` on install.

| Release | ABI | Boards built | Package |
|---|---|---|---|
| IRIX 5.3 | o32/coff | IP20, IP22, IP12, IP19 | `dp-<ver>-53.tardist` |
| IRIX 6.5 | n32 + 64-bit | IP22, IP32 (n32); IP28, IP30, IP35 (64) | `dp-<ver>-65.tardist` |

Every object is a **quiet** build (`-DDP_CHECK_ETHERIF` only — no per-op or
per-second logging). The two boot ID lines still print; nothing else does.

## Installing from the inst package (recommended)

On the target machine, with the matching `dp-<ver>-<53|65>.tardist`:

```sh
mkdir /tmp/dp && cd /tmp/dp
tar xf /path/to/dp-<ver>-53.tardist       # -> the product trio: dp dp.idb dp.sw
inst -f /tmp/dp                            # or: swmgr, point it here
#   Inst> install dp.sw.driver
#   Inst> go
#   Inst> quit
/var/sysgen/dp/dpinstall                   # picks YOUR board, relinks the kernel
/etc/shutdown -y -g0 -i6                   # clean reboot promotes the new kernel
```

`inst` drops all board objects under `/var/sysgen/boot/` (as `dp-ip20.o`,
`dp-ip22.o`, …), the `master.d/dp` file, the `system/dp.sm` config line, and the
`/var/sysgen/dp/dpinstall` helper. **The one manual step is running
`dpinstall`** — it reads `hinv`, copies the right board object to
`/var/sysgen/boot/dp.o`, and runs `autoconfig -f`. (gendist on these releases
does not support a post-install `exitop`, so the pick is a one-line manual
step rather than automatic.) Reboot and `dp0` appears.

To remove: `inst` / `swmgr` → remove `dp.sw.driver`, then `autoconfig -f` and
reboot.

## Installing from the CD (no inst)

`shared/scripts/mk-dp-cd.sh` builds an EFS CD carrying the objects plus an
`install.sh` that does the same `hinv` pick without `inst`. See
[irix5.3/README.md](irix5.3/README.md).

## Building a release

The GitHub Actions **Release** workflow (`.github/workflows/release.yml`, run
it from the Actions tab or push a `v*` tag) builds everything and publishes a
GitHub release. It needs two repository secrets — `IRIX53_DISK_URL` and
`IRIX65_DISK_URL`, each a download URL for a dev boot disk (blank root
password, IDO dev option installed so `gendist` is present).

Locally, with a dev disk and a built/downloaded `iris`:

```sh
shared/scripts/iris-release.sh --release 5.3 --version 1.2.3 \
    --image /path/to/irix53-dev.chd --iris-dir ../iris --outdir dist
shared/scripts/iris-release.sh --release 6.5 --version 1.2.3 \
    --image /path/to/irix65-dev.chd --iris-dir ../iris --outdir dist
```

Each boots the guest once, cross-builds every board object for that release
(compile-only for the boards the guest can't boot), and runs `gendist` to
produce the tardist. `--no-gendist` skips packaging and just emits the objects.

### Notes for anyone touching the pipeline

- **The guest is always an Indy** (IP22). It cross-builds foreign-board
  objects via `CPUBOARD`; only the native board could be boot-tested, which
  the release path skips (`iris-build.sh` is the boot-test path).
- **Some board objects come out byte-identical, and that is correct.** The
  driver is board-independent, so an object is decided purely by ABI +
  codegen flags. On 6.5 that collapses IP22≡IP32 (n32/mips3) and IP30≡IP35
  (mips4, same `-TARG`), with IP28 distinct (it gets `-TARG:t5_no_spec_stores`).
  The package still ships one per board name; `dpinstall` picks by `hinv`.
- **iris CPU variant `r4400` boots both dev disks.** An R5000 Indy needs IRIX
  6.2+, so r4400 covers 5.3 and 6.5. Only a disk explicitly installed as R5000
  needs the `r5000` iris variant.
- **All in-guest packaging runs from one staged script** (`guest-mkdist.sh`),
  invoked with a single serial command. Do not go back to firing many
  `ser_send` lines in a row — the shell garbles back-to-back sends and the idb
  came out empty.
- **No `sort -k5`.** IRIX 5.3's System V `sort` rejects POSIX `-k`
  ("Incorrect usage") and returns an empty file, which makes gendist emit
  "EMPTY product … removed". The idb is emitted already in destination-path
  order instead.
