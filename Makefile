# ShellBeats Makefile
CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lncurses

TARGET = shellbeats
SRC = shellbeats.c

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

debug: $(SRC)
	$(CC) $(CFLAGS) -g -DDEBUG -o $(TARGET) $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET)
