CC      = cc
CFLAGS  = -std=c17 -Wall -Wextra -Wpedantic -O2 -D_GNU_SOURCE
LDFLAGS =
SRC     = doot.c terminal.c buffer.c
OBJ     = $(SRC:.c=.o)
TARGET  = doot

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c doot.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
