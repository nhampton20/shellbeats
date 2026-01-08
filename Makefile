CC=gcc
CFLAGS=-O2 -Wall -Wextra -std=c11
LDLIBS=-lncursesw

all: shellbeats

shellbeats: shellbeats.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f shellbeats