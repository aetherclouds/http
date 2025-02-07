FLAGS :=
CFLAGS := -std=c23 -Wall -Wextra
OUTFILE := server

.PHONY: run prod deb

run: bin/$(OUTFILE)
	$<

prod: CFLAGS +=-O3
prod: bin/$(OUTFILE)

deb: CFLAGS += -ggdb3
deb: bin/$(OUTFILE)

bin/$(OUTFILE): main.c
	gcc -o $@ $< $(CFLAGS) $(FLAGS)

bin/measure: measure.c
	gcc -o $@ $< $(CFLAGS) $(FLAGS)
