OUTFILE := server
.PHONY: run debug
run: server
	./$(OUTFILE)
server: main.c
	gcc -o $(OUTFILE) -std=c23 main.c -Wall -Wextra $(CFLAGS) -O2
debug: main.c
	gcc -o $(OUTFILE) -std=c23 main.c -Wall -Wextra $(CFLAGS) -O0 -ggdb3
