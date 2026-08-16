/*
 * if_dp.c - DaynaPort SCSI/Link Ethernet driver for IRIX 5.3
 *
 * This is the IRIX 5.3 (o32, 32-bit) port of the IRIX 6.5 driver in
 * ../irix6.5/if_dp.c.  The DaynaPort protocol, the RX multi-packet parser, the
 * TX ring and the ifnet/etherif handlers are IDENTICAL to the 6.5
 * driver; only device discovery, SCSI submission and the kernel
 * locking primitives differ.
 *
 * The shared DaynaPort protocol code lives in ../shared/dp_proto.c and is
 * #included below (the 6.5 driver #includes the same file), so a protocol
 * fix lands in both at once - there is no duplicated region to keep in sync.
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
 * - no correctness impact - and it lets the shared dp_do_rx() in
 * dp_proto.c compile unchanged.
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

/* Per-target probe chatter. Separate from DP_LOG because the scan walks every
 * adapter and target on the machine - 24 lines on a three-controller box -
 * and only the match matters in normal use. */
#ifdef DP_LOG_PROBE
#define PROBELOG(x) cmn_err x
#else
#define PROBELOG(x)
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
#define DP_STALL_TICKS  200     /* poll ticks before declaring a command lost */

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
#ifdef DP_ASYNC_RX
    /* Asynchronous packet engine (see the DP_ASYNC_RX block below). Separate
     * request from dp_req: control commands and the packet engine can be
     * outstanding at different times, and dp_scsi_cmd() bzero()s whatever it
     * is handed. */
    scsi_request_t      dp_areq;
    u_char              dp_acdb[DP_CDB_LEN];
    struct dp_txframe  *dp_atx;         /* frame in flight, NULL if this is RX */
    volatile int        dp_abusy;       /* a packet command is outstanding    */
    volatile int        dp_fg;          /* a foreground command is in flight  */
    int                 dp_chain;       /* consecutive completion re-submits  */
    int                 dp_stall;       /* ticks a command has been in flight */
#ifdef DP_LOG_TICKRATE
    uint                dp_ntick;       /* poll callbacks since last watchdog */
    uint                dp_nsub;        /* commands submitted, ditto          */
    uint                dp_ndone;       /* completions seen, ditto            */
    uint                dp_nrx;         /* if_ipackets at the last watchdog   */
#endif
#endif
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
#ifdef DP_ASYNC_RX
extern int  dp_rx_parse(struct dp_softc *);     /* in the shared region */
static void dp_async_tick(struct dp_softc *);
static void dp_async_poll(struct dp_softc *);
static void dp_async_submit(struct dp_softc *);
static void dp_async_done(scsi_request_t *);
#endif

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

#ifdef DP_ASYNC_RX
/* =======================================================================
 * Asynchronous packet engine (IRIX 5.3)
 *
 * WHY THIS EXISTS. The portable path polls with dp_runqueue(), which issues
 * SCSI commands through dp_scsi_cmd() and waits in psema(). That is fine on
 * 6.5, whose timeout callbacks run on a thread. On 5.3 an itimeout() callback
 * runs on the interrupt/IDLE stack, where there is no context to switch away
 * from, so the sleep resumes at a null address:
 *
 *     PANIC: exception on IDLE stack k1:0x20 epc:0x0 cause:0x10000008
 *
 * 5.3 has no kernel-thread API to move the poll into (no sthread.h, no
 * kthread.h, nothing in the headers), so instead nothing here ever sleeps:
 * the tick SUBMITS a command and returns, and the completion routine parses
 * the result and submits the next one. ether_input() from a completion is
 * ordinary for a network driver.
 *
 * One command is outstanding at a time (dp_abusy), and foreground control
 * commands stand off against it, because the target answers one initiator
 * command at a time.
 * ======================================================================= */

/* Submit one packet command: a queued transmit if there is one, else a READ.
 * Never sleeps, so it is callable from the timeout callback and from the
 * completion routine. */
