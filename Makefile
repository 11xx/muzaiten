.DEFAULT_GOAL := help

-include .env

.PHONY: help configure build rebuild test smoke run dev demo-screens install uninstall clean distclean

BUILD_DIR                  ?= build
CMAKE                      ?= cmake
CTEST                      ?= ctest
CMAKE_GENERATOR            ?= Ninja
CMAKE_BUILD_TYPE           ?=
# User-space by default: a plain `make install` lands in ~/.local (no sudo).
# Override for a system install: `make install PREFIX=/usr` (run with sudo).
PREFIX                     ?= $(HOME)/.local
# Ninja is "ninja" on Arch/Debian and "ninja-build" on Fedora; detect either.
NINJA                      ?= $(shell command -v ninja 2>/dev/null || command -v ninja-build 2>/dev/null)
# Fast linker, auto-detected and a no-op when absent: the relink after every edit
# is the bulk of an incremental build, and mold/lld cut it ~5x. (A compiler cache
# like ccache/sccache buys little here — the C++-modules build makes objects
# non-cacheable — so it's intentionally not wired in.) Override: `LINKER_TYPE=`.
LINKER_TYPE                ?= $(if $(shell command -v mold 2>/dev/null),MOLD,$(if $(shell command -v ld.lld 2>/dev/null),LLD,))
QT_QPA_PLATFORM            ?= offscreen
MUZAITEN_LASTFM_API_KEY    ?=
MUZAITEN_LASTFM_SHARED_SECRET ?=
APP := $(BUILD_DIR)/muzaiten
DEMO_SCREEN_DIR            ?= $(CURDIR)/demo-screens
DEMO_THEMES                ?= light dark
DEMO_SIZE                  ?= 1440x900
DEMO_SEARCH                ?=
DEMO_SEARCH_VIDEO          ?= 1
DEMO_SEARCH_DELAY_MS       ?= 120
DEMO_ARTIST                ?=
DEMO_ALBUM                 ?=
DEMO_NOW_PLAYING           ?=
DEMO_NOW_PLAYING_STATE     ?= paused
DEMO_NOW_PLAYING_POSITION  ?= 0.6667
DEMO_OPTIMIZE_PNG          ?= 1
DEMO_PNG_LOSSY             ?= 1
DEMO_PNG_QUANTIZER         ?= pngquant
DEMO_PNG_QUANTIZER_FLAGS   ?= --force --skip-if-larger --ext .png --strip --speed 1 --quality 0-90
DEMO_PNG_OPTIMIZER         ?= oxipng
DEMO_PNG_OPTIMIZER_FLAGS   ?= -o 4 --strip safe

help:
	@printf '%s\n' \
		'Targets:' \
		'  make configure  Configure the CMake build directory' \
		'  make build      Configure and build the project' \
		'  make test       Run the test suite for the existing build' \
		'  make smoke      Run the existing build offscreen as a startup smoke test' \
		'  make run        Launch the existing build with --verbose (XDG dirs)' \
		'  make dev        Build and launch with isolated ./dev-state (MUZAITEN_DEV_STATE)' \
		'  make demo-screens Capture publishing screenshots/video from a temp XDG data copy' \
		'  make install    Install the existing build (user-space ~/.local by default)' \
		'  make uninstall  Remove a prior install (reads $(BUILD_DIR)/install_manifest.txt)' \
		'  make clean      Remove build outputs from $(BUILD_DIR)' \
		'  make distclean  Alias for clean' \
		'' \
		'Variables (override on command line or in .env):' \
		'  BUILD_DIR=build-archlinux' \
		'  CMAKE_GENERATOR=Ninja' \
		'  CMAKE_BUILD_TYPE=Release' \
		'  PREFIX=/usr            (install prefix; default ~/.local, no sudo)' \
		'  DEMO_SCREEN_DIR=demo-screens' \
		'  DEMO_THEMES="light dark"  (space- or comma-separated)' \
		'  DEMO_SIZE=1440x900' \
		'  DEMO_SEARCH="artist:example"' \
		'  DEMO_SEARCH_VIDEO=1' \
		'  DEMO_SEARCH_DELAY_MS=120' \
		'  DEMO_ARTIST="Rainbow"' \
		'  DEMO_ALBUM="Rising"' \
		'  DEMO_NOW_PLAYING="stargazer rainbow"' \
		'  DEMO_NOW_PLAYING_STATE=paused' \
		'  DEMO_NOW_PLAYING_POSITION=0.6667' \
		'  DEMO_OPTIMIZE_PNG=1' \
		'  DEMO_PNG_LOSSY=1' \
		'  DEMO_PNG_QUANTIZER=pngquant' \
		'  DEMO_PNG_QUANTIZER_FLAGS="--force --skip-if-larger --ext .png --strip --speed 1 --quality 0-90"' \
		'  DEMO_PNG_OPTIMIZER=oxipng' \
		'  DEMO_PNG_OPTIMIZER_FLAGS="-o 4 --strip safe"' \
		'  MUZAITEN_LASTFM_API_KEY=...' \
		'  MUZAITEN_LASTFM_SHARED_SECRET=...'

