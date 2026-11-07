CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11

bytebeat: bytebeat.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f bytebeat

.PHONY: clean