static void
dp_async_submit(struct dp_softc *sc)
{
    scsi_request_t *req = &sc->dp_areq;
    u_char *cdb = sc->dp_acdb;
    int len;

    sc->dp_atx = NULL;

    /* Transmit takes priority. trylock because we may be in timeout context;
     * a contended queue just means we do a READ now and the frame goes out on
     * the next tick. */
    if (sc->dp_txq_len > 0 && mutex_trylock(&sc->dp_taillock)) {
        if (sc->dp_txq_len > 0)
            sc->dp_atx = &sc->dp_txq[sc->dp_txq_head];
        mutex_unlock(&sc->dp_taillock);
    }

    bzero(req, sizeof *req);
    if (sc->dp_atx != NULL) {
        len = sc->dp_atx->len;
        cdb[0] = DP_WRITE;
        cdb[1] = 0;
        cdb[2] = 0;
        cdb[3] = (len >> 8) & 0xFF;
        cdb[4] = len & 0xFF;
        cdb[5] = 0x00;
        req->sr_buffer = sc->dp_atx->data;
        req->sr_buflen = (uint)len;
    } else {
        bzero(sc->dp_rxbuf, DP_RX_BUFSZ);
        cdb[0] = DP_READ;
        cdb[1] = 0;
        cdb[2] = 0;
        cdb[3] = (DP_RX_BUFLEN >> 8) & 0xFF;
        cdb[4] = DP_RX_BUFLEN & 0xFF;
        cdb[5] = DP_READ_FLAGS;
        req->sr_buffer = sc->dp_rxbuf;
        req->sr_buflen = (uint)DP_RX_BUFLEN;
        req->sr_flags  = SRF_DIR_IN;
    }

    req->sr_ctlr     = (u_char)sc->dp_adap;
    req->sr_target   = (u_char)sc->dp_target;
    req->sr_lun      = (u_char)sc->dp_lun;
    req->sr_command  = cdb;
    req->sr_cmdlen   = DP_CDB_LEN;
    req->sr_timeout  = 10 * HZ;
    req->sr_sense    = sc->dp_sense;
    req->sr_senselen = DP_SENSE_LEN;
    req->sr_notify   = dp_async_done;
    req->sr_dev      = (void *)sc;
    req->sr_flags   |= SRF_AEN_ACK | SRF_FLUSH;

    sc->dp_abusy = 1;
#ifdef DP_LOG_TICKRATE
    sc->dp_nsub++;
#endif
    (*scsi_command[sc->dp_drvnum])(req);
}

/* Completion. Runs wherever the host adapter calls notify routines - not a
 * sleepable context, so this only parses, hands frames up, and submits the
 * next command. */
static void
dp_async_done(scsi_request_t *req)
{
    struct dp_softc *sc = (struct dp_softc *)req->sr_dev;
    struct ifnet *ifp = eiftoifp(&sc->dp_eif);
    int more = 0;
    int s;

#ifdef DP_LOG_TICKRATE
    sc->dp_ndone++;
#endif
    if (sc->dp_atx != NULL) {
        int pktlen = sc->dp_atx->len;
        if (req->sr_status != 0) {
            SCSILOG((CE_NOTE, "dp%d: tx WRITE failed sr_status=%d\n",
                   sc->dp_unit, (int)req->sr_status));
            ifp->if_oerrors++;
        } else {
            ifp->if_opackets++;
            ifp->if_obytes += pktlen - (int)sizeof(struct ether_header);
        }
        if (mutex_trylock(&sc->dp_taillock)) {
            sc->dp_txq_head = (sc->dp_txq_head + 1) % DP_TX_QLEN;
            sc->dp_txq_len--;
            more = (sc->dp_txq_len > 0);
            mutex_unlock(&sc->dp_taillock);
        }
        sc->dp_atx = NULL;
    } else if (req->sr_status == 0) {
        more = dp_rx_parse(sc);
        if (more < 0) {
            /* Recovery sleeps, so it cannot run here. Let the interface go
             * quiet; the next foreground ifconfig will re-enable it. */
            more = 0;
        }
    }

    /* Chain straight into the next command while the device says there is
     * more, but bound it: a completion routine calling submit calling a
     * completion is fine once, less so a thousand times deep. dp_abusy
     * stays SET across the chain: clearing it first (as this used to)
     * opens a window where the transmit path can claim the engine and
     * double-submit the shared request. */
    if (more && !sc->dp_fg && sc->dp_enabled && ++sc->dp_chain < 8) {
        dp_async_submit(sc);
        return;
    }
    sc->dp_chain = 0;
    sc->dp_stall = 0;
    /* Release the engine and re-arm the poll in ONE atomic step.
     *
     * This runs at completion (interrupt) priority, but dp_async_poll()
     * performs the same dp_timer test-and-set from timeout context, which
     * a completion CAN interrupt between its test and its store. Two arms
     * both reading dp_timer == 0 each start a poll chain; every callout
     * re-arms itself, so the orphaned chain never dies, each extra chain
     * multiplies the odds of the next race, and the callout table fills:
     *     PANIC: Timeout table overflow.
     * Reproduced under IRIS with zero-latency completions (three pings).
     * splhi() blocks the interrupt across the peer's window, making the
     * two test-and-sets atomic with respect to each other.
     *
     * Re-arming here as well as in dp_async_poll() stays deliberate:
     * relying on either site alone has failed on hardware once in each
     * direction. The guard makes the redundancy safe. */
    s = splhi();
    sc->dp_abusy = 0;
    if (sc->dp_enabled && sc->dp_timer == 0)
        sc->dp_timer = itimeout((void (*)())dp_timer_kick, (void *)sc,
                                HZ / 100, plbase);
    splx(s);
}

