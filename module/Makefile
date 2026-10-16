# SPDX-License-Identifier: MIT
obj-m += virt_phc_extts.o
KDIR ?= /lib/modules/$(shell uname -r)/build

# Detect internal APIs that differ across the supported kernels.
ifneq ($(shell grep -w 'ktime_get_snapshot_id' $(srctree)/include/linux/timekeeping.h 2>/dev/null),)
ccflags-y += -DVPHC_SNAPSHOT_ID
endif
ifneq ($(shell grep -w 'hrtimer_setup' $(srctree)/include/linux/hrtimer.h 2>/dev/null),)
ccflags-y += -DVPHC_HRTIMER_SETUP
endif

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
