/*
 * if_dp.c - DaynaPort SCSI/Link Ethernet driver for IRIX 5.3
 *
 * This is the IRIX 5.3 (o32, 32-bit) port of the IRIX 6.5 driver in
 * ../if_dp.c.  The DaynaPort protocol, the RX multi-packet parser, the
 * TX ring and the ifnet/etherif handlers are IDENTICAL to the 6.5
 * driver; only device discovery, SCSI submission and the kernel
 * locking primitives differ.
 *
 * To keep the two files honest, the region between the
 *   "BEGIN SHARED" / "END SHARED" markers below is byte-for-byte
 * identical to the corresponding region of ../if_dp.c.  Run
 *   ./drift.sh
 * (or "smake drift") to verify.  Fix protocol bugs in BOTH files.
 *
 * What is different from 6.5, and why:
 *
 *   - No hwgraph.  IRIX 5.3 has no /hw filesystem, no vertex_hdl_t and
 *     no scsi_lun_info_t.  Devices are addressed by the integer triple
 *     (adapter, target, lun).
 *
 *   - No CDL / scsi_driver_register().  5.3 dispatches nothing to us,
 *     so dp_init() walks the bus itself with scsi_info[]() and matches
 *     the INQUIRY vendor/product strings.
 *
 *   - No loadable modules.  IRIX 5.3 has no ml(1M); the driver must be
 *     linked into the kernel with lboot.  See README.md.
 *
 *   - No mutex_t.  5.3 provides initnsema/psema/vsema/cpsema/freesema.
 *     The shim below maps the 6.5 mutex names onto those so that the
 *     shared region needs no #ifdef.
 *
 * References (SGI, IRIX 5.3 Device Driver Programming Guide):
 *   ch05 "Writing a SCSI Device Driver"      - scsi_info/alloc/free/command
 *   ch08 "Writing Multiprocessor Device Drivers" - semaphore calls
 *   ch09 "Writing Network Device Drivers"    - ifnet conventions
 *
 * Protocol reference: SLINKCMD.TXT (Roger Burrows, rev 1.20)
 *
 * Build with -DDP_LOG to enable verbose kernel logging.
 *
 * VERIFICATION STATUS.  The 5.3 interfaces used here were checked
 * against headers and the /unix symbol table extracted from an IRIX 5.3
 * IP22 (Indy) disk image.  Confirmed present and correctly shaped:
 * ether_attach, ether_input, add_to_inventory, m_vget, m_freem, psema,
 * vsema, cpsema, initnsema, initnsema_mutex, freesema, itimeout,
 * untimeout, plbase, kmem_zalloc, kmem_free, sprintf, cmn_err, delay,
 * scsi_driver_table, scsi_info, scsi_alloc, scsi_free, scsi_command.
 *
 * ONE UNKNOWN REMAINS: the layout of struct etherif (see sgi_ether.h).
 * ether_attach exists, but ether.h is not shipped and the kernel carries
 * no struct debug info, so the member list cannot be recovered without
 * running code.  Build the first kernel on any machine with
 * -DDP_CHECK_ETHERIF; see README.md.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/systm.h"
#include "sys/cmn_err.h"
#include "sys/errno.h"
#include "sys/invent.h"
#include "sys/mbuf.h"
#include "sys/socket.h"
#include "sys/kmem.h"
#include "sys/sema.h"
#include "sys/cred.h"
#include "sys/conf.h"   /* D_MP - 6.5 picks this up via another path */
#include "sys/ddi.h"
#include "sys/scsi.h"
#include "net/if.h"
#include "net/raw.h"
#include "net/soioctl.h"
#include "sgi_ether.h"
#include "netinet/in.h"
#include "netinet/if_ether.h"
#include "string.h"

/* -----------------------------------------------------------------------
 * IRIX 6.5 API shim
 *
 * The shared region below is copied verbatim from the 6.5 driver, so it
 * calls 6.5 names.  Map them onto the 5.3 equivalents here rather than
 * scattering #ifdefs through the protocol code.
 *
 * The mutexes become 5.3 mutex-semaphores.  The softc declares them as
 * sema_t directly: 5.3 spells the Sun-compat alias kmutex_t, not
 * mutex_t, so there is nothing to typedef and nothing to collide with.
 *
 * The #undefs are NOT defensive padding - sys/sema.h really does define
 * mutex_init, with a DIFFERENT four-argument shape:
 *     #define mutex_init(m, nm, f, i)  initnsema_mutex(m, nm)
 * (name second, not third).  Without the #undef the 6.5-style three-arg
 * call below expands wrongly.  mutex_lock/mutex_unlock/mutex_trylock are
 * not defined by 5.3 at all - it uses mutex_enter/mutex_exit - so those
 * #undefs are merely belt-and-braces.
 *
 * Verified against sys/sema.h and sys/ddi.h from an IRIX 5.3 IP22 disk
 * image, and every symbol below was confirmed present in that image's
 * /unix symbol table.
 * ----------------------------------------------------------------------- */

#ifndef MUTEX_DEFAULT
#define MUTEX_DEFAULT   0
#endif

#undef  mutex_init
#undef  mutex_destroy
#undef  mutex_lock
#undef  mutex_unlock

/* initnsema_mutex() is 5.3's dedicated mutex-semaphore initialiser. */
#define mutex_init(m, type, name)   initnsema_mutex((m), (name))
#define mutex_destroy(m)            freesema(m)
#define mutex_lock(m, pri)          ((void)psema((m), (pri)))
#define mutex_unlock(m)             ((void)vsema(m))

/*
 * cpsema() returns non-zero when the lock was acquired - confirmed by
 * sys/sema.h, which on a uniprocessor build aliases
 *     #define apcpsema(x)  1
 * i.e. "conditional acquire always succeeds", which only type-checks as
 * a success indicator if non-zero means acquired.
 *
 * DP_NO_TRYLOCK remains available: it disables the transmit-side queue
 * kick, which is a latency optimisation only - the 10ms poll timer still
 * drains the ring.  Keep it in reach if the TX ring is ever suspected.
 */
#undef  mutex_trylock
#ifdef DP_NO_TRYLOCK
#define mutex_trylock(m)            (0)
#else
#define mutex_trylock(m)            (cpsema(m))
#endif

/* 6.5 spells this init_sema(sema, value, name, unit) */
#undef  init_sema
#define init_sema(s, v, name, unit) initnsema((s), (v), (name))

