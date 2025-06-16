# Compiler and flags
CC = gcc
CFLAGS = -Wall -O2 -std=c11
LDFLAGS = -lncurses

# Project files
TARGET = marquee
SRCS = marquee.c
INSTALL_DIR = /usr/local/bin

# Phony targets are not files
.PHONY: all clean install uninstall

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

# Rule to install the target executable
install: $(TARGET)
	cp $(TARGET) $(INSTALL_DIR)

# Rule to uninstall the target executable
uninstall:
	rm -f $(INSTALL_DIR)/$(TARGET)
