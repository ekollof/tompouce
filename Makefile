CC = gcc
CFLAGS = -O2 -Wall -Wextra -Iinclude -Ithirdparty -pthread -Werror -pedantic
CFLAGS += -isystem thirdparty/quickjs -MMD -MP
LDFLAGS = -pthread
LIBS = $(shell pkg-config --libs slang portaudio-2.0 libcurl libcjson alsa tree-sitter 2>/dev/null) -lm -lleveldb
ifneq ($(wildcard $(TREE_SITTER_PREFIX)/include/tree_sitter/api.h),)
CFLAGS += -I$(TREE_SITTER_PREFIX)/include
LIBS += -L$(TREE_SITTER_PREFIX)/lib -Wl,-rpath,$(TREE_SITTER_PREFIX)/lib
endif
CSOUND_PREFIX ?= $(if $(wildcard $(HOME)/.local/opt/tompouce-csound7/include/csound/csound.h),$(HOME)/.local/opt/tompouce-csound7,$(if $(wildcard $(HOME)/.local/opt/strudelterm-csound7/include/csound/csound.h),$(HOME)/.local/opt/strudelterm-csound7,$(HOME)/.local/opt/tompouce-csound7))
ifneq ($(wildcard $(CSOUND_PREFIX)/include/csound/csound.h),)
CFLAGS += -DHAVE_CSOUND -DHAVE_CSOUND7 -isystem $(CSOUND_PREFIX)/include -DCSOUND7_OPCODEDIR='"$(CSOUND_PREFIX)/plugins"'
LIBS += -L$(CSOUND_PREFIX)/lib -Wl,-rpath,$(CSOUND_PREFIX)/lib -lcsound64
else ifneq ($(wildcard /usr/include/csound/csound.h),)
CFLAGS += -DHAVE_CSOUND
LIBS += -lcsound64
endif

SRC = src/common.c src/audio.c src/pattern.c src/harmony.c src/slider.c src/strudel.c src/xform.c src/track.c src/scheduler.c src/ai.c src/tui.c src/mcp.c src/wavetable.c src/banks.c src/sampler.c src/websamp.c src/midi.c src/songfile.c src/main.c
SRC += src/javascript.c src/js_audio.c src/dsp.c src/csound_host.c
SRC += src/convolution.c
SRC += src/companion.c
SRC += src/sounds.c src/sound_store.c
SRC += src/terminal_graphics.c
SRC += src/visual.c
SRC += src/visual_render.c
QJS_OBJ = $(addprefix thirdparty/quickjs/,quickjs.o cutils.o libregexp.o libunicode.o dtoa.o)
TSJS_OBJ = thirdparty/tree-sitter-javascript/src/parser.o thirdparty/tree-sitter-javascript/src/scanner.o
OBJ = $(SRC:.c=.o) $(QJS_OBJ) $(TSJS_OBJ)
TARGET = tompouce

all: $(TARGET)
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
thirdparty/quickjs/%.o: thirdparty/quickjs/%.c
	$(CC) -O2 -w -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION='"2026-06-04"' -c $< -o $@
thirdparty/tree-sitter-javascript/src/%.o: thirdparty/tree-sitter-javascript/src/%.c
	$(CC) -O2 -w -fwrapv -std=c11 -Ithirdparty/tree-sitter-javascript/src -c $< -o $@
src/javascript.o: js/dist/runtime.inc
src/csound_host.o: thirdparty/strudel-csound/presets.inc
thirdparty/strudel-csound/presets.inc: thirdparty/strudel-csound/presets.orc
	xxd -i -n strudel_csound_presets $< > $@
js/dist/runtime.inc: js/dist/runtime.js
	xxd -i -n strudel_js_bundle $< > $@
js-bundle:
	cd js && npm ci --ignore-scripts --no-audit --no-fund && npm run build
companion-bundle:
	cd web && npm ci --ignore-scripts --no-audit --no-fund && npm run build
