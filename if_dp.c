//#define DP_LOG
//#define DP_LOG_NET
/*
 * if_dp.c - DaynaPort SCSI/Link Ethernet driver for IRIX 6.5
 *
 * The DaynaPort SCSI/Link (DP0801/DP0802) and compatible emulators
 * (BlueSCSI V2, ZuluSCSI, PiSCSI, SCSI2SD) present as SCSI type 3
 * (Processor) devices and move Ethernet frames with vendor CDBs:
 *
 *   0x08  READ(6)            - receive packet(s)
 *   0x09  RETRIEVE STATS     - get MAC address
 *   0x0A  WRITE(6)           - transmit packet
 *   0x0C  SET INTERFACE MODE - enable broadcast reception
 *   0x0E  ENABLE/DISABLE     - enable or disable the interface
 *
 * All SCSI commands are issued synchronously; a mutex serialises the
 * single-command-at-a-time device.  RX is polled via a self-rescheduling
 * 10ms itimeout() callback.  TX is inline in eio_transmit.
 *
 * Architecture-independent: no DMA, no PCI, no cache flush required.
 * Targets IRIX 6.5 on all platforms (IP22, IP30, IP32, IP35, ...).
 *
 * Protocol reference: SLINKCMD.TXT (Roger Burrows, rev 1.20)
 *
 * Build with -DDP_LOG to enable verbose kernel logging.
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
#include "sys/ddi.h"
#include "sys/mload.h"
#include "sys/hwgraph.h"
#include "sys/iograph.h"
#include "sys/scsi.h"
#include "net/if.h"
#include "net/raw.h"
#include "net/soioctl.h"
#include "sgi_ether.h"
#include "netinet/in.h"
#include "netinet/if_ether.h"
#include "string.h"

/* -----------------------------------------------------------------------
 * Module identity (required for loadable modules)
 * ----------------------------------------------------------------------- */

char *dp_mversion = M_VERSION;
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
 * DaynaPort SCSI command opcodes
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

/* -----------------------------------------------------------------------
 * Per-interface soft state
 * ----------------------------------------------------------------------- */

struct dp_txframe {
    u_char *data;   /* points into dp_txpool; VM_CACHEALIGN|VM_DIRECT */
    int     len;
};

struct dp_softc {
    struct etherif      dp_eif;             /* MUST be first - ifptoeif() */
    vertex_hdl_t        dp_lun_vhdl;        /* SCSI LUN vertex handle     */
    scsi_lun_info_t    *dp_lun_info;        /* cached LUN info pointer    */
    sema_t              dp_sema;            /* SCSI command completion     */
    mutex_t             dp_qlock;           /* serialises dp_runqueue      */
    mutex_t             dp_taillock;        /* protects tx queue tail      */
    toid_t              dp_timer;           /* RX poll timer id           */
    volatile int        dp_enabled;         /* 1 after eio_init           */
    int                 dp_unit;
    scsi_request_t      dp_req;             /* embedded — no separate alloc */
    u_char              dp_cdb[DP_CDB_LEN]; /* CDB — must outlive SLI_COMMAND */
    u_char             *dp_sense;           /* VM_CACHEALIGN|VM_DIRECT    */
    u_char             *dp_rxbuf;           /* VM_CACHEALIGN|VM_DIRECT    */
    u_char             *dp_txpool;          /* VM_CACHEALIGN|VM_DIRECT, DP_TX_QLEN*DP_TX_BUFSZ */
    struct dp_txframe   dp_txq[DP_TX_QLEN]; /* TX frame queue             */
    int                 dp_txq_head;        /* dequeue index (qlock)      */
    int                 dp_txq_tail;        /* enqueue index (taillock)   */
    int                 dp_txq_len;         /* current depth (both locks) */
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
int         dp_attach(vertex_hdl_t);
int         dp_detach(vertex_hdl_t);
static int  dp_do_attach(vertex_hdl_t, scsi_lun_info_t *, scsi_unit_info_t *);

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
    req->sr_lun_vhdl = sc->dp_lun_vhdl;
    req->sr_ctlr     = SLI_ADAP(sc->dp_lun_info);
    req->sr_target   = SLI_TARG(sc->dp_lun_info);
    req->sr_lun      = SLI_LUN(sc->dp_lun_info);
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