/*
 * plbase (sys/ddi.h: "extern pl_t plbase") and
 * itimeout(void (*)(), void *, long, pl_t, ...) both exist on 5.3, so
 * dp_runqueue()'s timer call needs no adjustment.
 *
 * kmem_zalloc() flags are native too: KM_SLEEP is 0 in sys/kmem.h, and
 * sys/immu.h defines VM_DIRECT 0x0100 and VM_CACHEALIGN 0x0800.
 * VM_DIRECT|VM_CACHEALIGN keeps the buffers in k0/k1seg and cache
 * aligned so the host adapter can DMA straight into them.
 */

/*
 * SN_MORETOCOME does not exist on IRIX 5.3.  net/raw.h there defines only
 * SN_PROMISC / SN_ERROR / SN_TRAILER and the SNERR_* codes; the
 * "more packets follow, don't wake the protocol side yet" hint was added
 * later.  The value reaches ether_input() as snoopflags and is OR'd into
 * the snoopheader, so 0 is the correct 5.3 equivalent: we simply lose a
 * batching optimisation on multi-packet READ responses.  Purely advisory
 * - no correctness impact - and it keeps dp_do_rx() byte-identical to the
 * 6.5 driver.
 */
#ifndef SN_MORETOCOME
#define SN_MORETOCOME   0
#endif

/* -----------------------------------------------------------------------
 * Module identity
 *
 * No M_VERSION / dp_mversion: IRIX 5.3 has no loadable module support.
 * lboot assumes a pre-5.0 "old style" driver unless drvdevflag exists,
 * so it must be defined even though we set only D_MP.
 * ----------------------------------------------------------------------- */

int   dp_devflag  = D_MP;

/* -----------------------------------------------------------------------
 * Logging
 * ----------------------------------------------------------------------- */

#ifdef DP_LOG
#define DPLOG(x)    cmn_err x
#else
#define DPLOG(x)
#endif

#ifdef DP_LOG_SCSI
#define SCSILOG(x)  cmn_err x
#else
#define SCSILOG(x)
#endif

#ifdef DP_LOG_NET
#define NETLOG(x)   cmn_err x
#else
#define NETLOG(x)
#endif

/* -----------------------------------------------------------------------
 * DaynaPort SCSI command opcodes  (identical to 6.5)
 * ----------------------------------------------------------------------- */

#define DP_READ         0x08    /* READ(6)  - receive packet(s)        */
#define DP_GET_STATS    0x09    /* RETRIEVE STATISTICS - get MAC + ctr */
#define DP_WRITE        0x0A    /* WRITE(6) - transmit packet          */
#define DP_SET_MODE     0x0C    /* SET INTERFACE MODE                  */
#define DP_ENABLE       0x0E    /* ENABLE/DISABLE interface            */

#define DP_STATS_LEN    18      /* GET_STATS returns 18 bytes          */
#define DP_RX_BUFLEN    3072    /* 2x max ZuluSCSI multi-packet response (2x1530 = 3060, rounded up) */
#define DP_RX_BUFSZ     (DP_RX_BUFLEN + 4) /* +4 for CRC slop         */
#define DP_TX_BUFSZ     2048   /* one max Ethernet frame, 2KB aligned  */
#define DP_RX_HDR       6      /* length+flags prefix on each RX record*/
#define DP_CRC_LEN      4      /* trailing CRC bytes to strip         */
#define DP_SENSE_LEN    32     /* sense buffer size                   */
#define DP_CDB_LEN      6      /* all DaynaPort CDBs are 6 bytes      */
#define DP_ENABLE_POST_DELAY (HZ/2) /* 0.5s after ENABLE before cmds  */

/* READ CDB byte[5] - undocumented "read flags"; 0xC0 works universally */
#define DP_READ_FLAGS   0xC0

/* RX record flag field values (bytes 2-5 of each record header) */
#define DP_RX_MORE      0x00000010U     /* more packets pending         */
#define DP_RX_DROPPED   0xFFFFFFFFU     /* packet dropped; reset needed */

/* SET MODE byte[4] sub-flags */
#define DP_MODE_BCAST   0x04    /* enable broadcast reception          */

/* max units; SCSI bus allows 7 initiators, one slot each */
#define DP_MAXUNITS     8

/* inventory controller type - not in system invent.h, pick unused slot */
#define INV_ETHER_DP    43      /* DaynaPort SCSI/Link Ethernet */

#define DP_TX_QLEN      8       /* max frames in TX queue               */

/* SCSI peripheral device type we bind to: 3 == Processor */
#define DP_SCSI_TYPE    3

/* -----------------------------------------------------------------------
 * Per-interface soft state
 *
 * Differs from 6.5 only in the device address: 5.3 has no hwgraph, so we
 * keep the (adapter, target, lun) triple plus the host adapter driver
 * number used to index scsi_command[]/scsi_free[].
 * ----------------------------------------------------------------------- */

struct dp_txframe {
    u_char *data;   /* points into dp_txpool; VM_CACHEALIGN|VM_DIRECT */
    int     len;
};

/*
 * DP_CHECK_ETHERIF - canary for the struct etherif layout risk.
 *
 * sgi_ether.h is a reconstruction: ether.h is a private kernel header
 * that IRIX does not ship.  If the real 5.3 struct etherif has more
 * members than our copy, ether_attach() writes past dp_eif and silently
 * corrupts the softc - no link error, no warning.
 *
 * Building with -DDP_CHECK_ETHERIF places a magic pattern immediately
 * after dp_eif and verifies it survives ether_attach().  Use it on the
 * first boot on any new machine; the cost is 32 bytes per interface.
 */
#ifdef DP_CHECK_ETHERIF
#define DP_EIF_GUARD_WORDS  8
#define DP_EIF_GUARD_MAGIC  0x5A5AD9D9U
#endif