/* Submit if the engine is idle and no foreground command holds the device.
 * Never arms anything: callers that need the poll to continue go through
 * dp_async_poll() below. Safe from any context.
 *
 * The idle test and the claim of the engine must be one atomic step. This
 * is reached from user context (every transmit enqueue) and from timeout
 * context (dp_async_poll), and one can preempt the other between the test
 * and dp_async_submit()'s dp_abusy = 1 - a window dozens of instructions
 * wide, since submit bzero()s and rebuilds the shared request first. Two
 * entrants both passing the test double-submit the single dp_areq, and the
 * second bzero lands on a request the host adapter is actively
 * transferring - the data phase stops dead mid-transfer (seen from the
 * BlueSCSI side as a 5 s "finishRead timeout" followed by the firmware
 * abandoning the command). splhi() closes the window. */
static void
dp_async_tick(struct dp_softc *sc)
{
    int s;

    if (!sc->dp_enabled)
        return;
    s = splhi();
    if (sc->dp_abusy || sc->dp_fg) {
        splx(s);
        return;
    }
    sc->dp_abusy = 1;   /* claim the engine before dropping splhi */
    splx(s);
    sc->dp_chain = 0;
    dp_async_submit(sc);
}

/* The poll proper, called only from dp_timer_kick() i.e. timeout context.
 *
 * The re-arm happens HERE, before any submission, and deliberately not in the
 * completion routine. Timeout context is the only context in which this
 * driver has ever demonstrably armed a timer - the old synchronous poll
 * re-armed from dp_runqueue(), reached from this same callback. Re-arming
 * from a SCSI completion is a different context, and on real hardware the
 * poll stalls: an idle Indigo answers pings only when something else happens
 * to kick the engine, giving replies batched seconds apart (2106/1103/98 ms
 * repeating) with no packet loss at all. Arming from here keeps the chain
 * alive regardless of what completions do.
 */
static void
dp_async_poll(struct dp_softc *sc)
{
    int s;
    int lost = 0;

    if (!sc->dp_enabled)
        return;

    /* The test-and-set on dp_timer must be atomic against the identical
     * one at the end of dp_async_done(): a completion interrupt landing
     * between this test and this store arms a second self-rearming chain
     * and the orphans multiply into "PANIC: Timeout table overflow" - see
     * the comment in dp_async_done(). The stall bookkeeping shares the
     * region because its recovery writes dp_abusy, which a concurrent
     * completion also writes; recovering at the same instant a late
     * completion chains a new command would mark a live engine idle. */
    s = splhi();
    if (sc->dp_timer == 0)
        sc->dp_timer = itimeout((void (*)())dp_timer_kick, (void *)sc,
                                HZ / 100, plbase);

    /*
     * Recover from a command whose completion never arrives. One request is
     * outstanding at a time, so a lost completion leaves dp_abusy set and
     * stops EVERYTHING - transmit as well as receive, since dp_async_tick()
     * refuses to submit while busy. That is not hypothetical: it is what a
     * real DaynaPort did where the emulated one never does, and the interface
     * went completely silent in both directions.
     *
     * sr_timeout on the request is 10*HZ, so anything still outstanding after
     * DP_STALL_TICKS (2s at HZ/100) is not coming back. Give up on it and
     * carry on; a duplicate completion later is harmless, it only clears a
     * flag we have already cleared.
     */
    if (sc->dp_abusy) {
        if (++sc->dp_stall > DP_STALL_TICKS) {
            sc->dp_stall = 0;
            sc->dp_atx   = NULL;
            sc->dp_abusy = 0;
            lost = 1;
        }
    } else {
        sc->dp_stall = 0;
    }
    splx(s);

    if (lost)
        cmn_err(CE_WARN, "dp%d: SCSI command lost (no completion in %ds)"
                " - recovering\n", sc->dp_unit, DP_STALL_TICKS / (HZ / 100) );

    dp_async_tick(sc);
}

