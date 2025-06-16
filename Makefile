# Compiler and flags
CC = gcc
CFLAGS = -Wall -O2 -std=c11
LDFLAGS = -lncurses

# Project files
TARGET = marquee
SRCS = marquee.c

# Phony targets are not files
.PHONY: all clean

# Default target
all: $(TARGET)

# Rule to build the target executable
# $@: The file name of the target (e.g., "marquee").
# $^: The names of all the prerequisites (e.g., "marquee.c").
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Rule to remove generated files
clean:
	rm -f $(TARGET)
