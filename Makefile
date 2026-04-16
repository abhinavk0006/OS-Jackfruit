# Makefile for OS-Jackfruit (FIXED STATIC BUILD)

KDIR   ?= /lib/modules/$(shell uname -r)/build
CC      = gcc
CFLAGS  = -O2 -static
LDFLAGS = -lpthread

obj-m  := monitor.o

.PHONY: all user kernel load unload clean

all: user kernel

# ---------- user-space ----------

user: engine workload1 workload2 memory_hog

engine: engine.c monitor_ioctl.h
	$(CC) -Wall -O2 -D_GNU_SOURCE -o $@ engine.c $(LDFLAGS)

workload1: workload1.c
	$(CC) $(CFLAGS) -o $@ $<

workload2: workload2.c
	$(CC) $(CFLAGS) -o $@ $<

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o $@ $<

# ---------- kernel module ----------

kernel:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

load:
	sudo insmod monitor.ko

unload:
	sudo rmmod monitor || true

# ---------- cleanup ----------

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f engine workload1 workload2 memory_hog
	rm -f *.o *.mod *.mod.c *.symvers *.order
