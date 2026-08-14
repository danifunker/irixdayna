# IRIX Network and SCSI Architecture for DaynaPort Driver

This document outlines the architecture of Ethernet and SCSI drivers in IRIX, specifically detailing how to write a SCSI-to-Ethernet network adapter driver like the **DaynaPort SCSI Link**.

---

## 1. Ethernet Driver Architecture in IRIX

IRIX utilizes a BSD-derived network interface model extended to support multi-processing (MP) safety and loadable drivers.

### 1.1. Core Ethernet Driver Registration

The driver initializes and registers itself with the generic Ethernet layer using the [ether_attach](file:///home/dbehr/gits/irix/irix/kern/bsd/misc/ether.c#L101) function:

```c
void ether_attach(
    struct etherif    *eif,
    char              *name,
    int                unit,
    caddr_t            eif_private,
    struct etherifops *ops,
    struct etheraddr  *ea,
    int                controller,
    int                state
);
```

- **`eif`**: Pointer to a [struct etherif](file:///home/dbehr/gits/irix/irix/kern/bsd/misc/ether.h#L115) allocated within the driver's per-unit structure.
- **`name`**: The interface prefix string (e.g., `"dp"` for DaynaPort).
- **`unit`**: Device unit number (e.g., `0`, `1`).
- **`eif_private`**: Pointer to the driver's private per-device state structure.
- **`ops`**: Pointer to the [struct etherifops](file:///home/dbehr/gits/irix/irix/kern/bsd/misc/ether.h#L137) containing the driver entry points.
- **`ea`**: The hardware MAC address of the adapter.
- **`controller`**: Controller type (defined in `sys/invent.h`).
- **`state`**: Initial status flag.

#### Example registration call:
```c
ether_attach(&dp_info->dp_eif, "dp", unit, (caddr_t)dp_info, &dpops, &dp_addr, INV_ETHER_EP, 0);
```

---

### 1.2. The Driver Operations Table (`etherifops`)

The driver implements interface control and packet transmission entry points via [struct etherifops](file:///home/dbehr/gits/irix/irix/kern/bsd/misc/ether.h#L137):

```c
struct etherifops {
    int  (*eio_init)(struct etherif *eif, int flags);
    void (*eio_reset)(struct etherif *eif);
    void (*eio_watchdog)(struct ifnet *ifp);
    int  (*eio_transmit)(struct etherif *eif, struct etheraddr *dhost,
                         struct etheraddr *shost, u_short type, struct mbuf *m);
    int  (*eio_ioctl)(struct etherif *eif, int cmd, void *data);
};
```

1. **`eio_init`**: Prepares the hardware, configures multicast filtering, sets the MAC address from `eif_arpcom.ac_enaddr`, and changes interface status to up/running.
2. **`eio_reset`**: Resets the hardware to recover from fatal error states.
3. **`eio_watchdog`**: Scheduled regularly by the network stack to check if the card has hung (using `ifp->if_timer` countdown).
4. **`eio_transmit`**: Handles outgoing packet serialization.
5. **`eio_ioctl`**: Configures hardware features (promiscuous mode, multicast address addition `SIOCADDMULTI` and deletion `SIOCDELMULTI`).

---

### 1.3. Outgoing Packet Path (TX)

Transmit frames are pushed into the driver by the IP stack invoking the driver's registered `eio_transmit` routine:

```c
int dp_transmit(
    struct etherif   *eif,
    struct etheraddr *dhost,
    struct etheraddr *shost,
    u_short           type, // in network byte order
    struct mbuf      *m
);
```

#### TX Workflow:
1. **Header Assembly**: Prepend the destination and source MAC addresses, followed by the Ethernet frame type, to the packet payload. (Drivers can construct the header directly or prepend an `ether_header` onto the `mbuf` chain).
2. **Hardware Queueing**: Queue the `mbuf` chain to the device.
3. **Memory Cleanup**: 
   - On **success** (or when queued successfully), the driver must free the `mbuf` chain using `m_freem(m)`.
   - On **failure** (e.g., ring buffer full), the driver returns a standard error code (e.g., `ENOBUFS`) and leaves the `mbuf` alone so the stack can queue or retry it.

---

### 1.4. Incoming Packet Path (RX)

Upon receiving an Ethernet frame (normally from a SCSI command response or completion interrupt), the driver pushes the packet up the network stack:

1. **Mbuf Allocation**: Allocate an `mbuf` cluster (typically with [m_vget](file:///home/dbehr/gits/irix/irix/kern/bsd/mips/if_ef.c#L1278) containing [struct etherbufhead](file:///home/dbehr/gits/irix/irix/kern/bsd/misc/ether.h#L88) at the start).
2. **Data Alignment**: Ensure the packet payload is correctly positioned, and set `m->m_len` to:
   $$\text{len} = (\text{Packet Length} - \text{sizeof}(\text{struct ether\_header})) + \text{sizeof}(\text{struct etherbufhead})$$
3. **Interface Association**: Initialize the packet header indicating the source network interface structure:
   ```c
   IF_INITHEADER(&rb->rb_ebh, &dp_info->dp_if, sizeof(struct etherbufhead));
   ```
4. **Statistics**: Increment `dp_info->dp_if.if_ipackets` and `dp_if.if_ibytes`.
5. **Protocol Delivery**: Send the packet up the stack:
   ```c
   ether_input(&dp_info->dp_eif, snoopflags, m);
   ```
   *Note: If multiple packets are dispatched in the same interrupt batch, pass `SN_MORETOCOME` in `snoopflags` to hint the stack to skip immediate blocking/waiting operations.*

---

## 2. SCSI Driver Architecture in IRIX

IRIX manages physical SCSI adapters, target IDs, and logical unit numbers (LUNs) via the hardware graph (**hwgraph**) namespace. 

### 2.1. Device Discovery and the Hardware Graph

During startup, the SCSI subsystem probes the host adapters and registers active target devices in the path:
`/hw/scsi_ctlr/<ctlr_num>/target/<target_id>/lun/<lun_num>`

To attach class-specific drivers (e.g. disk, tape, or general-purpose processors), the SCSI core relies on the **Class Driver Library (CDL)**.

1. **Driver Registration**: During driver initialization, a SCSI driver registers its device class using [scsi_driver_register](file:///home/dbehr/gits/irix/irix/kern/io/scsi.c#L259):
   ```c
   scsi_driver_register(unit_type, "dp");
   ```
   - `unit_type`: The SCSI device type reported in the INQUIRY data (e.g., `3` for Processor/Communications devices).
   - `"dp"`: The driver naming prefix.
2. **Driver Entry Points**: Lboot compiles static drivers and exports their character device switches ([struct cdevsw](file:///home/dbehr/gits/irix/irix/kern/sys/conf.h#L45)). The registry resolves the driver functions using the name prefix.
3. **Device Binding (Attach)**:
   - When a matching target/LUN is found, the CDL matches the device's SCSI `unit_type` and invokes the driver's registered `d_attach` routine:
     ```c
     int dpattach(vertex_hdl_t conn_vhdl);
     ```
   - In `dpattach()`, the driver accesses target configurations and binds its private metadata:
     ```c
     scsi_unit_info_t *unit_info = scsi_unit_info_get(conn_vhdl);
     u_char *inq_data = SUI_INV(unit_info);
     
     // Check if Vendor ID / Product ID in inq_data matches DaynaPort SCSI Link
     if (is_daynaport(inq_data)) {
         struct dp_softc *sc = alloc_dp_softc();
         SUI_CTINFO(unit_info) = sc; // save state handle
         ...
     }
     ```

---

### 2.2. Communicating with SCSI Devices

High-level SCSI device drivers communicate with host adapters by compiling and sending requests via the [scsi_request](file:///home/dbehr/gits/irix/irix/kern/sys/scsi.h#L323) structure.

```c
typedef struct scsi_request {
    u_char        sr_ctlr;         // Controller number
    u_char        sr_target;       // SCSI Target ID
    u_char        sr_lun;          // SCSI LUN
    vertex_hdl_t  sr_lun_vhdl;     // LUN vertex handle
    
    u_char       *sr_command;      // SCSI Command Descriptor Block (CDB) pointer
    ushort        sr_cmdlen;       // CDB length (6, 10, or 12 bytes)
    ushort        sr_flags;        // Direction flag (SRF_DIR_IN, SRF_FLUSH, etc.)
    uint          sr_timeout;      // Timeout in HZ
    
    u_char       *sr_buffer;       // Data payload buffer
    uint          sr_buflen;       // Payload size
    
    u_char       *sr_sense;        // Request Sense buffer
    uint          sr_senselen;     // Sense buffer capacity
    
    void        (*sr_notify)(struct scsi_request *); // Completion callback
    
    // Output status populated by host adapter
    uint          sr_status;       // System/Bus/DMA error status (SC_GOOD, SC_TIMEOUT, etc.)
    u_char        sr_scsi_status;  // SCSI protocol status byte (ST_GOOD, ST_CHECK, etc.)
    short         sr_sensegotten;  // Size of sense data captured on ST_CHECK (-1 on error)
    uint          sr_resid;        // Residual transfer count
} scsi_request_t;
```

#### Executing a SCSI command:
1. **Acquire LUN Information**:
   ```c
   scsi_lun_info_t *lun_info = SUI_LUN_INFO(unit_info);
   ```
2. **Initialize Connection**: Ensure the device is allocated for driver use by calling the `SLI_ALLOC` macro:
   ```c
   int res = SLI_ALLOC(lun_info)(SLI_LUN_VHDL(lun_info), max_queue_depth, callback);
   if (res != SCSIALLOCOK) {
       // Handle lock/allocation error
   }
   ```
3. **Populate Request**: Set up the CDB and buffer parameters in the `scsi_request` structure.
4. **Issue Command**: Submit the transaction using the adapter command execution macro:
   ```c
   SLI_COMMAND(lun_info)(req);
   ```
   - For **synchronous commands**: The driver blocks until the host adapter finishes the request.
   - For **asynchronous commands**: The driver defines `sr_notify` callback. When the transaction finishes, the SCSI host adapter calls `sr_notify(req)` under interrupt context.
5. **Disconnect**: When the driver is done with the device, call `SLI_FREE` to release resources:
   ```c
   SLI_FREE(lun_info)(SLI_LUN_VHDL(lun_info), callback);
   ```

---

## 3. DaynaPort SCSI Link Implementation Considerations

A DaynaPort driver will bridge SCSI commands to the network subsystem:
1. **Discovery**: Register under SCSI device type `3` (Processor). Verify the device vendor during `d_attach` using the SCSI `INQUIRY` response.
2. **TX Packets**: In `dp_transmit()`, convert the outgoing Ethernet `mbuf` chain into a SCSI `WRITE` command CDB (typically vendor-specific DaynaPort format). Send the data payload via `SLI_COMMAND` to the adapter.
3. **RX Packets**: Run a background thread or a recurring timer that issues SCSI `READ` commands to poll the device for incoming network packets. Once received, wrap the SCSI data buffer into an `mbuf` and pass it to the stack using `ether_input()`.
