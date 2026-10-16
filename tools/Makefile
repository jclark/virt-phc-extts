# SPDX-License-Identifier: MIT
CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

.PHONY: all clean

all: test-vphc vphc-pulse

test-vphc: test-vphc.c
	$(CC) $(CFLAGS) -o $@ $<

vphc-pulse: pulse.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f test-vphc vphc-pulse