struct dp_softc {
    struct etherif      dp_eif;             /* MUST be first - ifptoeif() */
#ifdef DP_CHECK_ETHERIF
    uint                dp_eif_guard[DP_EIF_GUARD_WORDS];
#endif
    int                 dp_adap;            /* SCSI adapter (controller)  */
    int                 dp_target;          /* SCSI target id             */
    int                 dp_lun;             /* SCSI logical unit          */
    int                 dp_drvnum;          /* host adapter driver number */
    sema_t              dp_sema;            /* SCSI command completion     */
    sema_t              dp_qlock;           /* serialises dp_runqueue      */
    sema_t              dp_taillock;        /* protects tx queue tail      */
    toid_t              dp_timer;           /* RX poll timer id           */
    volatile int        dp_enabled;         /* 1 after eio_init           */
    int                 dp_unit;
    scsi_request_t      dp_req;             /* embedded - no separate alloc */
    u_char              dp_cdb[DP_CDB_LEN]; /* CDB - must outlive command */
    u_char             *dp_sense;           /* VM_CACHEALIGN|VM_DIRECT    */
    u_char             *dp_rxbuf;           /* VM_CACHEALIGN|VM_DIRECT    */
    u_char             *dp_txpool;          /* VM_CACHEALIGN|VM_DIRECT, DP_TX_QLEN*DP_TX_BUFSZ */
    struct dp_txframe   dp_txq[DP_TX_QLEN]; /* TX frame queue             */
    int                 dp_txq_head;        /* dequeue index (qlock)      */
    int                 dp_txq_tail;        /* enqueue index (taillock)   */
    int                 dp_txq_len;         /* current depth (both locks) */
    /* Non-zero while a foreground (user-context) SCSI command holds dp_qlock.
     * dp_timer_kick() checks it and skips the tick rather than blocking on
     * the mutex from timeout context - see the comment there. */
    volatile int        dp_fg;
};

static int              dp_nunit = 0;
static struct dp_softc *dp_units[DP_MAXUNITS];

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */

static int  dp_eio_init(struct etherif *, int);
static void dp_eio_reset(struct etherif *);
static void dp_eio_watchdog(struct ifnet *);
static int  dp_eio_transmit(struct etherif *, struct etheraddr *,
                            struct etheraddr *, u_short, struct mbuf *);
static int  dp_eio_ioctl(struct etherif *, int, void *);
static int  dp_do_attach(int, int, int, int);

static struct etherifops dp_ops = {
    dp_eio_init,
    dp_eio_reset,
    dp_eio_watchdog,
    dp_eio_transmit,
    dp_eio_ioctl,
};

static void dp_runqueue(struct dp_softc *);
static void dp_timer_kick(struct dp_softc *);

/* -----------------------------------------------------------------------
 * dp_scsi_cmd - issue a SCSI command and wait for completion.
 * Caller must hold dp_qlock.  dir=SRF_DIR_IN for reads, 0 for writes.
 *
 * 5.3 difference: the request is addressed by integers rather than by a
 * lun vertex handle, and it is submitted through the scsi_command[]
 * function-pointer array indexed by host adapter driver number.
 * ----------------------------------------------------------------------- */

static void
dp_scsi_done(scsi_request_t *req)
{
    struct dp_softc *sc = (struct dp_softc *)req->sr_dev;
    vsema(&sc->dp_sema);
}

static int
dp_scsi_cmd(struct dp_softc *sc, u_char *cdb, int cdblen,
            void *buf, int buflen, ushort dir)
{
    scsi_request_t *req = &sc->dp_req;
    int status;

    bzero(req, sizeof *req);
    bcopy(cdb, sc->dp_cdb, cdblen);
    req->sr_ctlr     = (u_char)sc->dp_adap;
    req->sr_target   = (u_char)sc->dp_target;
    req->sr_lun      = (u_char)sc->dp_lun;
    req->sr_command  = sc->dp_cdb;
    req->sr_cmdlen   = (ushort)cdblen;
    req->sr_timeout  = 10 * HZ;
    req->sr_sense    = sc->dp_sense;
    req->sr_senselen = DP_SENSE_LEN;
    req->sr_notify   = dp_scsi_done;
    req->sr_dev      = (void *)sc;
    req->sr_flags    = SRF_AEN_ACK | SRF_FLUSH;

    if (buflen > 0) {
        req->sr_buffer = (u_char *)buf;
        req->sr_buflen = (uint)buflen;
        req->sr_flags |= dir;
    }

    if (cdb[0] != DP_READ)
        SCSILOG((CE_NOTE, "dp%d: scsi_cmd op=0x%02x buflen=%d dir=0x%x\n",
               sc->dp_unit, cdb[0], buflen, (int)dir));

    (*scsi_command[sc->dp_drvnum])(req);
    psema(&sc->dp_sema, PRIBIO);

    status = (int)req->sr_status;

    if (cdb[0] != DP_READ)
        SCSILOG((CE_NOTE, "dp%d: scsi_cmd op=0x%02x -> status=%d scsi_status=%d resid=%d\n",
               sc->dp_unit, cdb[0], status, (int)req->sr_scsi_status, (int)req->sr_resid));

    return status;
}

/* =======================================================================
 * BEGIN SHARED WITH ../if_dp.c
 *
 * Everything from here to "END SHARED" is byte-for-byte identical to the
 * 6.5 driver.  Do not edit one copy without editing the other; run
 * ./drift.sh to check.
 * ======================================================================= */

/* dp_scsi_cmd_locked - acquire dp_qlock, run command, release.
 * Use for control commands (enable, set_mode, get_mac) called outside
 * of dp_runqueue context. */
static int
dp_scsi_cmd_locked(struct dp_softc *sc, u_char *cdb, int cdblen,
                   void *buf, int buflen, ushort dir)
{
    int status;
    sc->dp_fg++;                /* tell the poll tick to stand off */
    mutex_lock(&sc->dp_qlock, PZERO);
    status = dp_scsi_cmd(sc, cdb, cdblen, buf, buflen, dir);
    mutex_unlock(&sc->dp_qlock);
    sc->dp_fg--;
    return status;
}

/* -----------------------------------------------------------------------
 * dp_enable - send ENABLE or DISABLE CDB
 * ----------------------------------------------------------------------- */

static void
dp_enable(struct dp_softc *sc, int on)
{
    u_char cdb[6];
    cdb[0] = DP_ENABLE; cdb[1] = 0; cdb[2] = 0;
    cdb[3] = 0; cdb[4] = 0; cdb[5] = on ? 0x80 : 0x00;

    DPLOG((CE_NOTE, "dp%d: %s interface\n", sc->dp_unit,
           on ? "enabling" : "disabling"));

    dp_scsi_cmd_locked(sc, cdb, 6, NULL, 0, 0);
    delay(DP_ENABLE_POST_DELAY);
}