clean:
	rm -f $(OBJ) $(TARGET) tests/test_pattern tests/test_harmony tests/test_strudel tests/test_websamp
	rm -f tests/test_editor tests/test_terminal_render tests/test_install_runtime
	rm -f tests/test_terminal_graphics
	rm -f js/dist/runtime.inc tests/test_javascript
	rm -f thirdparty/strudel-csound/presets.inc
	rm -f tests/render_audio
	rm -f $(SRC:.c=.d) tests/*.d tests/test_dsp tests/test_examples tests/test_csound tests/test_audio
test: $(TARGET) tests/test_pattern tests/test_harmony tests/test_strudel tests/test_websamp tests/test_editor tests/test_terminal_render tests/test_javascript tests/test_dsp tests/test_examples tests/test_csound tests/test_audio tests/test_terminal_graphics
	./tests/test_pattern
	./tests/test_harmony
	./tests/test_strudel
	./tests/test_websamp
	./tests/test_editor
	TERM=xterm-256color ./tests/test_terminal_render
	./tests/test_terminal_graphics
	./tests/test_javascript
	./tests/test_dsp
	./tests/test_examples
	./tests/test_csound
	./tests/test_audio
tests/test_audio: tests/test_audio.c src/audio.c $(filter-out src/main.o src/audio.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ tests/test_audio.c $(filter-out src/main.o src/audio.o,$(OBJ)) $(LDFLAGS) $(LIBS)
tests/render_audio: tests/render_audio.c src/audio.c src/js_audio.c $(filter-out src/main.o src/audio.o src/js_audio.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ tests/render_audio.c $(filter-out src/main.o src/audio.o src/js_audio.o,$(OBJ)) $(LDFLAGS) $(LIBS)
tests/test_csound: tests/test_csound.c $(filter-out src/main.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
test-examples: tests/test_examples
	./tests/test_examples
compare-audio: tests/render_audio
	node scripts/compare-audio.mjs
tests/test_examples: tests/test_examples.c $(filter-out src/main.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
tests/test_dsp: tests/test_dsp.c $(filter-out src/main.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
tests/test_javascript: tests/test_javascript.c $(filter-out src/main.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
tests/test_editor: tests/test_editor.c src/tui.c src/tui_vi.h $(filter-out src/main.o src/tui.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ tests/test_editor.c $(filter-out src/main.o src/tui.o,$(OBJ)) $(LDFLAGS) $(LIBS)
tests/test_terminal_render: tests/test_terminal_render.c src/tui.c src/tui_vi.h $(filter-out src/main.o src/tui.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ tests/test_terminal_render.c $(filter-out src/main.o src/tui.o,$(OBJ)) $(LDFLAGS) $(LIBS)
tests/test_install_runtime: tests/test_install_runtime.c $(filter-out src/main.o,$(OBJ))
	$(CC) $(CFLAGS) -o $@ tests/test_install_runtime.c $(filter-out src/main.o,$(OBJ)) $(LDFLAGS) $(LIBS)
tests/test_terminal_graphics: tests/test_terminal_graphics.c src/terminal_graphics.o src/visual.o src/visual_render.o
	$(CC) $(CFLAGS) -o $@ $^ -lm -lcjson
tests/test_pattern: tests/test_pattern.c src/pattern.o src/common.o
	$(CC) $(CFLAGS) -o $@ tests/test_pattern.c src/pattern.o src/common.o -lm -pthread
tests/test_harmony: tests/test_harmony.c src/harmony.o
	$(CC) $(CFLAGS) -o $@ tests/test_harmony.c src/harmony.o -lm
tests/test_strudel: tests/test_strudel.c src/strudel.o src/pattern.o src/slider.o src/common.o src/xform.o src/harmony.o
	$(CC) $(CFLAGS) -o $@ tests/test_strudel.c src/strudel.o src/pattern.o src/slider.o src/common.o src/xform.o src/harmony.o -lm -pthread
tests/test_websamp: tests/test_websamp.c tests/fixtures/mp3-gapless.h src/websamp.o src/sound_store.o src/sampler.o src/banks.o src/common.o
	$(CC) $(CFLAGS) -o $@ tests/test_websamp.c src/websamp.o src/sound_store.o src/sampler.o src/banks.o src/common.o $(shell pkg-config --libs libcurl libcjson 2>/dev/null) -lleveldb -lm -pthread
.PHONY: all clean test js-bundle companion-bundle test-examples compare-audio
-include $(SRC:.c=.d)