    SLI_COMMAND(sc->dp_lun_info)(req);
    psema(&sc->dp_sema, PRIBIO);

    status = (int)req->sr_status;

    if (cdb[0] != DP_READ)
        SCSILOG((CE_NOTE, "dp%d: scsi_cmd op=0x%02x -> status=%d scsi_status=%d resid=%d\n",
               sc->dp_unit, cdb[0], status, (int)req->sr_scsi_status, (int)req->sr_resid));

    return status;
}

/* dp_scsi_cmd_locked - acquire dp_qlock, run command, release.
 * Use for control commands (enable, set_mode, get_mac) called outside
 * of dp_runqueue context. */
static int
dp_scsi_cmd_locked(struct dp_softc *sc, u_char *cdb, int cdblen,
                   void *buf, int buflen, ushort dir)
{
    int status;
    mutex_lock(&sc->dp_qlock, PZERO);
    status = dp_scsi_cmd(sc, cdb, cdblen, buf, buflen, dir);
    mutex_unlock(&sc->dp_qlock);
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

#ifndef DP_NO_POLL_TIMER
    sc->dp_timer = itimeout((void (*)())dp_timer_kick, (void *)sc,
                            HZ / 100, plbase);
#endif
}

/* -----------------------------------------------------------------------
 * dp_timer_kick - timer callback: acquire dp_qlock and run queue
 * ----------------------------------------------------------------------- */