/* -----------------------------------------------------------------------
 * dp_set_mode - SET INTERFACE MODE to enable broadcast reception
 * ----------------------------------------------------------------------- */

static void
dp_set_mode(struct dp_softc *sc)
{
    u_char cdb[6];
    cdb[0] = DP_SET_MODE; cdb[1] = 0; cdb[2] = 0;
    cdb[3] = 0; cdb[4] = DP_MODE_BCAST; cdb[5] = 0x80;

    DPLOG((CE_NOTE, "dp%d: set_mode (broadcast)\n", sc->dp_unit));

    dp_scsi_cmd_locked(sc, cdb, 6, NULL, 0, 0);
}

/* -----------------------------------------------------------------------
 * dp_get_mac - RETRIEVE STATISTICS to read MAC address
 * Returns 1 on success, 0 on failure.
 * ----------------------------------------------------------------------- */

static int
dp_get_mac(struct dp_softc *sc, u_char *mac)
{
    u_char cdb[6];
    int err;

    cdb[0] = DP_GET_STATS; cdb[1] = 0; cdb[2] = 0;
    cdb[3] = 0; cdb[4] = DP_STATS_LEN; cdb[5] = 0;

    DPLOG((CE_NOTE, "dp%d: get_mac\n", sc->dp_unit));

    err = dp_scsi_cmd_locked(sc, cdb, 6, sc->dp_rxbuf, DP_STATS_LEN, SRF_DIR_IN);
    if (err == 0) {
        char macstr[18];
        bcopy(sc->dp_rxbuf, mac, 6);
        sprintf(macstr, "%x:%x:%x:%x:%x:%x",
                (uint)mac[0], (uint)mac[1], (uint)mac[2],
                (uint)mac[3], (uint)mac[4], (uint)mac[5]);
        DPLOG((CE_NOTE, "dp%d: MAC %s\n", sc->dp_unit, macstr));
    } else {
        DPLOG((CE_NOTE, "dp%d: get_mac failed, sr_status=%d\n",
               sc->dp_unit, err));
    }
    return (err == 0);
}

/* -----------------------------------------------------------------------
 * dp_do_rx - issue one READ and dispatch all frames packed in the response.
 * Caller must hold dp_qlock.
 *
 * ZuluSCSI multi-packet mode (cdb[5] bit 6 set) packs multiple frames
 * back-to-back in a single READ response, each with its own 6-byte header:
 *   [0,1]   pktlen  big-endian, includes 4-byte CRC, excludes itself+flags
 *   [2..5]  flags   0x00000010 = more in next READ, 0xFFFFFFFF = dropped
 *   [6..]   full Ethernet frame
 * The last frame in the buffer has flags == 0 (or DP_RX_MORE if device
 * still has more queued for the next READ).
 *
 * Returns 1 if DP_RX_MORE was set on the last frame (issue another READ).
 * ----------------------------------------------------------------------- */

static int
dp_do_rx(struct dp_softc *sc)
{
    u_char cdb[6];
    struct ifnet *ifp = eiftoifp(&sc->dp_eif);
    uint pktlen, flags, framelen, mbuflen;
    struct mbuf *m;
    struct etherbufhead *ebh;
    int snoopflags;
    int err;
    u_char *p;
    u_char *end;
    int last_more;

    cdb[0] = DP_READ;
    cdb[1] = 0;
    cdb[2] = 0;
    cdb[3] = (DP_RX_BUFLEN >> 8) & 0xFF;
    cdb[4] = DP_RX_BUFLEN & 0xFF;
    cdb[5] = DP_READ_FLAGS;

    bzero(sc->dp_rxbuf, DP_RX_BUFSZ);

    err = dp_scsi_cmd(sc, cdb, 6, sc->dp_rxbuf, DP_RX_BUFLEN, SRF_DIR_IN);
    if (err != 0) {
        SCSILOG((CE_NOTE, "dp%d: rx READ failed sr_status=%d\n",
               sc->dp_unit, err));
        return 0;
    }

    p        = sc->dp_rxbuf;
    end      = sc->dp_rxbuf + DP_RX_BUFLEN;
    last_more = 0;

    for (;;) {
        if (p + DP_RX_HDR > end)
            break;

        pktlen = ((uint)p[0] << 8) | p[1];
        flags  = ((uint)p[2] << 24) | ((uint)p[3] << 16)
               | ((uint)p[4] << 8)  |  (uint)p[5];

        if (pktlen == 0)
            break;

        NETLOG((CE_NOTE, "dp%d: rx pktlen=%u flags=0x%08x\n",
               sc->dp_unit, pktlen, flags));

        if (flags == DP_RX_DROPPED) {
            cmn_err(CE_WARN, "dp%d: packet dropped, resetting\n", sc->dp_unit);
            dp_enable(sc, 0);
            dp_enable(sc, 1);
            dp_set_mode(sc);
            return 0;
        }

        if (pktlen <= DP_CRC_LEN) {
            NETLOG((CE_NOTE, "dp%d: rx pktlen %u too small\n", sc->dp_unit, pktlen));
            break;
        }
        framelen = pktlen - DP_CRC_LEN;

        if (framelen < sizeof(struct ether_header)) {
            NETLOG((CE_NOTE, "dp%d: rx framelen %u < ether header\n",
                   sc->dp_unit, framelen));
            break;
        }

        if (p + DP_RX_HDR + framelen > end) {
            NETLOG((CE_NOTE, "dp%d: rx frame overruns buffer\n", sc->dp_unit));
            break;
        }

        mbuflen = (uint)sizeof(struct etherbufhead)
                + framelen - (uint)sizeof(struct ether_header);

        NETLOG((CE_NOTE, "dp%d: rx framelen=%u mbuflen=%u more=%d\n",
               sc->dp_unit, framelen, mbuflen, (flags & DP_RX_MORE) ? 1 : 0));

        m = m_vget(M_DONTWAIT, (int)mbuflen, MT_DATA);
        if (m == NULL) {
            NETLOG((CE_NOTE, "dp%d: rx m_vget failed\n", sc->dp_unit));
            ifp->if_ierrors++;
            break;
        }

        ebh = mtod(m, struct etherbufhead *);
        IF_INITHEADER(ebh, ifp, sizeof(struct etherbufhead));
        bcopy(p + DP_RX_HDR, &ebh->ebh_ether, framelen);
        m->m_len = (int)mbuflen;
        ifp->if_ipackets++;
        ifp->if_ibytes += framelen - sizeof(struct ether_header);

        /* SN_MORETOCOME only if more frames follow in this same buffer */
        snoopflags = ((flags & DP_RX_MORE) && (p + DP_RX_HDR + pktlen + DP_RX_HDR <= end))
                     ? SN_MORETOCOME : 0;
        ether_input(&sc->dp_eif, snoopflags, m);

        last_more = (flags & DP_RX_MORE) ? 1 : 0;

        /* advance to next packed frame: header + pktlen (includes CRC) */
        p += DP_RX_HDR + pktlen;

        /* if MORE is clear this was the last frame in this READ response */
        if (!last_more)
            break;
    }

    return last_more;
}

