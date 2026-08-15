product dp
    id "DaynaPort SCSI/Link Ethernet driver (@ABI_DESC@)"
    image sw
        id "DaynaPort driver"
        version @VERSION@
        subsys driver default
            id "dp driver + all @ABI@ boards. AFTER INSTALL run /var/sysgen/dp/dpinstall"
            replaces self
            exp dp.sw.driver
        endsubsys
    endimage
endproduct
