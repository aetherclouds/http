# MACROS:
# IPV4 - IPv4 support (default IPv6)
# TYPE: 1 | 2 - 1. buffered write, 2. sendfile (default 2)

CFLAGS :=
OUTFILE := server

.PHONY: run prod deb

run: deb
	bin/$(OUTFILE)

prod: TFLAGS := -O3
prod: bin/$(OUTFILE)

deb: TFLAGS := -g
deb: bin/$(OUTFILE)

bin/$(OUTFILE): main.c
	gcc -o $@ $< -std=c23 -Wall -Wextra $(CFLAGS) $(TFLAGS)
