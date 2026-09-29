CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=gnu11
TARGET  := runtime

all: $(TARGET)

$(TARGET): runtime.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean