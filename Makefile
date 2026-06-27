#!smake
#
# Makefile for IRIX DaynaPort SCSI/Link Ethernet Driver (if_dp)
#
# Builds a loadable kernel module for IRIX 6.5.
# Uses smake and follows IRIX loadable module conventions.
#
# Supported platforms: IP22, IP30, IP32, IP35 (and earlier).
# The driver source is architecture-independent; only CPUBOARD
# affects ABI flags passed to the compiler and linker.
#
# Usage:
#   smake CPUBOARD=IP30        # build for Octane
#   smake CPUBOARD=IP32        # build for O2
#   smake CPUBOARD=IP35        # build for Fuel/Origin 350
#   smake CPUBOARD=IP22        # build for Indigo2/Challenge S
#   smake load                 # load the driver into the running kernel
#   smake unload               # unload
#   smake reload               # unload + load
#   smake install              # copy to /var/sysgen for autoconfig
#

CPUBOARD=IP30
BUILTIN=0

#if $(BUILTIN) == "1"
include /var/sysgen/Makefile.kernio
BUILTIN_CFLAGS=-DDP_BUILTIN
#else
include /var/sysgen/Makefile.kernloadio
BUILTIN_CFLAGS=-DDP_MODULE
#endif

# Per-platform ABI flags
LDFLAGS_IP35=-nostdlib -64 -mips4
LDFLAGS_IP30=-nostdlib -64 -mips4
LDFLAGS_IP32=-nostdlib -n32 -mips3
LDFLAGS_IP22=-nostdlib -n32 -mips3

MYCFLAGS_IP35=-mips4 -DPTE_64BIT
MYCFLAGS_IP30=-mips4 -DPTE_64BIT -DHEART_INVALIDATE_WAR
MYCFLAGS_IP32=-mips3
MYCFLAGS_IP22=-mips3

#if $(CPUBOARD) == "IP30"
MYCFLAGS=$(MYCFLAGS_IP30) $(BUILTIN_CFLAGS)
LDFLAGS=$(LDFLAGS_IP30) -v
#elif $(CPUBOARD) == "IP32"
MYCFLAGS=$(MYCFLAGS_IP32) $(BUILTIN_CFLAGS)
LDFLAGS=$(LDFLAGS_IP32) -v
#elif $(CPUBOARD) == "IP35"
MYCFLAGS=$(MYCFLAGS_IP35) $(BUILTIN_CFLAGS)
LDFLAGS=$(LDFLAGS_IP35) -v
#elif $(CPUBOARD) == "IP22"
MYCFLAGS=$(MYCFLAGS_IP22) $(BUILTIN_CFLAGS)
LDFLAGS=$(LDFLAGS_IP22) -v
#else
MYCFLAGS=$(BUILTIN_CFLAGS)
LDFLAGS=-nostdlib -v
#endif

ML=ml

SRCS=if_dp.c
OBJS=$(SRCS:.c=.o)
MODULE=dp.o

all: $(MODULE)

$(MODULE): $(OBJS)
	$(LD) $(LDFLAGS) -r $(OBJS) -o $(MODULE)

.c.o:
	$(CC) $(CFLAGS) $(MYCFLAGS) -c $<

if_dp.o: if_dp.c

# Major device number 44 — choose a free slot on your system.
# Check /var/sysgen/master.d/* for conflicts.
DP_MAJOR=44

load: $(MODULE)
	@echo "Loading DaynaPort driver..."
	$(ML) ld -v -c $(MODULE) -p dp_ -s $(DP_MAJOR)

unload:
	$(ML) unld -v -p dp_

reload: unload load

list:
	$(ML) list

# Permanent installation: copies files and prints autoconfig instructions.
install: $(MODULE)
	cp master.d/dp /var/sysgen/master.d/dp
	cp $(MODULE)   /var/sysgen/boot/dp.o
	@echo ""
	@echo "Add the following line to /var/sysgen/system/irix.sm:"
	@echo "  USE: dp"
	@echo ""
	@echo "Then run:  autoconfig && reboot"

clean:
	rm -f $(OBJS) $(MODULE)

reboot:
	shutdown -y -g0 -i6

help:
	@echo "DaynaPort SCSI/Link Ethernet Driver Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all      Build dp.o (default)"
	@echo "  load     Load into running kernel (major=$(DP_MAJOR))"
	@echo "  unload   Unload from kernel"
	@echo "  reload   Unload + load"
	@echo "  list     Show loaded modules"
	@echo "  install  Install for permanent boot (needs reboot + autoconfig)"
	@echo "  clean    Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  CPUBOARD   Target board: IP22 IP30 IP32 IP35  (default: $(CPUBOARD))"
	@echo "  DP_MAJOR   Major device number              (default: $(DP_MAJOR))"

.PHONY: all load unload reload list install clean reboot help