/* -----------------------------------------------------------------------
 * dp_do_tx - transmit one frame from the TX queue head.
 * Caller must hold dp_qlock.  Returns 1 if a frame was sent, 0 if empty.
 * ----------------------------------------------------------------------- */

static int
dp_do_tx(struct dp_softc *sc)
{
    struct ifnet *ifp = eiftoifp(&sc->dp_eif);
    struct dp_txframe *tf;
    u_char cdb[6];
    int pktlen;
    int err;

    mutex_lock(&sc->dp_taillock, PZERO);
    if (sc->dp_txq_len == 0) {
        mutex_unlock(&sc->dp_taillock);
        return 0;
    }
    mutex_unlock(&sc->dp_taillock);

    tf = &sc->dp_txq[sc->dp_txq_head];
    pktlen = tf->len;

    cdb[0] = DP_WRITE;
    cdb[1] = 0;
    cdb[2] = 0;
    cdb[3] = (pktlen >> 8) & 0xFF;
    cdb[4] = pktlen & 0xFF;
    cdb[5] = 0x00;

    err = dp_scsi_cmd(sc, cdb, 6, tf->data, pktlen, 0);
    if (err != 0) {
        SCSILOG((CE_NOTE, "dp%d: tx WRITE failed sr_status=%d\n",
               sc->dp_unit, err));
        ifp->if_oerrors++;
    } else {
        ifp->if_opackets++;
        ifp->if_obytes += pktlen - (int)sizeof(struct ether_header);
    }

    mutex_lock(&sc->dp_taillock, PZERO);
    sc->dp_txq_head = (sc->dp_txq_head + 1) % DP_TX_QLEN;
    sc->dp_txq_len--;
    mutex_unlock(&sc->dp_taillock);

    return 1;
}

/* -----------------------------------------------------------------------
 * dp_runqueue - interleaved TX/RX drain loop.
 * Caller must hold dp_qlock.  Cancels the poll timer on entry and
 * re-arms it on exit.
 * ----------------------------------------------------------------------- */

static void
dp_runqueue(struct dp_softc *sc)
{
    int more_rx;
    int more_tx;

    if (sc->dp_timer) {
        untimeout(sc->dp_timer);
        sc->dp_timer = 0;
    }

    if (!sc->dp_enabled)
        return;

    do {
        more_tx = dp_do_tx(sc);
        do {
            more_rx = dp_do_rx(sc);
        } while (more_rx);
    } while (more_tx);

    sc->dp_timer = itimeout((void (*)())dp_timer_kick, (void *)sc,
                            HZ / 100, plbase);
}

/* -----------------------------------------------------------------------
 * dp_timer_kick - timer callback: acquire dp_qlock and run queue
 * ----------------------------------------------------------------------- */

static void
dp_timer_kick(struct dp_softc *sc)
{
    /*
     * This handle has just fired, so it is dead. Clear it before anything
     * else: dp_runqueue() below calls untimeout(sc->dp_timer) on entry, and
     * untimeout() on an already-fired handle corrupts the callout list. The
     * corruption shows up later as a dispatch through a null callback -
     *     PANIC: exception on IDLE stack k1:0x20 epc:0x0
     * i.e. the kernel jumping to address 0 from timeout context, long after
     * the command that provoked it has finished.
     */
    sc->dp_timer = 0;

    if (!sc->dp_enabled)
        return;
    /*
     * This runs from itimeout(), i.e. in timeout context. mutex_lock() there
     * is only safe if the mutex is free: a foreground SCSI command holds
     * dp_qlock for the duration of a command (milliseconds), and blocking on
     * it here means sleeping in timeout context, which on IRIX 5.3 ends as
     *
     *     Kernel/Interrupt Stack Overflow @0x0
     *     PANIC: stack underflow/overflow
     *
     * Any ifconfig/ioctl that reaches the device - set_mode from SIOCADDMULTI,
     * for instance - is enough to trigger it once the poll is armed. So skip
     * this tick entirely when a foreground command is in flight and come back
     * in 10ms; there is nothing time-critical about a poll.
     */
    if (sc->dp_fg) {
        sc->dp_timer = itimeout((void (*)())dp_timer_kick, (void *)sc,
                                HZ / 100, plbase);
        return;
    }
    mutex_lock(&sc->dp_qlock, PZERO);
    dp_runqueue(sc);
    mutex_unlock(&sc->dp_qlock);
}

static void
dp_runqueue_stop(struct dp_softc *sc)
{
    sc->dp_enabled = 0;
    mutex_lock(&sc->dp_qlock, PZERO);
    /* The timer is armed far more often than not: dp_runqueue() re-arms it at
     * the end of every poll. Dropping the handle without untimeout() leaves a
     * callback pending that can still walk into dp_runqueue and race whatever
     * control command asked us to stop. */
    if (sc->dp_timer) {
        untimeout(sc->dp_timer);
        sc->dp_timer = 0;
    }
    mutex_unlock(&sc->dp_qlock);
}

static void
dp_runqueue_start(struct dp_softc *sc)
{
    sc->dp_enabled = 1;
    dp_timer_kick(sc);
}

/* -----------------------------------------------------------------------
 * eio_init - bring the interface up
 * Called by ether_init() when ifconfig sets IFF_UP.
 * ----------------------------------------------------------------------- */

static int
dp_eio_init(struct etherif *eif, int flags)
{
    struct dp_softc *sc = (struct dp_softc *)eif->eif_private;
    struct ifnet *ifp = eiftoifp(eif);
    (void)flags;

    DPLOG((CE_NOTE, "dp%d: eio_init\n", sc->dp_unit));

    /*
     * The ether layer calls init again (and reset, below) on an interface
     * that is already up - ifconfig up alone produces init, reset, init.
     * Every one of those issues SCSI commands from user context, and once
     * dp_runqueue_start() has armed the poll, they nest on top of a poll
     * sleeping in psema ON THE INTERRUPT STACK. 5.3's is small, and the
     * second init reliably overflows it:
     *
     *     Kernel/Interrupt Stack Overflow @0x0 sp:0x881aa578
     *     PANIC: stack underflow/overflow
     *
     * Skipping the redundant work keeps anything from nesting. This is a
     * containment measure, not the cure: the cure is for dp_runqueue() to
     * stop issuing sleeping SCSI commands from an itimeout callback and run
     * in process context instead.
     */
    if (sc->dp_enabled) {
        DPLOG((CE_NOTE, "dp%d: eio_init: already up, nothing to do\n", sc->dp_unit));
        return 0;
    }

    /* Read real MAC now that we can sleep (called from ifconfig context). */
    {
        u_char mac[6];
        if (dp_get_mac(sc, mac)) {
            char macstr[18];
            bcopy(mac, eif->eif_addr.ea_vec, 6);
            bcopy(mac, eif->eif_arpcom.ac_enaddr, 6);
            sprintf(macstr, "%x:%x:%x:%x:%x:%x",
                    (uint)mac[0], (uint)mac[1], (uint)mac[2],
                    (uint)mac[3], (uint)mac[4], (uint)mac[5]);
            DPLOG((CE_NOTE, "dp%d: MAC %s\n", sc->dp_unit, macstr));
        }
    }

    dp_enable(sc, 1);
    dp_set_mode(sc);

    ifp->if_flags |= IFF_RUNNING;
    ifp->if_timer  = IFNET_SLOWHZ;     /* arm watchdog */

    dp_runqueue_start(sc);

    DPLOG((CE_NOTE, "dp%d: eio_init done, rx poll started\n", sc->dp_unit));
    return 0;
}

/* -----------------------------------------------------------------------
 * eio_reset - hardware reset (called on error recovery)
 * ----------------------------------------------------------------------- */

static void
dp_eio_reset(struct etherif *eif)
{
    struct dp_softc *sc = (struct dp_softc *)eif->eif_private;

    DPLOG((CE_NOTE, "dp%d: eio_reset\n", sc->dp_unit));

    /* Same nesting hazard as eio_init above. */
    if (sc->dp_enabled) {
        DPLOG((CE_NOTE, "dp%d: eio_reset: already up, not bouncing\n", sc->dp_unit));
        return;
    }

    dp_runqueue_stop(sc);
    dp_enable(sc, 0);
    dp_enable(sc, 1);
    dp_set_mode(sc);
    dp_runqueue_start(sc);
}

/* -----------------------------------------------------------------------
 * eio_watchdog - called ~1/sec by network stack via if_timer
 * ----------------------------------------------------------------------- */

static void
dp_eio_watchdog(struct ifnet *ifp)
{
    struct dp_softc *sc = NULL;
    int i;

    /*
     * Do NOT use ifptoeif() here. It is a raw cast that assumes the kernel's
     * ifnet lives at offset 0 of the real struct etherif - an assumption this
     * driver cannot verify, because ether.h is not shipped and sgi_ether.h is
     * a reconstruction. On IRIX 5.3 the cast yields a bogus etherif, so
     * eif_private reads as 0 and this function panics the kernel on the first
     * watchdog tick after ifconfig up:
     *
     *     PANIC: KERNEL FAULT  ... `Software detected SEGV'
     *     Bad addr: 0x0, cause: 0x10000008<CE=1,EXC=RMISS>
     *
     * (16 bytes into dp_eio_watchdog, per nm on the linked kernel.) The
     * DP_CHECK_ETHERIF canary cannot catch this: it guards the bytes AFTER
     * dp_eif, not the offsets of members inside it.
     *
     * Matching ifp against the interfaces we attached needs no layout
     * assumption at all, and an unrecognised ifp is simply ignored rather
     * than dereferenced.
     */
    for (i = 0; i < DP_MAXUNITS; i++) {
        if (dp_units[i] != NULL && eiftoifp(&dp_units[i]->dp_eif) == ifp) {
            sc = dp_units[i];
            break;
        }
    }
    if (sc == NULL)
        return;

    if (sc->dp_enabled)
        ifp->if_timer = IFNET_SLOWHZ;
}

/* -----------------------------------------------------------------------
 * eio_transmit - send one Ethernet frame
 *
 * The mbuf chain contains payload only (no Ethernet header).  We prepend
 * dst+src MAC addresses and ethertype, copy to dp_txbuf, and issue a
 * WRITE(6) SCSI command.  m_freem() is always called (data is copied).
 * ----------------------------------------------------------------------- */

static int
dp_eio_transmit(struct etherif *eif,
                struct etheraddr *dhost,
                struct etheraddr *shost,
                u_short type,
                struct mbuf *m)
{
    struct dp_softc *sc = (struct dp_softc *)eif->eif_private;
    struct ifnet *ifp = eiftoifp(eif);
    struct dp_txframe *tf;
    u_char *p;
    struct mbuf *n;
    int payloadlen, pktlen;

    if (!sc->dp_enabled) {
        m_freem(m);
        return ENETDOWN;
    }

    mutex_lock(&sc->dp_taillock, PZERO);

    if (sc->dp_txq_len >= DP_TX_QLEN) {
        mutex_unlock(&sc->dp_taillock);
        ifp->if_oerrors++;
        m_freem(m);
        return ENOBUFS;
    }

    tf = &sc->dp_txq[sc->dp_txq_tail];
    p  = tf->data;

    /* assemble Ethernet header */
    bcopy(dhost->ea_vec, p, 6);     p += 6;
    bcopy(shost->ea_vec, p, 6);     p += 6;
    *(u_short *)p = type;           p += 2;

    /* flatten mbuf chain */
    payloadlen = 0;
    for (n = m; n; n = n->m_next) {
        int len = n->m_len;
        if (len == 0)
            continue;
        if (p + len > tf->data + DP_TX_BUFSZ) {
            mutex_unlock(&sc->dp_taillock);
            ifp->if_oerrors++;
            m_freem(m);
            return EMSGSIZE;
        }
        bcopy(mtod(n, caddr_t), p, len);
        p += len;
        payloadlen += len;
    }
    m_freem(m);

