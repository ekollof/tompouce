CC = gcc
CFLAGS = -O2 -Wall -Wextra -Iinclude -Ithirdparty -pthread
LDFLAGS = -pthread
LIBS = $(shell pkg-config --libs slang portaudio-2.0 libcurl libcjson alsa 2>/dev/null) -lm -lleveldb

SRC = src/common.c src/audio.c src/pattern.c src/harmony.c src/track.c src/scheduler.c src/ai.c src/tui.c src/mcp.c src/wavetable.c src/banks.c src/sampler.c src/midi.c src/songfile.c src/main.c
OBJ = $(SRC:.c=.o)
TARGET = strudelterm

all: $(TARGET)
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(OBJ) $(TARGET) tests/test_pattern tests/test_harmony
test: $(TARGET) tests/test_pattern tests/test_harmony
	./tests/test_pattern
	./tests/test_harmony
tests/test_pattern: tests/test_pattern.c src/pattern.o src/common.o
	$(CC) $(CFLAGS) -o $@ tests/test_pattern.c src/pattern.o src/common.o -lm -pthread
tests/test_harmony: tests/test_harmony.c src/harmony.o
	$(CC) $(CFLAGS) -o $@ tests/test_harmony.c src/harmony.o -lm
.PHONY: all clean test