configure:
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		prog=$$(sed -n 's/^CMAKE_MAKE_PROGRAM:[^=]*=//p' "$(BUILD_DIR)/CMakeCache.txt"); \
		if [ -n "$$prog" ] && [ ! -x "$$prog" ]; then \
			echo "Stale CMake cache (build program '$$prog' not found here); reconfiguring from scratch."; \
			rm -rf "$(BUILD_DIR)/CMakeCache.txt" "$(BUILD_DIR)/CMakeFiles"; \
		fi; \
	fi
	$(CMAKE) -S . -B $(BUILD_DIR) -G $(CMAKE_GENERATOR) \
		$(if $(NINJA),-DCMAKE_MAKE_PROGRAM="$(NINJA)") \
		$(if $(CMAKE_BUILD_TYPE),-DCMAKE_BUILD_TYPE="$(CMAKE_BUILD_TYPE)") \
		$(if $(LINKER_TYPE),-DCMAKE_LINKER_TYPE="$(LINKER_TYPE)") \
		-DMUZAITEN_LASTFM_API_KEY="$(MUZAITEN_LASTFM_API_KEY)" \
		-DMUZAITEN_LASTFM_SHARED_SECRET="$(MUZAITEN_LASTFM_SHARED_SECRET)"

build: configure
	$(CMAKE) --build $(BUILD_DIR)

rebuild: clean build

test:
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

smoke:
	timeout 2s env QT_QPA_PLATFORM=$(QT_QPA_PLATFORM) ./$(APP); \
	status=$$?; \
	if [ $$status -ne 0 ] && [ $$status -ne 124 ]; then \
		exit $$status; \
	fi

run:
	./$(APP) --verbose

# Dev launcher: MUZAITEN_DEV_STATE points all dirs at ./dev-state (CWD-relative),
# so data/state/cache are isolated in the repo and shared across every build dir.
dev: build
	MUZAITEN_DEV_STATE=1 ./$(APP) --verbose

