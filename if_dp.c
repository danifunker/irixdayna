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
#include "misc/ether.h"
#include "netinet/in.h"
#include "netinet/if_ether.h"
#include "string.h"

/* -----------------------------------------------------------------------
 * Module identity (required for loadable modules)
 * ----------------------------------------------------------------------- */

char *dp_mversion = M_VERSION;
int   dp_devflag  = D_MP;

/* -----------------------------------------------------------------------
 * DaynaPort SCSI command opcodes
 * ----------------------------------------------------------------------- */

#define DP_READ         0x08    /* READ(6)  - receive packet(s)        */
#define DP_GET_STATS    0x09    /* RETRIEVE STATISTICS - get MAC + ctr */
#define DP_WRITE        0x0A    /* WRITE(6) - transmit packet          */
#define DP_SET_MODE     0x0C    /* SET INTERFACE MODE                  */
#define DP_ENABLE       0x0E    /* ENABLE/DISABLE interface            */

#define DP_STATS_LEN    18      /* GET_STATS returns 18 bytes          */
#define DP_RX_BUFLEN    4096    /* READ allocation length              */
#define DP_RX_BUFSZ     (DP_RX_BUFLEN + 4) /* +4 for CRC slop         */
#define DP_TX_BUFSZ     1540   /* one max Ethernet frame              */
#define DP_RX_HDR       6      /* length+flags prefix on each RX record*/
#define DP_CRC_LEN      4      /* trailing CRC bytes to strip         */
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

/* -----------------------------------------------------------------------
 * Per-interface soft state
 * ----------------------------------------------------------------------- */

struct dp_softc {
    struct etherif      dp_eif;             /* MUST be first - ifptoeif() */
    vertex_hdl_t        dp_lun_vhdl;        /* SCSI LUN vertex handle     */
    scsi_lun_info_t    *dp_lun_info;        /* cached LUN info pointer    */
    mutex_t             dp_lock;            /* serialises all SCSI cmds   */
    toid_t              dp_timer;           /* RX poll timer id           */
    int                 dp_enabled;         /* 1 after eio_init           */
    int                 dp_unit;
    u_char              dp_rxbuf[DP_RX_BUFSZ]; /* raw SCSI RX buffer     */
    u_char              dp_txbuf[DP_TX_BUFSZ]; /* TX frame assembly area  */
};

static struct dp_softc *dp_units[DP_MAXUNITS];
static int dp_nunit = 0;

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */

static int  dp_eio_init(struct etherif *, int);
static void dp_eio_reset(struct etherif *);
static void dp_eio_watchdog(struct ifnet *);
static int  dp_eio_transmit(struct etherif *, struct etheraddr *,
                            struct etheraddr *, u_short, struct mbuf *);
static int  dp_eio_ioctl(struct etherif *, int, void *);

static struct etherifops dp_ops = {
    dp_eio_init,
    dp_eio_reset,
    dp_eio_watchdog,
    dp_eio_transmit,
    dp_eio_ioctl,
};

static void dp_rx_poll(struct dp_softc *);

/* -----------------------------------------------------------------------
 * dp_scsi_cmd - issue a synchronous SCSI command
 *
 * Acquires dp_lock for the duration.  dir=SRF_DIR_IN for reads,
 * dir=0 for writes/no-data.  Returns sr_status (0 = SC_GOOD).
 * ----------------------------------------------------------------------- */

static int
dp_scsi_cmd(struct dp_softc *sc, u_char *cdb, int cdblen,
            void *buf, int buflen, ushort dir)
{
    scsi_request_t req;
    u_char sense[32];

    bzero(&req, sizeof req);
    req.sr_lun_vhdl    = sc->dp_lun_vhdl;
    req.sr_command     = cdb;
    req.sr_cmdlen      = (ushort)cdblen;
    req.sr_flags       = dir;
    req.sr_timeout     = 10 * HZ;
    req.sr_buffer      = (u_char *)buf;
    req.sr_buflen      = (uint)buflen;
    req.sr_sense       = sense;
    req.sr_senselen    = sizeof sense;
    req.sr_notify      = NULL;  /* synchronous */

    mutex_lock(&sc->dp_lock, PZERO);
    SLI_COMMAND(sc->dp_lun_info)(&req);
    mutex_unlock(&sc->dp_lock);

    return (int)req.sr_status;
}

/* -----------------------------------------------------------------------
 * dp_enable - send ENABLE or DISABLE CDB
 * ----------------------------------------------------------------------- */

