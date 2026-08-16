/*
 * dp_proto.c - DaynaPort protocol core, shared VERBATIM between the IRIX 6.5
 * driver (../irix6.5/if_dp.c) and the IRIX 5.3 port (../irix5.3/if_dp.c).
 *
 * This file is #included by both if_dp.c, never compiled on its own. It relies
 * entirely on what each driver defines ABOVE the include: the struct dp_softc,
 * the 6.5<->5.3 shim macros, the DP_* CDB constants and the kernel headers.
 * Keeping the protocol in one file means a fix lands in both drivers at once -
 * which is why the old byte-identity checker (drift.sh) is gone.
 *
 * Covers: dp_scsi_cmd_locked, dp_enable / set_mode / get_mac, the RX
 * multi-packet parser (dp_do_rx), the TX ring (dp_do_tx), dp_runqueue, the CDB
 * builders and all five etherif handlers.
 */

/* dp_scsi_cmd_locked - acquire dp_qlock, run command, release.
 * Use for control commands (enable, set_mode, get_mac) called outside
 * of dp_runqueue context. */
static int
dp_scsi_cmd_locked(struct dp_softc *sc, u_char *cdb, int cdblen,
                   void *buf, int buflen, ushort dir)
{
    int status;
#ifdef DP_ASYNC_RX
    /* The target answers one command at a time and the async engine may have
     * one outstanding. Claim the device: dp_fg stops new submissions, then
     * wait for any in-flight one. User context here, so delay() is legal. */
    sc->dp_fg++;
    while (sc->dp_abusy)
        delay(1);
#endif
    mutex_lock(&sc->dp_qlock, PZERO);
    status = dp_scsi_cmd(sc, cdb, cdblen, buf, buflen, dir);
    mutex_unlock(&sc->dp_qlock);
#ifdef DP_ASYNC_RX
    sc->dp_fg--;
    dp_async_tick(sc);          /* resume polling */
#endif
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

/* dp_rx_parse - dispatch every frame packed into dp_rxbuf by a completed
 * READ. Split out of dp_do_rx() so the asynchronous path (5.3, see
 * DP_ASYNC_RX) can reuse it from a completion routine, where issuing a new
 * SCSI command is fine but sleeping is not.
 *
 * Returns 1 if the device said more packets are queued, 0 otherwise, and
 * -1 if it reported a drop (the caller decides whether it is in a context
 * that may run the disable/enable/set-mode recovery, which sleeps). */
int
dp_rx_parse(struct dp_softc *sc)
{
    struct ifnet *ifp = eiftoifp(&sc->dp_eif);
    uint pktlen, flags, framelen, mbuflen;
    struct mbuf *m;
    struct etherbufhead *ebh;
    int snoopflags;
    u_char *p;
    u_char *end;
    int last_more;

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
            cmn_err(CE_WARN, "dp%d: packet dropped\n", sc->dp_unit);
            return -1;
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

/* dp_do_rx - synchronous READ + parse. Sleeps, so it may only be called from
 * a context that is allowed to: NOT from an itimeout() callback on 5.3. */
static int
dp_do_rx(struct dp_softc *sc)
{
    u_char cdb[6];
    int err;
    int more;

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

    more = dp_rx_parse(sc);
    if (more < 0) {                     /* device dropped: full recovery */
        dp_enable(sc, 0);
        dp_enable(sc, 1);
        dp_set_mode(sc);
        return 0;
    }
    return more;
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
#ifdef DP_ASYNC_RX
    /* 5.3: nothing may sleep here, so hand off to the async engine - it arms
     * the next tick and submits one command, then returns. Everything below
     * is the portable synchronous path, which 6.5 still uses. */
    sc->dp_timer = 0;
#ifdef DP_LOG_TICKRATE
    sc->dp_ntick++;
#endif
    dp_async_poll(sc);
    return;
#endif
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

    if (sc->dp_enabled) {
        ifp->if_timer = IFNET_SLOWHZ;
#if defined(DP_ASYNC_RX) && defined(DP_LOG_TICKRATE)
        /* This watchdog is the one clock in the driver whose rate we can
         * trust: the ifnet layer runs it at IFNET_SLOWHZ regardless of what
         * our own timer is doing. So measure the poll against it.
         *
         *   tick=~100  the poll is running at the HZ/100 it asked for, and a
         *              latency problem is somewhere other than the timer;
         *   tick=~1    itimeout() is not delivering the requested delay;
         *   tick=0     the chain has lapsed altogether and the only thing
         *              still collecting packets is this watchdog.
         *
         * sub/done bracket the SCSI round trip: ticks without submissions
         * mean the engine is being skipped (busy/fg), submissions without
         * completions mean the device is not answering. Note that dp_stall
         * is counted in poll ticks, so if tick is ~1 the stall guard needs
         * DP_STALL_TICKS seconds rather than DP_STALL_TICKS/100 to fire -
         * its silence is not evidence that no completion was lost. */
        cmn_err(CE_NOTE, "dp%d: 1s tick=%u sub=%u done=%u rx=%u"
                " busy=%d fg=%d stall=%d\n", sc->dp_unit,
                sc->dp_ntick, sc->dp_nsub, sc->dp_ndone,
                (uint)ifp->if_ipackets - sc->dp_nrx,
                sc->dp_abusy, sc->dp_fg, sc->dp_stall);
        sc->dp_ntick = sc->dp_nsub = sc->dp_ndone = 0;
        sc->dp_nrx   = (uint)ifp->if_ipackets;
#endif
    }
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

#ifdef DP_ASYNC_RX
    dp_async_tick(sc);          /* submits it now, or picks it up next tick */
#else
    /* kick the queue: trylock so we don't block if runqueue is active */
    if (mutex_trylock(&sc->dp_qlock)) {
        dp_runqueue(sc);
        mutex_unlock(&sc->dp_qlock);
    }
#endif

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

