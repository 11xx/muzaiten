.DEFAULT_GOAL := help

-include .env

.PHONY: help configure build rebuild test smoke run dev demo-screens install uninstall clean distclean

BUILD_DIR                  ?= build
CMAKE                      ?= cmake
CTEST                      ?= ctest
CMAKE_GENERATOR            ?= Ninja
# An empty build type means NO optimization flags at all in CMake — dev
# builds and every benchmark run against them were silently -O0 (DSP ~10x
# slower than the shipped Release binary). RelWithDebInfo keeps -O2 with
# debug info; override with CMAKE_BUILD_TYPE=Debug when chasing a bug.
CMAKE_BUILD_TYPE           ?= RelWithDebInfo
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
DEMO_LIBRARY_ARTIST        ?= $(DEMO_ARTIST)
DEMO_LIBRARY_ALBUM         ?= $(DEMO_ALBUM)
DEMO_PLAYLIST_NAME         ?=
DEMO_PLAYLIST_TRACK        ?=
DEMO_FILE_EXPLORER_LIBRARY_PATH  ?=
DEMO_FILE_EXPLORER_LIBRARY_TRACK ?=
DEMO_FILE_EXPLORER_SYSTEM_PATH   ?=
DEMO_FILE_EXPLORER_SYSTEM_TRACK  ?=
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
		'  DEMO_LIBRARY_ARTIST="Rainbow" (DEMO_ARTIST alias)' \
		'  DEMO_LIBRARY_ALBUM="Rising" (DEMO_ALBUM alias)' \
		'  DEMO_PLAYLIST_NAME="Favorites"' \
		'  DEMO_PLAYLIST_TRACK="Stargazer"' \
		'  DEMO_FILE_EXPLORER_LIBRARY_PATH="/path/to/library/album"' \
		'  DEMO_FILE_EXPLORER_LIBRARY_TRACK="01.flac"' \
		'  DEMO_FILE_EXPLORER_SYSTEM_PATH="/path/to/music"' \
		'  DEMO_FILE_EXPLORER_SYSTEM_TRACK="01.flac"' \
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
	@printf '%s\n' "Configuring $(BUILD_DIR) with $(CMAKE_GENERATOR)"
	@$(CMAKE) -S . -B $(BUILD_DIR) -G $(CMAKE_GENERATOR) \
		$(if $(NINJA),-DCMAKE_MAKE_PROGRAM="$(NINJA)") \
		$(if $(CMAKE_BUILD_TYPE),-DCMAKE_BUILD_TYPE="$(CMAKE_BUILD_TYPE)") \
		$(if $(LINKER_TYPE),-DCMAKE_LINKER_TYPE="$(LINKER_TYPE)") \
		-DMUZAITEN_LASTFM_API_KEY="$(MUZAITEN_LASTFM_API_KEY)" \
		-DMUZAITEN_LASTFM_SHARED_SECRET="$(MUZAITEN_LASTFM_SHARED_SECRET)"

# Configure leaves a marker behind when the compiler, Qt, or a packaged
# dependency differs from the recorded fingerprint; the objects may predate
# the corresponding headers, so only a from-scratch recompile is trustworthy.
build: configure
	@if [ -f "$(BUILD_DIR)/toolchain-changed.stamp" ]; then \
		rm -f "$(BUILD_DIR)/toolchain-changed.stamp"; \
		$(CMAKE) --build $(BUILD_DIR) --clean-first; \
	else \
		$(CMAKE) --build $(BUILD_DIR); \
	fi

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

# Captures still PNGs plus an animated PNG (02-search.png, an APNG) for the search
# demo. The runner detects APNG chunks and optimizes only static images, since
# still-image optimizers can flatten animations to their first frame.
demo-screens: build
	@env MUZAITEN_DEMO_THEMES="$(DEMO_THEMES)" \
		DEMO_OPTIMIZE_PNG="$(DEMO_OPTIMIZE_PNG)" DEMO_PNG_LOSSY="$(DEMO_PNG_LOSSY)" \
		DEMO_PNG_QUANTIZER="$(DEMO_PNG_QUANTIZER)" DEMO_PNG_QUANTIZER_FLAGS="$(DEMO_PNG_QUANTIZER_FLAGS)" \
		DEMO_PNG_OPTIMIZER="$(DEMO_PNG_OPTIMIZER)" DEMO_PNG_OPTIMIZER_FLAGS="$(DEMO_PNG_OPTIMIZER_FLAGS)" \
		python3 tools/demo-screens.py --app "$(APP)" --output "$(DEMO_SCREEN_DIR)" \
		--demo-size "$(DEMO_SIZE)" --demo-search "$(DEMO_SEARCH)" \
		$(if $(filter-out 0 false no,$(DEMO_SEARCH_VIDEO)),--demo-search-video) \
		--demo-search-delay-ms "$(DEMO_SEARCH_DELAY_MS)" \
		--demo-library-artist "$(DEMO_LIBRARY_ARTIST)" --demo-library-album "$(DEMO_LIBRARY_ALBUM)" \
		--demo-playlist-name "$(DEMO_PLAYLIST_NAME)" --demo-playlist-track "$(DEMO_PLAYLIST_TRACK)" \
		--demo-file-explorer-library-path "$(DEMO_FILE_EXPLORER_LIBRARY_PATH)" --demo-file-explorer-library-track "$(DEMO_FILE_EXPLORER_LIBRARY_TRACK)" \
		--demo-file-explorer-system-path "$(DEMO_FILE_EXPLORER_SYSTEM_PATH)" --demo-file-explorer-system-track "$(DEMO_FILE_EXPLORER_SYSTEM_TRACK)" \
		--demo-now-playing "$(DEMO_NOW_PLAYING)" \
		--demo-now-playing-state "$(DEMO_NOW_PLAYING_STATE)" \
		--demo-now-playing-position "$(DEMO_NOW_PLAYING_POSITION)"

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