static void
dp_enable(struct dp_softc *sc, int on)
{
    u_char cdb[6] = { DP_ENABLE, 0, 0, 0, 0, on ? 0x80 : 0x00 };
    dp_scsi_cmd(sc, cdb, 6, NULL, 0, 0);
    delay(DP_ENABLE_POST_DELAY);
}

/* -----------------------------------------------------------------------
 * dp_set_mode - SET INTERFACE MODE to enable broadcast reception
 * ----------------------------------------------------------------------- */

static void
dp_set_mode(struct dp_softc *sc)
{
    u_char cdb[6] = { DP_SET_MODE, 0, 0, 0, DP_MODE_BCAST, 0x80 };
    dp_scsi_cmd(sc, cdb, 6, NULL, 0, 0);
}

/* -----------------------------------------------------------------------
 * dp_get_mac - RETRIEVE STATISTICS to read MAC address
 * Returns 1 on success, 0 on failure.
 * ----------------------------------------------------------------------- */

static int
dp_get_mac(struct dp_softc *sc, u_char *mac)
{
    u_char cdb[6] = { DP_GET_STATS, 0, 0, 0, DP_STATS_LEN, 0 };
    u_char buf[DP_STATS_LEN];
    int err;

    err = dp_scsi_cmd(sc, cdb, 6, buf, DP_STATS_LEN, SRF_DIR_IN);
    if (err == 0)
        bcopy(buf, mac, 6);
    return (err == 0);
}

/* -----------------------------------------------------------------------
 * dp_rx_once - issue one READ and dispatch any received packets
 *
 * Returns non-zero if more packets are pending (caller may loop).
 * Returns 0 if no packet, error, or device indicates "last packet".
 *
 * RX record format (SLINKCMD.TXT):
 *   [0,1]   pktlen  big-endian, includes 4-byte CRC, excludes itself+flags
 *   [2..5]  flags   0x00000010 = more pending, 0xFFFFFFFF = dropped
 *   [6..]   full Ethernet frame (dst+src+type+payload)
 *   [last4] CRC (discard)
 * ----------------------------------------------------------------------- */