/* IRIX 5.3's if_slowtimo calls if_watchdog in the old BSD style -
 * (*if_watchdog)(unit) - not with the ifp the etherifops signature
 * expects. ether_attach() wires the etherifops watchdog straight into the
 * ifnet, so dp_eio_watchdog() was being reached with unit 0 cast to a
 * pointer: its ifp-matching loop could never match, it could never re-arm
 * if_timer, and so it ran exactly once per ifconfig and went silent.
 * (The ifptoeif() cast it replaced panicked on the same near-zero value -
 * "Bad addr: 0x0" - which this finally explains.) Measured under IRIS:
 * the watchdog argument arrives as 0x0 while the attached ifp is a real
 * kernel pointer.
 *
 * So on 5.3 dp_do_attach() installs this unit-style watchdog over the one
 * ether_attach() wired. It only re-arms the timer (and carries the
 * DP_LOG_TICKRATE probe); it deliberately does NOT kick the packet
 * engine - a watchdog that collects packets masks a dead poll chain.
 * The shared dp_eio_watchdog() remains for 6.5, whose ifnet layer really
 * does pass the ifp. */
static void
dp_wdog53(int unit)
{
    struct dp_softc *sc;
    struct ifnet *ifp;

    if (unit < 0 || unit >= DP_MAXUNITS)
        return;
    sc = dp_units[unit];
    if (sc == NULL || !sc->dp_enabled)
        return;
    ifp = eiftoifp(&sc->dp_eif);
    ifp->if_timer = IFNET_SLOWHZ;
#ifdef DP_LOG_TICKRATE
    cmn_err(CE_NOTE, "dp%d: 1s tick=%u sub=%u done=%u rx=%u"
            " busy=%d fg=%d stall=%d\n", sc->dp_unit,
            sc->dp_ntick, sc->dp_nsub, sc->dp_ndone,
            (uint)ifp->if_ipackets - sc->dp_nrx,
            sc->dp_abusy, sc->dp_fg, sc->dp_stall);
    sc->dp_ntick = sc->dp_nsub = sc->dp_ndone = 0;
    sc->dp_nrx   = (uint)ifp->if_ipackets;
#endif
}
#endif /* DP_ASYNC_RX */

/* DaynaPort protocol core - shared verbatim with the 6.5 driver.
 * See shared/dp_proto.c. */
#include "dp_proto.c"

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

#ifdef DP_ASYNC_RX
    /* 5.3's if_slowtimo calls if_watchdog as (*wd)(unit), not (*wd)(ifp),
     * so the etherifops watchdog ether_attach() just wired can neither
     * find its softc nor re-arm if_timer. Install the unit-style one -
     * see dp_wdog53() for the whole story. */
    eiftoifp(&sc->dp_eif)->if_watchdog = (void (*)())dp_wdog53;
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
    /* An empty adapter slot or an empty target is the overwhelmingly common
     * case - a machine with three controllers walks 24 of them at every boot.
     * Say nothing for those unless DP_LOG_PROBE is asked for explicitly. */
    drvnum = scsi_driver_table[adap];
    if (drvnum == SCSIDRIVER_NULL) {
        PROBELOG((CE_NOTE, "dp: probe %d/%d/%d: no adapter (drvnum NULL)\n",
               adap, target, lun));
        return 0;
    }

    /* scsi_info issues an INQUIRY; NULL means nothing is there. */
    tinfo = (*scsi_info[drvnum])((u_char)adap, (u_char)target, (u_char)lun);
    if (tinfo == NULL || tinfo->si_inq == NULL) {
        PROBELOG((CE_NOTE, "dp: probe %d/%d/%d: drvnum=%d but no target info\n",
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
