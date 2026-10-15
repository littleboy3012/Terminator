# Makefile for Terminator: Custom Terminal Emulator and Shell
# CS69201 - Computing Lab Term Project

CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -Iinclude
LDFLAGS = -lpthread -lrt

# Project directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
TARGET = terminator

# Source and Object files
SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/shell.c \
       $(SRC_DIR)/parser.c \
       $(SRC_DIR)/builtins.c

OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Quick test rule to verify execution in WSL environment
test: $(TARGET)
	@echo "Testing built-in and external command execution..."
	./$(TARGET) < /dev/null