    pktlen   = (int)sizeof(struct ether_header) + payloadlen;
    tf->len  = pktlen;

    sc->dp_txq_tail = (sc->dp_txq_tail + 1) % DP_TX_QLEN;
    sc->dp_txq_len++;

    mutex_unlock(&sc->dp_taillock);

    {
        char dststr[18];
        sprintf(dststr, "%x:%x:%x:%x:%x:%x",
                (uint)dhost->ea_vec[0], (uint)dhost->ea_vec[1],
                (uint)dhost->ea_vec[2], (uint)dhost->ea_vec[3],
                (uint)dhost->ea_vec[4], (uint)dhost->ea_vec[5]);
        NETLOG((CE_NOTE, "dp%d: tx enqueue pktlen=%d dst=%s\n",
               sc->dp_unit, pktlen, dststr));
    }

    /* kick the queue: trylock so we don't block if runqueue is active */
    if (mutex_trylock(&sc->dp_qlock)) {
        dp_runqueue(sc);
        mutex_unlock(&sc->dp_qlock);
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * eio_ioctl - handle multicast / promiscuous ioctls
 * ----------------------------------------------------------------------- */

static int
dp_eio_ioctl(struct etherif *eif, int cmd, void *data)
{
    struct dp_softc *sc = (struct dp_softc *)eif->eif_private;
    (void)data;

    DPLOG((CE_NOTE, "dp%d: ioctl cmd=0x%x\n", sc->dp_unit, cmd));

    switch (cmd) {
    case SIOCADDMULTI:
        dp_set_mode(sc);
        return 0;

    case SIOCDELMULTI:
        return 0;

    default:
        return EINVAL;
    }
}

/* =======================================================================
 * END SHARED
 * ======================================================================= */

/* -----------------------------------------------------------------------
 * dp_open/close/ioctl - stubs required by lboot.
 *
 * We are a network driver and have no device special file, but the 5.3
 * master file must carry the 'c' (character) and 's' (software) flags -
 * lboot cannot probe for SCSI devices and will otherwise decide the
 * device is absent and drop the driver from the kernel.  lboot builds a
 * cdevsw entry from these, so they must exist.
 * ----------------------------------------------------------------------- */

/* ARGSUSED */
int
dp_open(dev_t *devp, int flag, int otyp, cred_t *crp)
{
    return ENODEV;
}

/* ARGSUSED */
int
dp_close(dev_t dev, int flag, int otyp, cred_t *crp)
{
    return ENODEV;
}

/* ARGSUSED */
int
dp_ioctl(dev_t dev, int cmd, void *arg, int mode, cred_t *crp, int *rvalp)
{
    return ENODEV;
}

/* -----------------------------------------------------------------------
 * dp_do_attach - claim one (adapter, target, lun) and attach it to the
 * Ethernet stack.
 *
 * 5.3 has no ioconfig unit assignment, so units are numbered in bus scan
 * order.  Returns 0 on success, -1 on failure.
 * ----------------------------------------------------------------------- */

static int
dp_do_attach(int adap, int target, int lun, int drvnum)
{
    struct dp_softc  *sc;
    struct etheraddr  ea;
    u_char            mac[6];
    int               unit = dp_nunit;
    int               i;

    if (unit >= DP_MAXUNITS) {
        cmn_err(CE_WARN, "dp: unit %d exceeds max %d\n", unit, DP_MAXUNITS);
        return -1;
    }

    /*
     * 5.3 scsi_alloc returns the adapter TYPE on success and 0 on
     * failure - note this is inverted relative to the 6.5 SLI_ALLOC
     * convention of comparing against SCSIALLOCOK.
     */
    if ((*scsi_alloc[drvnum])((u_char)adap, (u_char)target,
                              (u_char)lun, 1, NULL) == 0) {
        cmn_err(CE_WARN, "dp%d: scsi_alloc failed for %d/%d/%d\n",
                unit, adap, target, lun);
        return -1;
    }

    sc = (struct dp_softc *)kmem_zalloc(sizeof(*sc), KM_SLEEP);
    if (!sc) {
        (*scsi_free[drvnum])((u_char)adap, (u_char)target,
                             (u_char)lun, NULL);
        return -1;
    }

    sc->dp_sense  = (u_char *)kmem_zalloc(DP_SENSE_LEN, VM_CACHEALIGN|VM_DIRECT);
    sc->dp_rxbuf  = (u_char *)kmem_zalloc(DP_RX_BUFSZ,  VM_CACHEALIGN|VM_DIRECT);
    sc->dp_txpool = (u_char *)kmem_zalloc(DP_TX_QLEN * DP_TX_BUFSZ,
                                          VM_CACHEALIGN|VM_DIRECT);

    if (!sc->dp_sense || !sc->dp_rxbuf || !sc->dp_txpool) {
        if (sc->dp_sense)  kmem_free(sc->dp_sense, DP_SENSE_LEN);
        if (sc->dp_rxbuf)  kmem_free(sc->dp_rxbuf, DP_RX_BUFSZ);
        if (sc->dp_txpool) kmem_free(sc->dp_txpool, DP_TX_QLEN * DP_TX_BUFSZ);
        kmem_free(sc, sizeof(*sc));
        (*scsi_free[drvnum])((u_char)adap, (u_char)target,
                             (u_char)lun, NULL);
        return -1;
    }

    for (i = 0; i < DP_TX_QLEN; i++)
        sc->dp_txq[i].data = sc->dp_txpool + i * DP_TX_BUFSZ;

    sc->dp_unit   = unit;
    sc->dp_adap   = adap;
    sc->dp_target = target;
    sc->dp_lun    = lun;
    sc->dp_drvnum = drvnum;

    init_sema(&sc->dp_sema, 0, "dp_sema", unit);
    mutex_init(&sc->dp_qlock,    MUTEX_DEFAULT, "dp_qlock");
    mutex_init(&sc->dp_taillock, MUTEX_DEFAULT, "dp_taillock");
    sc->dp_txq_head = 0;
    sc->dp_txq_tail = 0;
    sc->dp_txq_len  = 0;

    /*
     * Placeholder MAC - the real one is read by RETRIEVE STATISTICS in
     * dp_eio_init(), where we are allowed to sleep.  dp_init() runs at
     * boot before the scheduler is fully up, so no SCSI command may be
     * issued from here.
     */
    mac[0] = 0x00; mac[1] = 0x80; mac[2] = 0x19;
    mac[3] = 0x00; mac[4] = 0x00; mac[5] = (u_char)unit;
    bcopy(mac, ea.ea_vec, 6);

    dp_units[unit] = sc;
    dp_nunit++;

    /* for hinv: (class, type, controller, unit, state) */
    add_to_inventory(INV_NETWORK, INV_NET_ETHER, INV_ETHER_DP, unit, 0);

    DPLOG((CE_NOTE, "dp%d: calling ether_attach\n", unit));

#ifdef DP_CHECK_ETHERIF
    for (i = 0; i < DP_EIF_GUARD_WORDS; i++)
        sc->dp_eif_guard[i] = DP_EIF_GUARD_MAGIC;
#endif

    ether_attach(&sc->dp_eif, "dp", unit, (caddr_t)sc,
                 &dp_ops, &ea, INV_ETHER_DP, 0);

#ifdef DP_CHECK_ETHERIF
    for (i = 0; i < DP_EIF_GUARD_WORDS; i++) {
        if (sc->dp_eif_guard[i] != DP_EIF_GUARD_MAGIC) {
            cmn_err(CE_WARN,
                "dp%d: ***** struct etherif LAYOUT MISMATCH *****\n"
                "dp%d: ether_attach() wrote at least %d word(s) past the end\n"
                "dp%d: of our struct etherif (guard[%d] = 0x%x).\n"
                "dp%d: irix5.3/sgi_ether.h does NOT match this kernel and the\n"
                "dp%d: softc is already corrupt.  DO NOT ifconfig dp%d up.\n"
                "dp%d: Add the missing member(s) to sgi_ether.h and rebuild.\n",
                unit, unit, i + 1, unit, i, sc->dp_eif_guard[i],
                unit, unit, unit, unit);
            break;
        }
    }
#endif

    cmn_err(CE_NOTE, "dp%d: DaynaPort SCSI/Link at scsi(%d) target %d lun %d\n",
            unit, adap, target, lun);

    return 0;
}

/* -----------------------------------------------------------------------
 * dp_probe_one - INQUIRY one (adapter, target, lun) and attach if it is
 * a DaynaPort.  Returns 1 if attached.
 * ----------------------------------------------------------------------- */

static int
dp_probe_one(int adap, int target, int lun)
{
    struct scsi_target_info *tinfo;
    u_char *inq;
    int     drvnum;

    /*
     * scsi_driver_table is indexed by adapter number and yields the host
     * adapter driver number used to index scsi_info[]/scsi_alloc[]/
     * scsi_command[]/scsi_free[].  The table is sparsely populated;
     * SCSIDRIVER_NULL (0) marks a slot with no adapter behind it.
     */
    drvnum = scsi_driver_table[adap];
    if (drvnum == SCSIDRIVER_NULL) {
        DPLOG((CE_NOTE, "dp: probe %d/%d/%d: no adapter (drvnum NULL)\n",
               adap, target, lun));
        return 0;
    }

    /* scsi_info issues an INQUIRY; NULL means nothing is there. */
    tinfo = (*scsi_info[drvnum])((u_char)adap, (u_char)target, (u_char)lun);
    if (tinfo == NULL || tinfo->si_inq == NULL) {
        DPLOG((CE_NOTE, "dp: probe %d/%d/%d: drvnum=%d but no target info\n",
               adap, target, lun, drvnum));
        return 0;
    }

    inq = tinfo->si_inq;

    if ((inq[0] & 0x1F) != DP_SCSI_TYPE)
        return 0;

    {
        char vbuf[9];
        char pbuf[17];
        strncpy(vbuf, (char *)inq + 8,  8); vbuf[8]  = '\0';
        strncpy(pbuf, (char *)inq + 16, 16); pbuf[16] = '\0';
        DPLOG((CE_NOTE, "dp: %d/%d/%d type 3 vendor='%s' product='%s'\n",
               adap, target, lun, vbuf, pbuf));
    }

    if (strncmp((char *)inq + 8,  "Dayna",     5) != 0 ||
        strncmp((char *)inq + 16, "SCSI/Link", 9) != 0)
        return 0;

    DPLOG((CE_NOTE, "dp: found DaynaPort at %d/%d/%d, attaching\n",
           adap, target, lun));

    return (dp_do_attach(adap, target, lun, drvnum) == 0);
}

/* -----------------------------------------------------------------------
 * dp_scan_bus - walk every integral SCSI adapter looking for DaynaPorts.
 *
 * 6.5 gets attach callbacks from CDL and, for the loadable case, walks
 * the hwgraph inventory.  5.3 has neither, so we probe directly.  Only
 * LUN 0 is scanned: every DaynaPort and every emulator that implements
 * the protocol presents on LUN 0.
 * ----------------------------------------------------------------------- */

static void
dp_scan_bus(void)
{
    int adap, target;

    for (adap = SCSI_SGISTART; adap < SCSI_SGISTART + SCSI_SGICOUNT; adap++) {
        for (target = 0; target < 8; target++) {
            if (dp_nunit >= DP_MAXUNITS)
                return;
            (void)dp_probe_one(adap, target, 0);
        }
    }
}

/* -----------------------------------------------------------------------
 * dp_init - called by lboot at boot time because master.d/dp is pulled
 * in with the INCLUDE: directive.  (A VECTOR: driver would need
 * dp_edtinit() instead; we are a software driver with nothing to probe
 * for in physical address space.)
 * ----------------------------------------------------------------------- */

void
dp_init(void)
{
    cmn_err(CE_NOTE, "dp: DaynaPort SCSI/Link driver (IRIX 5.3)\n");
}

/* -----------------------------------------------------------------------
 * dp_start - the bus scan, deferred.
 *
 * It cannot run in dp_init(): master.c's io_init[] calls dp_init 4th, ahead
 * of dsinit/dkscinit and before the host adapter has scanned the bus, so
 * scsi_info() returns NULL for every target and the scan silently finds
 * nothing. (DEPENDENCIES scsi in master.d/dp guarantees the adapter driver is
 * PRESENT, not that it has probed anything.) lboot puts <prefix>start into
 * io_start[], which the kernel calls after every io_init[] and after device
 * configuration - the first point where scsi_info() can answer, and where
 * sleeping is legal.
 * ----------------------------------------------------------------------- */

void
dp_start(void)
{
    if (dp_nunit == 0)
        dp_scan_bus();
}
