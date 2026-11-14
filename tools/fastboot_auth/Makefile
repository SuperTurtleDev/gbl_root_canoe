CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lssl -lcrypto

all: fastboot_auth

fastboot_auth: fastboot_auth.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f fastboot_auth

.PHONY: all clean
