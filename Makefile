# Makefile for Terminator: Custom Terminal Emulator and Shell
# CS69201 - Computing Lab Term Project

CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -Iinclude
# LDFLAGS includes:
# -lX11: Xlib client-side library for window rendering and keyboard events
# -lpthread: POSIX threads support for thread mode
# -lrt: POSIX real-time library for shared memory and semaphores
LDFLAGS = -lX11 -lpthread -lrt

# Project directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
TARGET = terminator

# Source and Object files
SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/shell.c \
       $(SRC_DIR)/parser.c \
       $(SRC_DIR)/builtins.c \
       $(SRC_DIR)/gui.c \
       $(SRC_DIR)/hotkeys.c \
       $(SRC_DIR)/multiwatch.c \
       $(SRC_DIR)/jobs.c \
       $(SRC_DIR)/history.c \
       $(SRC_DIR)/completion.c \
       $(SRC_DIR)/script.c

OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

.PHONY: all clean test

TEST_RUNNER = $(BUILD_DIR)/test_shell_runner

all: $(TARGET)

HARNESS = harness
HARNESS_OBJS = $(filter-out $(BUILD_DIR)/main.o, $(OBJS))

$(HARNESS): harness.c tests/harness_compat.h $(HARNESS_OBJS)
	$(CC) $(CFLAGS) harness.c $(HARNESS_OBJS) -o $(HARNESS) $(LDFLAGS)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

$(TEST_RUNNER): tests/test_shell_runner.c $(BUILD_DIR)/shell.o $(BUILD_DIR)/parser.o $(BUILD_DIR)/builtins.o $(BUILD_DIR)/multiwatch.o $(BUILD_DIR)/jobs.o $(BUILD_DIR)/history.o $(BUILD_DIR)/completion.o $(BUILD_DIR)/script.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

TEST_SCRIPT_RUNNER = $(BUILD_DIR)/test_task11_scripts

$(TEST_SCRIPT_RUNNER): tests/test_task11_scripts.c $(BUILD_DIR)/shell.o $(BUILD_DIR)/parser.o $(BUILD_DIR)/builtins.o $(BUILD_DIR)/multiwatch.o $(BUILD_DIR)/jobs.o $(BUILD_DIR)/history.o $(BUILD_DIR)/completion.o $(BUILD_DIR)/script.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(HARNESS)

test: $(TARGET) $(TEST_RUNNER) $(TEST_SCRIPT_RUNNER)
	./$(TEST_SCRIPT_RUNNER)
	python3 tests/test_modes_and_history.py
