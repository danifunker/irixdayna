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

/* DaynaPort protocol core - shared verbatim with the other release's driver.
 * See shared/dp_proto.c. */
#include "dp_proto.c"

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
