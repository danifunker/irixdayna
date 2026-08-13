#!smake
#
# Makefile for IRIX DaynaPort SCSI/Link Ethernet Driver (if_dp) - IRIX 5.3
#
# IRIX 5.3 has no loadable kernel module support (no ml(1M)), so unlike the
# 6.5 build there is no load/unload/reload target: the driver is linked into
# the kernel by lboot and requires a reboot.
#
# CPUBOARD IS MANDATORY.  5.3's /var/sysgen/Makefile.kernio wraps its whole
# body in "#if defined(CPUBOARD) && !empty(CPUBOARD)" and defines CFLAGS
# only inside it.  With CPUBOARD unset you get an EMPTY CFLAGS, which means
# no -D_KERNEL, no -coff, and no R4000 errata workarounds - it compiles, and
# produces an object that will wreck the kernel.  Always check `hinv`.
#
# We deliberately do NOT set our own ABI flags.  Makefile.kernio supplies the
# complete, SGI-blessed set for each board, e.g. for IP22:
#
#   -D_K32U32 -D_KERNEL -DSTATIC=static -DJUMP_WAR -DPROBE_WAR -DBADVA_WAR
#   -DIP22 -DR4000 -Wx,-G8 -non_shared -coff -Wc,-pic0
#
# Note there is no -mips2: SGI does not pass one, so neither do we. The
# JUMP_WAR/PROBE_WAR/BADVA_WAR defines are R4000 silicon errata workarounds
# and are not optional on IP17/IP20/IP22.
#
# Usage:
#   smake                       # Indy / Indigo2 / Challenge S (IP22) default
#   smake CPUBOARD=IP20         # Indigo R4000
#   smake CPUBOARD=IP12         # Indigo R3000
#   smake CPUBOARD=IP19         # Challenge / Onyx
#   smake install               # install for lboot
#   smake drift                 # check protocol code still matches 6.5
#

# Must precede the include - Makefile.kernio tests it at parse time. If your
# smake only honours the environment here, use:  setenv CPUBOARD IP22
CPUBOARD=IP22

include /var/sysgen/Makefile.kernio

# -I. so our sgi_ether.h is found (IRIX does not ship ether.h).
#
# We do NOT add -D_MP_NETLOCKS -DMP. The 5.3 driver guide mentions them for
# multi-threaded TCP/IP, but Makefile.kernio does not pass them for ANY board
# - including IP19/Challenge - so a stock kernel is not built with them.
# Setting them here when the kernel was not built that way would change the
# size of struct ifnet, and therefore of the struct etherif we embed in the
# softc, which is precisely the silent-corruption failure this port is most
# exposed to. If you are targeting a kernel you KNOW was built MP, override:
#   smake MYCFLAGS="-I. -D_MP_NETLOCKS -DMP"
MYCFLAGS=-I.

SRCS=if_dp.c
MODULE=dp.o

all: $(MODULE)

# Single translation unit: the compiler output is the loadable object, so no
# separate "ld -r" step is needed.
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
	@echo "Note INCLUDE: (not USE:) - 5.3 lboot calls dp_init() for"
	@echo "INCLUDE'd drivers.  USE: is a 6.x spelling."

# Verify the duplicated protocol code has not drifted from the 6.5 driver.
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
	@echo "  CPUBOARD  IP22 (default) IP20 IP12 IP19 IP17 - MUST match hinv"
	@echo "  MYCFLAGS  extra cflags (default -I.)"
	@echo ""
	@echo "Debug:"
	@echo "  -DDP_LOG        init/attach/enable/reset events"
	@echo "  -DDP_LOG_SCSI   SCSI command traces (op/status/resid)"
	@echo "  -DDP_LOG_NET    RX/TX packet traces"
	@echo "  -DDP_NO_TRYLOCK disable the TX-side queue kick"
	@echo "  -DDP_CHECK_ETHERIF  canary after dp_eif to catch a struct"
	@echo "                  etherif layout mismatch - USE THIS ON THE"
	@echo "                  FIRST BOOT OF ANY NEW MACHINE"
	@echo ""
	@echo "Recommended first build:"
	@echo "  smake MYCFLAGS=\"-I. -DDP_LOG -DDP_LOG_SCSI -DDP_CHECK_ETHERIF\""
	@echo ""
	@echo "There is no load/unload target: IRIX 5.3 has no ml(1M)."

.PHONY: all install drift clean help