static void
dp_timer_kick(struct dp_softc *sc)
{
#ifdef DP_NO_POLL_TIMER
    /* Diagnostic build: no periodic poll at all. RX then only happens when
     * dp_eio_transmit() kicks the queue, which runs in a context where
     * sleeping is legal - unlike this callback. Slow (RX lags one transmit)
     * but it isolates "the packet path is broken" from "the packet path is
     * fine, we just cannot poll it from timeout context". */
    return;
#endif
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
     * this tick entirely when the lock is held and come back in 10ms; there is
     * nothing time-critical about a poll. dp_eio_transmit() already uses
     * mutex_trylock() for the same reason.
     */
    if (!mutex_trylock(&sc->dp_qlock)) {
        sc->dp_timer = itimeout((void (*)())dp_timer_kick, (void *)sc,
                                HZ / 100, plbase);
        return;
    }
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

#ifdef DP_MODULE
/* -----------------------------------------------------------------------
 * dp_open/close/ioctl - stubs required by ml(1) for loadable modules.
 * We are not a character device; these will never be called at runtime.
 * ----------------------------------------------------------------------- */

/* ARGSUSED */
int
dp_open(dev_t *devp, int flag, int otyp, struct cred *crp)
{
    return ENODEV;
}

/* ARGSUSED */
int
dp_close(dev_t dev, int flag, int otyp, struct cred *crp)
{
    return ENODEV;
}

/* ARGSUSED */
int
dp_ioctl(dev_t dev, int cmd, void *arg, int mode, struct cred *crp, int *rvalp)
{
    return ENODEV;
}
#endif /* DP_MODULE */

/* dp_scan_invent - walk kernel inventory for INV_SCSI/INV_CPU (type 3)
 * devices and call dp_do_attach directly on each matching lun vertex.
 * Used by the loadable module path since CDL has already run by the time
 * the module is loaded and dp_attach will not be called automatically. */
static void
dp_scan_invent(void)
{
    invplace_t          iplace;
    inventory_t        *inv;
    vertex_hdl_t        vhdl;
    vertex_hdl_t        lun_vhdl;
    scsi_lun_info_t    *lun_info;
    scsi_target_info_t *tinfo;
    u_char             *inq;
    char                namebuf[256];

    iplace = INVPLACE_NONE;
    while ((inv = get_next_inventory(&iplace)) != NULL) {
        if (inv->inv_class != INV_SCSI || inv->inv_type != INV_CPU)
            continue;

        /* invplace_vhdl is the "scsi" child of lun vertex; go up to lun */
        vhdl = iplace.invplace_vhdl;

        DPLOG((CE_NOTE, "dp: invent INV_CPU ctlr=%d unit=%d scsi_vhdl=%d\n",
               inv->inv_controller, inv->inv_unit, (int)vhdl));

        if (vhdl == GRAPH_VERTEX_NONE) {
            DPLOG((CE_NOTE, "dp: invplace_vhdl is NONE, skip\n"));
            continue;
        }

        DPLOG((CE_NOTE, "dp: scsi_vhdl path=%s\n",
               vertex_to_name(vhdl, namebuf, sizeof(namebuf))));

        if (hwgraph_traverse(vhdl, "..", &lun_vhdl) != GRAPH_SUCCESS) {
            DPLOG((CE_NOTE, "dp: can't traverse to lun vertex\n"));
            continue;
        }

        DPLOG((CE_NOTE, "dp: lun_vhdl=%d path=%s\n", (int)lun_vhdl,
               vertex_to_name(lun_vhdl, namebuf, sizeof(namebuf))));

        lun_info = scsi_lun_info_get(lun_vhdl);
        if (lun_info == NULL) {
            DPLOG((CE_NOTE, "dp: no lun_info\n"));
            hwgraph_vertex_unref(lun_vhdl);
            continue;
        }
        DPLOG((CE_NOTE, "dp: lun_info=%p sli_targ_info=%p\n",
               lun_info, lun_info->sli_targ_info));

        if (lun_info->sli_targ_info == NULL) {
            DPLOG((CE_NOTE, "dp: null sli_targ_info\n"));
            hwgraph_vertex_unref(lun_vhdl);
            continue;
        }
        if (lun_info->sli_targ_info->sti_ctlr_info == NULL) {
            DPLOG((CE_NOTE, "dp: null sti_ctlr_info\n"));
            hwgraph_vertex_unref(lun_vhdl);
            continue;
        }
        if (lun_info->sli_targ_info->sti_ctlr_info->sci_inq == NULL) {
            DPLOG((CE_NOTE, "dp: null sci_inq fn\n"));
            hwgraph_vertex_unref(lun_vhdl);
            continue;
        }

        tinfo = SLI_INQ(lun_info)(lun_vhdl);
        if (tinfo == NULL || tinfo->si_inq == NULL) {
            DPLOG((CE_NOTE, "dp: no inq data\n"));
            hwgraph_vertex_unref(lun_vhdl);
            continue;
        }

        inq = tinfo->si_inq;

        {
            char vbuf[9];
            char pbuf[17];
            strncpy(vbuf, (char *)inq + 8,  8); vbuf[8]  = '\0';
            strncpy(pbuf, (char *)inq + 16, 16); pbuf[16] = '\0';
            DPLOG((CE_NOTE, "dp: inq vendor='%s' product='%s'\n", vbuf, pbuf));
        }

        if (strncmp((char *)inq + 8,  "Dayna",     5) != 0 ||
            strncmp((char *)inq + 16, "SCSI/Link", 9) != 0) {
            hwgraph_vertex_unref(lun_vhdl);
            continue;
        }

        DPLOG((CE_NOTE, "dp: found DaynaPort, attaching\n"));
        dp_do_attach(lun_vhdl, lun_info, NULL);
        hwgraph_vertex_unref(lun_vhdl);
    }
}

/* -----------------------------------------------------------------------
 * dp_reg - called after initialize_io() has scanned the SCSI bus.
 * CDL won't call dp_attach automatically for type-3 devices since there
 * is no entry in master.d/scsi for type 3, so we walk inventory directly.
 * ----------------------------------------------------------------------- */
int
dp_reg(void)
{
    int err = scsi_driver_register(3, "dp_");
    if (err)
        cmn_err(CE_WARN, "dp: scsi_driver_register failed: %d\n", err);
    else
        cmn_err(CE_NOTE, "dp: registered for SCSI type 3 (Processor)\n");
#ifdef DP_BUILTIN
    dp_scan_invent();
#endif
    return err;
}

/* -----------------------------------------------------------------------
 * dp_init - called by lboot/ml framework at load time.
 * ----------------------------------------------------------------------- */
void
dp_init(void)
{
    cmn_err(CE_NOTE, "dp: DaynaPort SCSI/Link driver\n");
#ifdef DP_MODULE
    dp_scan_invent();
#endif
}

/* -----------------------------------------------------------------------
 * dp_do_detach - tear down one unit and null its slot in dp_units[]
 * ----------------------------------------------------------------------- */
static void
dp_do_detach(int unit)
{
    struct dp_softc *sc = dp_units[unit];
    if (!sc)
        return;

    dp_units[unit] = NULL;      /* null first so unload won't double-detach */

    DPLOG((CE_NOTE, "dp%d: detach\n", sc->dp_unit));

    dp_runqueue_stop(sc);
    dp_enable(sc, 0);

    kmem_free(sc->dp_sense,  DP_SENSE_LEN);
    kmem_free(sc->dp_rxbuf,  DP_RX_BUFSZ);
    kmem_free(sc->dp_txpool, DP_TX_QLEN * DP_TX_BUFSZ);

    mutex_destroy(&sc->dp_qlock);
    mutex_destroy(&sc->dp_taillock);
    freesema(&sc->dp_sema);

    SLI_FREE(sc->dp_lun_info)(sc->dp_lun_vhdl, NULL);

    kmem_free(sc, sizeof(*sc));
}

#ifdef DP_MODULE
/* -----------------------------------------------------------------------
 * dp_unload - called by ml framework at module unload time.
 * ----------------------------------------------------------------------- */
void
dp_unload(void)
{
    int i;
    DPLOG((CE_NOTE, "dp: dp_unload\n"));
    for (i = 0; i < dp_nunit; i++)
        dp_do_detach(i);
    dp_nunit = 0;
}
#endif /* DP_MODULE */

/* -----------------------------------------------------------------------
 * dp_attach - CDL calls this for each discovered type-3 SCSI LUN
 * We verify it is a DaynaPort by checking the INQUIRY vendor/product strings,
 * read the MAC, enable the interface, and attach to the Ethernet stack.
 * ----------------------------------------------------------------------- */

/* dp_do_attach - real attach work given a lun_vhdl+lun_info.
 * Called from dp_attach (CDL path) and dp_scan_invent (direct path). */
static int
dp_do_attach(vertex_hdl_t lun_vhdl, scsi_lun_info_t *lun_info,
             scsi_unit_info_t *unit_info)
{
    struct dp_softc  *sc;
    struct etheraddr  ea;
    u_char            mac[6];
    int               unit;

    /* register inventory first so ioconfig can assign a unit number */
    device_inventory_add(lun_vhdl, INV_NETWORK, INV_NET_ETHER,
                         INV_ETHER_DP, -1, 0);
    unit = device_controller_num_get(lun_vhdl);
    if (unit < 0)
        unit = dp_nunit;    /* ioconfig not run yet, use sequential */

    DPLOG((CE_NOTE, "dp: unit=%d from ioconfig\n", unit));

    if (unit >= DP_MAXUNITS) {
        cmn_err(CE_WARN, "dp: unit %d exceeds max %d\n", unit, DP_MAXUNITS);
        return -1;
    }

    DPLOG((CE_NOTE, "dp%d: calling SLI_ALLOC\n", unit));

    if (SLI_ALLOC(lun_info)(lun_vhdl, 1, NULL) != SCSIALLOCOK) {
        cmn_err(CE_WARN, "dp%d: scsi_alloc failed\n", unit);
        return -1;
    }

    sc = (struct dp_softc *)kmem_zalloc(sizeof(*sc), KM_SLEEP);
    if (!sc) {
        SLI_FREE(lun_info)(lun_vhdl, NULL);
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
        SLI_FREE(lun_info)(lun_vhdl, NULL);
        return -1;
    }

    {
        int i;
        for (i = 0; i < DP_TX_QLEN; i++)
            sc->dp_txq[i].data = sc->dp_txpool + i * DP_TX_BUFSZ;
    }

    sc->dp_unit     = unit;
    sc->dp_lun_vhdl = lun_vhdl;
    sc->dp_lun_info = lun_info;
    init_sema(&sc->dp_sema, 0, "dp_sema", unit);
    mutex_init(&sc->dp_qlock,    MUTEX_DEFAULT, "dp_qlock");
    mutex_init(&sc->dp_taillock, MUTEX_DEFAULT, "dp_taillock");
    sc->dp_txq_head = 0;
    sc->dp_txq_tail = 0;
    sc->dp_txq_len  = 0;

    DPLOG((CE_NOTE, "dp%d: softc at 0x%x\n", unit, (uint)sc));

    /* placeholder MAC — real MAC read in dp_eio_init when ifconfig brings
     * the interface up. dp_do_attach runs under majorsem so we cannot
     * sleep inside mutex_lock here. */
    mac[0] = 0x00; mac[1] = 0x80; mac[2] = 0x19;
    mac[3] = 0x00; mac[4] = 0x00; mac[5] = (u_char)unit;
    bcopy(mac, ea.ea_vec, 6);

    if (unit_info)
        SUI_CTINFO(unit_info) = (void *)sc;

    dp_units[unit] = sc;
    dp_nunit++;

    {
        char *hinvstr = (char *)kmem_alloc(64, KM_SLEEP);
        if (hinvstr) {
            sprintf(hinvstr, "DaynaPort SCSI/Link Ethernet: dp%d", unit);
            hwgraph_info_add_LBL(lun_vhdl, "_hinv_string",
                                 (arbitrary_info_t)hinvstr);
        }
    }

    DPLOG((CE_NOTE, "dp%d: calling ether_attach\n", unit));

    ether_attach(&sc->dp_eif, "dp", unit, (caddr_t)sc,
                 &dp_ops, &ea, INV_ETHER_DP, 0);

    cmn_err(CE_NOTE, "dp%d: DaynaPort SCSI/Link at SCSI id %d\n",
            unit, (int)SLI_TARG(lun_info));

    DPLOG((CE_NOTE, "dp%d: dp_do_attach complete\n", unit));
    return 0;
}

int
dp_attach(vertex_hdl_t conn_vhdl)
{
    scsi_unit_info_t   *unit_info;
    scsi_lun_info_t    *lun_info;
    u_char             *inq;
    char                vbuf[9];
    char                pbuf[17];

    DPLOG((CE_NOTE, "dp: dp_attach called\n"));

    unit_info = scsi_unit_info_get(conn_vhdl);
    if (!unit_info) {
        DPLOG((CE_NOTE, "dp: dp_attach: no unit_info\n"));
        return -1;
    }

    inq = SUI_INV(unit_info);
    if (!inq) {
        DPLOG((CE_NOTE, "dp: dp_attach: no inq data\n"));
        return -1;
    }

    strncpy(vbuf, (char *)inq + 8,  8); vbuf[8]  = '\0';
    strncpy(pbuf, (char *)inq + 16, 16); pbuf[16] = '\0';
    DPLOG((CE_NOTE, "dp: dp_attach: type=0x%02x vendor='%s' product='%s'\n",
           inq[0], vbuf, pbuf));

    if (strncmp((char *)inq + 8,  "Dayna",     5) != 0 ||
        strncmp((char *)inq + 16, "SCSI/Link", 9) != 0) {
        DPLOG((CE_NOTE, "dp: dp_attach: not a DaynaPort, skipping\n"));
        return -1;
    }

    lun_info = SUI_LUN_INFO(unit_info);
    if (!lun_info) {
        DPLOG((CE_NOTE, "dp: dp_attach: no lun_info\n"));
        return -1;
    }

    return dp_do_attach(SLI_LUN_VHDL(lun_info), lun_info, unit_info);
}

int
dp_detach(vertex_hdl_t conn_vhdl)
{
    int i;

    DPLOG((CE_NOTE, "dp: dp_detach called\n"));

    for (i = 0; i < dp_nunit; i++) {
        if (dp_units[i] && dp_units[i]->dp_lun_vhdl == conn_vhdl) {
            dp_do_detach(i);
            return 0;
        }
    }

    DPLOG((CE_NOTE, "dp: dp_detach: vhdl not found\n"));
    return -1;
}