demo-screens: build
	@set -eu; \
	data_home="$${XDG_DATA_HOME:-$$HOME/.local/share}"; \
	state_home="$${XDG_STATE_HOME:-$$HOME/.local/state}"; \
	cache_home="$${XDG_CACHE_HOME:-$$HOME/.cache}"; \
	library="$$data_home/muzaiten/library.sqlite"; \
	if [ ! -f "$$library" ]; then \
		printf '%s\n' \
			"Could not find $$library" \
			"make demo-screens only auto-copies from XDG data/state/cache homes." \
			"Run the demo manually with an explicit isolated state root instead:" \
			"  mkdir -p /tmp/muzdir/data /tmp/muzdir/state /tmp/muzdir/cache" \
			"  cp /path/to/library.sqlite /tmp/muzdir/data/" \
			"  env QT_QPA_PLATFORM=offscreen MUZAITEN_STATE_ROOT=/tmp/muzdir ./$(APP) --demo-screens '$(DEMO_SCREEN_DIR)'"; \
		exit 1; \
	fi; \
	tmp_state=$$(mktemp -d "$${TMPDIR:-/tmp}/muzaiten-demo-state.XXXXXX"); \
	trap 'rm -rf "$$tmp_state"' EXIT INT TERM; \
	mkdir -p "$$tmp_state/data" "$$tmp_state/state" "$$tmp_state/cache"; \
	cp "$$library" "$$tmp_state/data/"; \
	if [ -f "$$data_home/muzaiten/playlists.sqlite" ]; then cp "$$data_home/muzaiten/playlists.sqlite" "$$tmp_state/data/"; fi; \
	if [ -f "$$data_home/muzaiten/history.sqlite" ]; then cp "$$data_home/muzaiten/history.sqlite" "$$tmp_state/data/"; fi; \
	if [ -f "$$state_home/muzaiten/state.sqlite" ]; then cp "$$state_home/muzaiten/state.sqlite" "$$tmp_state/state/"; fi; \
	if [ -f "$$cache_home/muzaiten/artwork.sqlite" ]; then cp "$$cache_home/muzaiten/artwork.sqlite" "$$tmp_state/cache/"; fi; \
	mkdir -p "$(DEMO_SCREEN_DIR)"; \
	env "QT_QPA_PLATFORM=offscreen" "MUZAITEN_STATE_ROOT=$$tmp_state" "MUZAITEN_DEMO_THEMES=$(DEMO_THEMES)" \
		./$(APP) --demo-screens "$(DEMO_SCREEN_DIR)" \
		--demo-size "$(DEMO_SIZE)" \
		$(if $(DEMO_SEARCH),--demo-search "$(DEMO_SEARCH)") \
		$(if $(filter-out 0 false no,$(DEMO_SEARCH_VIDEO)),--demo-search-video) \
		--demo-search-delay-ms "$(DEMO_SEARCH_DELAY_MS)" \
		$(if $(DEMO_ARTIST),--demo-artist "$(DEMO_ARTIST)") \
		$(if $(DEMO_ALBUM),--demo-album "$(DEMO_ALBUM)") \
		$(if $(DEMO_NOW_PLAYING),--demo-now-playing "$(DEMO_NOW_PLAYING)") \
		--demo-now-playing-state "$(DEMO_NOW_PLAYING_STATE)" \
		--demo-now-playing-position "$(DEMO_NOW_PLAYING_POSITION)"; \
		if [ "$(DEMO_OPTIMIZE_PNG)" != "0" ] && [ "$(DEMO_OPTIMIZE_PNG)" != "false" ] && [ "$(DEMO_OPTIMIZE_PNG)" != "no" ]; then \
			if [ "$(DEMO_PNG_LOSSY)" != "0" ] && [ "$(DEMO_PNG_LOSSY)" != "false" ] && [ "$(DEMO_PNG_LOSSY)" != "no" ]; then \
				if command -v "$(DEMO_PNG_QUANTIZER)" >/dev/null 2>&1; then \
					find "$(DEMO_SCREEN_DIR)" -type f -name '*.png' -print0 \
						| xargs -0 -r "$(DEMO_PNG_QUANTIZER)" $(DEMO_PNG_QUANTIZER_FLAGS) --; \
				else \
					printf '%s\n' "warning: $(DEMO_PNG_QUANTIZER) not found; skipping lossy demo PNG optimization" >&2; \
				fi; \
			fi; \
			if command -v "$(DEMO_PNG_OPTIMIZER)" >/dev/null 2>&1; then \
				find "$(DEMO_SCREEN_DIR)" -type f -name '*.png' -print0 \
					| xargs -0 -r "$(DEMO_PNG_OPTIMIZER)" $(DEMO_PNG_OPTIMIZER_FLAGS); \
			else \
				printf '%s\n' "warning: $(DEMO_PNG_OPTIMIZER) not found; leaving demo PNGs unoptimized" >&2; \
			fi; \
		fi

# Installs the existing build. Run `make build` (optionally with
# CMAKE_BUILD_TYPE=Release) first. Defaults to the user-space ~/.local prefix
# (no sudo); pass PREFIX=/usr (with sudo) for a system-wide install.
install:
	$(CMAKE) --install $(BUILD_DIR) $(if $(PREFIX),--prefix "$(PREFIX)")

# Reverses the last `make install` from this build dir by deleting the files it
# recorded in install_manifest.txt. Prefix-agnostic: it cleans whatever prefix
# that install used, so `make uninstall` undoes `make install` regardless of the
# PREFIX you chose. Use sudo if you installed to a system prefix like /usr.
uninstall:
	@if [ ! -f "$(BUILD_DIR)/cmake_uninstall.cmake" ]; then \
		echo "No configured build in '$(BUILD_DIR)'; run 'make build' first."; exit 1; \
	fi
	$(CMAKE) -P "$(BUILD_DIR)/cmake_uninstall.cmake"

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
