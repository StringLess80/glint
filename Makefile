# Glint
#   make         build ./glint (the first build downloads UxPlay's AirPlay library)
#   make test    run the tests (needs the ffmpeg command-line tool)
#   make clean   remove build output

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra
BUILD   := build

UXPLAY_TAG := v1.73.7
UXPLAY_URL := https://github.com/FDH2/UxPlay.git
UXPLAY_DIR := third_party/uxplay
UXLIB      := $(UXPLAY_DIR)/lib

PKGS := libavcodec libavutil libswscale sdl2 libplist-2.0 libcrypto avahi-compat-libdns_sd
OBJS := $(BUILD)/main.o $(BUILD)/viewer.o $(BUILD)/airplay.o
HDRS := src/mirror.h src/viewer.h

# Dependency check (skipped for "make clean")
ifeq ($(filter clean distclean,$(MAKECMDGOALS)),)
MISSING := $(strip $(foreach p,$(PKGS),$(if $(shell pkg-config --exists $(p) && echo ok),,$(p))))
ifneq ($(MISSING),)
$(error Missing development packages for: $(MISSING) (see README, "Install dependencies"))
endif
endif

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS)) -lpthread -lm

# Flags for UxPlay's library (same as its CMakeLists.txt)
PLIST_DEFS := $(shell pkg-config --atleast-version=2.1.0 libplist-2.0 2>/dev/null && echo -DPLIST_210) \
              $(shell pkg-config --atleast-version=2.3.0 libplist-2.0 2>/dev/null && echo -DPLIST_230)
UX_CFLAGS  := -O2 -g -w -fPIC -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 \
              -DSTANDALONE -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX -D_LINUX \
              -DNOHOLD -DOPENSSL_API_COMPAT=0x10101000L $(PLIST_DEFS) \
              -I$(UXLIB) -I$(UXLIB)/playfair -I$(UXLIB)/llhttp $(PKG_CFLAGS)

all: glint

glint: $(OBJS) $(BUILD)/libairplay.a
	$(CC) -o $@ $^ $(PKG_LIBS)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(PKG_CFLAGS) -c -o $@ $<

$(BUILD)/airplay.o: src/airplay.c $(HDRS) $(UXPLAY_DIR)/.fetched | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(PKG_CFLAGS) -I$(UXLIB) -c -o $@ $<

# Download UxPlay's AirPlay protocol library (once)
$(UXPLAY_DIR)/.fetched:
	@echo "Downloading UxPlay $(UXPLAY_TAG) (AirPlay protocol library)..."
	rm -rf $(UXPLAY_DIR)
	git -c advice.detachedHead=false clone --quiet --depth 1 --branch $(UXPLAY_TAG) $(UXPLAY_URL) $(UXPLAY_DIR)
	touch $@

$(BUILD)/libairplay.a: $(UXPLAY_DIR)/.fetched | $(BUILD)
	@echo "Building the AirPlay library..."
	@mkdir -p $(BUILD)/airplay
	@set -e; for f in $(UXLIB)/*.c $(UXLIB)/playfair/*.c $(UXLIB)/llhttp/*.c; do \
	    $(CC) $(UX_CFLAGS) -c $$f -o $(BUILD)/airplay/$$(echo $$f | tr / _).o; \
	done
	ar rcs $@ $(BUILD)/airplay/*.o

# ---- tests -----------------------------------------------------------------
# They run without an iPad and without opening real windows (SDL "dummy" drivers).

TEST_ENV := SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

$(BUILD)/test_colors: tests/test_colors.c $(BUILD)/viewer.o $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(PKG_CFLAGS) -Isrc -o $@ $< $(BUILD)/viewer.o $(PKG_LIBS)

$(BUILD)/test_airplay: tests/test_airplay.c src/airplay.c $(BUILD)/viewer.o $(BUILD)/libairplay.a $(HDRS)
	$(CC) $(CFLAGS) $(WARN) -Wno-unused-function $(PKG_CFLAGS) -Isrc -I$(UXLIB) -o $@ $< \
	    $(BUILD)/viewer.o $(BUILD)/libairplay.a $(PKG_LIBS)

test: $(BUILD)/test_colors $(BUILD)/test_airplay
	sh tests/make_fixtures.sh $(CURDIR)/$(BUILD)/test_colors $(BUILD)/testdata
	cd $(BUILD)/testdata && $(TEST_ENV) ../test_colors full709.h264 lim709.h264 lim601.h264
	cd $(BUILD)/testdata && $(TEST_ENV) ../test_airplay

clean:
	rm -rf $(BUILD) glint

distclean: clean
	rm -rf third_party

.PHONY: all test clean distclean
