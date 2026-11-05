KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build
KDIR := $(CURDIR)
# Module parameters for "make load".
PARAMS ?= primary_userspace_nv12=1

modules:
	make -C $(KSRC) M=$(KDIR) modules

clean:
	make -C $(KSRC) M=$(KDIR) clean

install: modules
	make -C $(KSRC) M=$(KDIR) modules_install
	depmod -a

# Quick test without installing (as root, after "make" as your user): insmod
# does not load dependencies, so load the ones modinfo lists first.
load:
	@test -f trigger6.ko || { echo "trigger6.ko not found: run 'make' first"; exit 1; }
	@deps="$$(modinfo -F depends ./trigger6.ko | tr ',' ' ')"; \
	if [ -n "$$deps" ]; then echo "modprobe -a $$deps"; modprobe -a $$deps; fi
	insmod ./trigger6.ko $(PARAMS)

# Stop the helper first; with the adapter plugged in, the desktop may still
# hold the device (see README section A).
unload:
	rmmod trigger6

.PHONY: modules clean install load unload
