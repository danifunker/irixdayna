#!smake
#
# Makefile for IRIX DaynaPort SCSI/Link Ethernet Driver (if_dp) - IRIX 5.3
#
# IRIX 5.3 has no loadable kernel module support (no ml(1M)), so unlike
# the 6.5 build there is no load/unload/reload target: the driver is
# linked into the kernel by lboot and requires a reboot.
#
# IRIX 5.3 is o32 (32-bit) only.  There is no -n32 and no -64, and all
# IRIX 5.x drivers must be compiled -coff.
#
# Unlike ../Makefile this file uses plain variables rather than smake
# #if/#elif conditionals, so that it does not depend on the conditional
# support of whichever make ships with your 5.3 install.  Override on
# the command line:
#
#   smake                      # Indy / Indigo2 (IP22, R4x00) - default
#   smake MIPSOPT=-mips1       # Indigo R3000 (IP12), Personal Iris
#   smake install              # install for lboot
#   smake drift                # check protocol code still matches 6.5
#

include /var/sysgen/Makefile.kernio

# ---------------------------------------------------------------------
# Target selection
#
# IRIX 5.3 runs on IP12/IP17/IP19/IP20/IP21/IP22 only.  IP26/IP28 need
# 6.2+, and IP30/IP32/IP35 (Octane/O2/Fuel) need 6.3+ - for those use
# the 6.5 driver in the parent directory.
#
#   -mips2   R4000/R4400 (Indy, Indigo2, Challenge S)  <- default
#   -mips1   R3000 (Indigo, Personal Iris)
# ---------------------------------------------------------------------

MIPSOPT=-mips2

# -coff is required for all IRIX 5.x kernel objects.
# -D_MP_NETLOCKS -DMP enable the multi-threaded TCP/IP locking macros,
# which IRIX 5.3 requires of any ifnet driver (see the Device Driver
# Programming Guide, ch.9 "Compilation Flags for MP TCP/IP").
MYCFLAGS=-coff $(MIPSOPT) -D_MP_NETLOCKS -DMP -I.

SRCS=if_dp.c
MODULE=dp.o

all: $(MODULE)

# Single translation unit: the compiler output is the loadable object,
# so no separate "ld -r" step is needed.
$(MODULE): $(SRCS) sgi_ether.h
	$(CC) $(CFLAGS) $(MYCFLAGS) -c $(SRCS) -o $(MODULE)

install: $(MODULE)
	cp master.d/dp /var/sysgen/master.d/dp
	cp $(MODULE)   /var/sysgen/boot/dp.o
	@echo ""
	@echo "Add the following line to /var/sysgen/system/irix.sm:"
	@echo "  INCLUDE: dp"
	@echo ""
	@echo "Then run:  /etc/autoconfig -f && reboot"
	@echo ""
	@echo "Note: INCLUDE: (not USE:) - 5.3 lboot calls dp_init() for"
	@echo "INCLUDE'd drivers.  USE: is a 6.x spelling."

# Verify the duplicated protocol code has not drifted from the 6.5
# driver.  See drift.sh for why the duplication exists.
drift:
	@sh ./drift.sh

clean:
	rm -f $(MODULE)

help:
	@echo "DaynaPort SCSI/Link Ethernet Driver - IRIX 5.3"
	@echo ""
	@echo "Targets:"
	@echo "  all      Build dp.o (default)"
	@echo "  install  Install for lboot (needs autoconfig + reboot)"
	@echo "  drift    Check protocol code still matches the 6.5 driver"
	@echo "  clean    Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  MIPSOPT  -mips2 (R4x00, default) or -mips1 (R3000)"
	@echo ""
	@echo "Debug:"
	@echo "  -DDP_LOG        init/attach/enable/reset events"
	@echo "  -DDP_LOG_SCSI   SCSI command traces (op/status/resid)"
	@echo "  -DDP_LOG_NET    RX/TX packet traces"
	@echo "  -DDP_NO_TRYLOCK disable the TX-side queue kick (see XXX53"
	@echo "                  note on cpsema in if_dp.c)"
	@echo "  -DDP_CHECK_ETHERIF  canary after dp_eif to catch a"
	@echo "                  struct etherif layout mismatch - USE THIS"
	@echo "                  ON THE FIRST BOOT OF ANY NEW MACHINE"
	@echo ""
	@echo "Recommended first build on a new machine:"
	@echo "  smake MYCFLAGS=\"-coff -mips2 -D_MP_NETLOCKS -DMP -I. \\"
	@echo "                  -DDP_LOG -DDP_LOG_SCSI -DDP_CHECK_ETHERIF\""
	@echo ""
	@echo "There is no load/unload target: IRIX 5.3 has no ml(1M)."

.PHONY: all install drift clean help