static int
dp_rx_once(struct dp_softc *sc)
{
    u_char cdb[6];
    struct ifnet *ifp = eiftoifp(&sc->dp_eif);
    uint pktlen, flags, framelen, mbuflen;
    struct mbuf *m;
    struct etherbufhead *ebh;
    int snoopflags;
    int err;

    cdb[0] = DP_READ;
    cdb[1] = 0;
    cdb[2] = 0;
    cdb[3] = (DP_RX_BUFLEN >> 8) & 0xFF;
    cdb[4] = DP_RX_BUFLEN & 0xFF;
    cdb[5] = DP_READ_FLAGS;

    bzero(sc->dp_rxbuf, DP_RX_BUFSZ);

    err = dp_scsi_cmd(sc, cdb, 6, sc->dp_rxbuf, DP_RX_BUFLEN, SRF_DIR_IN);
    if (err != 0)
        return 0;

    pktlen = ((uint)sc->dp_rxbuf[0] << 8) | sc->dp_rxbuf[1];
    flags  = ((uint)sc->dp_rxbuf[2] << 24) | ((uint)sc->dp_rxbuf[3] << 16)
           | ((uint)sc->dp_rxbuf[4] << 8)  |  (uint)sc->dp_rxbuf[5];

    if (pktlen == 0)
        return 0;   /* no packets available */

    if (flags == DP_RX_DROPPED) {
        /*
         * Packet dropped: device buffer overrun.  A disable+enable sequence
         * clears the stuck state (per SLINKCMD.TXT).
         */
        cmn_err(CE_WARN, "dp%d: packet dropped, resetting\n", sc->dp_unit);
        dp_enable(sc, 0);
        dp_enable(sc, 1);
        dp_set_mode(sc);
        return 0;
    }

    /* pktlen includes 4-byte CRC; strip it to get the Ethernet frame */
    if (pktlen <= DP_CRC_LEN)
        return 0;
    framelen = pktlen - DP_CRC_LEN;

    if (framelen < sizeof(struct ether_header))
        return 0;

    /* mbuf payload = etherbufhead + (frame - ether header) */
    mbuflen = sizeof(struct etherbufhead)
            + framelen - sizeof(struct ether_header);

    m = m_vget(M_DONTWAIT, (int)mbuflen, MT_DATA);
    if (m == NULL) {
        ifp->if_ierrors++;
        return (flags & DP_RX_MORE) ? 1 : 0;
    }

    ebh = mtod(m, struct etherbufhead *);
    IF_INITHEADER(ebh, ifp, sizeof(struct etherbufhead));

    /* copy full Ethernet frame (header + payload) starting at rxbuf[6] */
    bcopy(sc->dp_rxbuf + DP_RX_HDR, &ebh->ebh_ether, framelen);

    m->m_len = (int)mbuflen;
    ifp->if_ipackets++;
    ifp->if_ibytes += framelen - sizeof(struct ether_header);

    snoopflags = (flags & DP_RX_MORE) ? SN_MORETOCOME : 0;
    ether_input(&sc->dp_eif, snoopflags, m);

    return (flags & DP_RX_MORE) ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * dp_rx_poll - 10ms recurring timer: drain pending RX packets
 * ----------------------------------------------------------------------- */

static void
dp_rx_poll(struct dp_softc *sc)
{
    int more, i;

    if (!sc->dp_enabled)
        return;

    /* drain up to 16 packets per tick to avoid starvation */
    more = 1;
    for (i = 0; i < 16 && more; i++)
        more = dp_rx_once(sc);

    /* reschedule at plbase so mutex_lock() can sleep if bus is busy */
    sc->dp_timer = itimeout((void (*)())dp_rx_poll, (void *)sc,
                            HZ / 100, plbase);
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

    dp_enable(sc, 1);
    dp_set_mode(sc);

    ifp->if_flags |= IFF_RUNNING;
    ifp->if_timer  = IFNET_SLOWHZ;     /* arm watchdog */

    sc->dp_enabled = 1;
    sc->dp_timer = itimeout((void (*)())dp_rx_poll, (void *)sc,
                            HZ / 100, plbase);
    return 0;
}

/* -----------------------------------------------------------------------
 * eio_reset - hardware reset (called on error recovery)
 * ----------------------------------------------------------------------- */

static void
dp_eio_reset(struct etherif *eif)
{
    struct dp_softc *sc = (struct dp_softc *)eif->eif_private;

    dp_enable(sc, 0);
    dp_enable(sc, 1);
    dp_set_mode(sc);
}

/* -----------------------------------------------------------------------
 * eio_watchdog - called ~1/sec by network stack via if_timer
 * ----------------------------------------------------------------------- */

static void
dp_eio_watchdog(struct ifnet *ifp)
{
    struct etherif *eif = ifptoeif(ifp);
    struct dp_softc *sc = (struct dp_softc *)eif->eif_private;

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
    u_char *p = sc->dp_txbuf;
    struct mbuf *n;
    int payloadlen, pktlen, err;
    u_char cdb[6];

    if (!sc->dp_enabled) {
        m_freem(m);
        return ENETDOWN;
    }

    /* assemble Ethernet header */
    bcopy(dhost->ea_vec, p, 6);     p += 6;
    bcopy(shost->ea_vec, p, 6);     p += 6;
    *(u_short *)p = type;           p += 2;

    /* flatten mbuf chain into tx buffer */
    payloadlen = 0;
    for (n = m; n; n = n->m_next) {
        int len = n->m_len;
        if (len == 0)
            continue;
        if (p + len > sc->dp_txbuf + DP_TX_BUFSZ) {
            ifp->if_oerrors++;
            m_freem(m);
            return EMSGSIZE;
        }
        bcopy(mtod(n, caddr_t), p, len);
        p += len;
        payloadlen += len;
    }
    m_freem(m);

    pktlen = (int)(sizeof(struct ether_header)) + payloadlen;

    cdb[0] = DP_WRITE;
    cdb[1] = 0;
    cdb[2] = 0;
    cdb[3] = (pktlen >> 8) & 0xFF;
    cdb[4] = pktlen & 0xFF;
    cdb[5] = 0x00;  /* raw frame mode per SLINKCMD.TXT */

    err = dp_scsi_cmd(sc, cdb, 6, sc->dp_txbuf, pktlen, 0);
    if (err != 0) {
        ifp->if_oerrors++;
        return ENOBUFS;
    }

    ifp->if_opackets++;
    ifp->if_obytes += payloadlen;
    return 0;
}

/* -----------------------------------------------------------------------
 * eio_ioctl - handle multicast / promiscuous ioctls
 * ----------------------------------------------------------------------- */

static int
dp_eio_ioctl(struct etherif *eif, int cmd, void *data)
{
    struct dp_softc *sc = (struct dp_softc *)eif->eif_private;

    switch (cmd) {
    case SIOCADDMULTI:
        /*
         * The DaynaPort has no per-address multicast filter.
         * SET INTERFACE MODE 0x04 enables reception of all broadcasts
         * (and on most firmware versions, multicasts too).  Issue it
         * once; subsequent calls are harmless.
         */
        dp_set_mode(sc);
        return 0;

    case SIOCDELMULTI:
        return 0;

    default:
        return EINVAL;
    }
}

/* -----------------------------------------------------------------------
 * dpinit - module init, called by lboot/ml framework
 * Registers with SCSI CDL as a handler for device type 3 (Processor).
 * ----------------------------------------------------------------------- */

void
dpinit(void)
{
    scsi_driver_register(3, "dp");
}

/* -----------------------------------------------------------------------
 * dpattach - CDL calls this for each discovered type-3 SCSI LUN
 * We verify it is a DaynaPort by checking the INQUIRY vendor/product strings,
 * read the MAC, enable the interface, and attach to the Ethernet stack.
 * ----------------------------------------------------------------------- */

int
dpattach(vertex_hdl_t conn_vhdl)
{
    scsi_unit_info_t  *unit_info;
    scsi_lun_info_t   *lun_info;
    u_char            *inq;
    struct dp_softc   *sc;
    struct etheraddr   ea;
    u_char             mac[6];
    int                unit;

    unit_info = scsi_unit_info_get(conn_vhdl);
    if (!unit_info)
        return -1;

    inq = SUI_INV(unit_info);
    if (!inq)
        return -1;

    /*
     * Verify vendor "Dayna" at offset 8 (5 chars) and
     * product "SCSI/Link" at offset 16 (9 chars).
     * INQUIRY data uses space-padded ASCII fields.
     */
    if (strncmp((char *)inq + 8,  "Dayna",     5) != 0 ||
        strncmp((char *)inq + 16, "SCSI/Link", 9) != 0) {
        return -1;   /* not a DaynaPort; let another driver try */
    }

    unit = dp_nunit;
    if (unit >= DP_MAXUNITS) {
        cmn_err(CE_WARN, "dp: too many DaynaPort devices (max %d)\n",
                DP_MAXUNITS);
        return -1;
    }

    lun_info = SUI_LUN_INFO(unit_info);
    if (!lun_info)
        return -1;

    if (SLI_ALLOC(lun_info)(SLI_LUN_VHDL(lun_info), 1, NULL) != SCSIALLOCOK) {
        cmn_err(CE_WARN, "dp%d: scsi_alloc failed\n", unit);
        return -1;
    }

    sc = (struct dp_softc *)kmem_zalloc(sizeof(*sc), KM_SLEEP);
    if (!sc) {
        SLI_FREE(lun_info)(SLI_LUN_VHDL(lun_info), NULL);
        return -1;
    }

    sc->dp_unit     = unit;
    sc->dp_lun_vhdl = SLI_LUN_VHDL(lun_info);
    sc->dp_lun_info = lun_info;
    mutex_init(&sc->dp_lock, MUTEX_DEFAULT, "dp_lock");

    /* retrieve MAC address */
    if (!dp_get_mac(sc, mac)) {
        cmn_err(CE_WARN, "dp%d: failed to read MAC address\n", unit);
        mutex_destroy(&sc->dp_lock);
        kmem_free(sc, sizeof(*sc));
        SLI_FREE(lun_info)(SLI_LUN_VHDL(lun_info), NULL);
        return -1;
    }

    /* enable the interface and set broadcast reception */
    dp_enable(sc, 1);
    dp_set_mode(sc);

    bcopy(mac, ea.ea_vec, 6);

    SUI_CTINFO(unit_info) = (void *)sc;
    dp_units[unit] = sc;
    dp_nunit++;

    /* register with IRIX Ethernet layer */
    ether_attach(&sc->dp_eif, "dp", unit, (caddr_t)sc,
                 &dp_ops, &ea, INV_ETHER_EP, 0);

    add_to_inventory(INV_NETWORK, INV_NET_ETHER, INV_ETHER_EP, unit, 0);

    cmn_err(CE_NOTE, "dp%d: DaynaPort SCSI/Link at SCSI id %d, "
            "MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            unit,
            (int)SLI_TARG(SUI_LUN_INFO(unit_info)),
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return 0;
}
