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
DEMO_STATE_ROOT            ?= $(CURDIR)/agent-state
DEMO_SCREEN_DIR            ?= $(CURDIR)/demo-screens
DEMO_SEARCH                ?=

help:
	@printf '%s\n' \
		'Targets:' \
		'  make configure  Configure the CMake build directory' \
		'  make build      Configure and build the project' \
		'  make test       Run the test suite for the existing build' \
		'  make smoke      Run the existing build offscreen as a startup smoke test' \
		'  make run        Launch the existing build with --verbose (XDG dirs)' \
		'  make dev        Build and launch with isolated ./dev-state (MUZAITEN_DEV_STATE)' \
		'  make demo-screens Capture publishing screenshots from copied data in agent-state/' \
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
		'  DEMO_STATE_ROOT=agent-state' \
		'  DEMO_SCREEN_DIR=demo-screens' \
		'  DEMO_SEARCH="artist:example"' \
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
	@if [ ! -f "$(DEMO_STATE_ROOT)/data/library.sqlite" ]; then \
		printf '%s\n' \
			"Missing copied demo library: $(DEMO_STATE_ROOT)/data/library.sqlite" \
			"Create an isolated demo state first, for example:" \
			"  mkdir -p '$(DEMO_STATE_ROOT)/data' '$(DEMO_STATE_ROOT)/state' '$(DEMO_STATE_ROOT)/cache'" \
			"  cp ~/.local/share/muzaiten/library.sqlite '$(DEMO_STATE_ROOT)/data/'" \
			"  cp ~/.local/share/muzaiten/playlists.sqlite '$(DEMO_STATE_ROOT)/data/' 2>/dev/null || true" \
			"  cp ~/.local/state/muzaiten/state.sqlite '$(DEMO_STATE_ROOT)/state/' 2>/dev/null || cp ~/.local/share/muzaiten/state.sqlite '$(DEMO_STATE_ROOT)/state/' 2>/dev/null || true" \
			"  cp ~/.cache/muzaiten/artwork.sqlite '$(DEMO_STATE_ROOT)/cache/' 2>/dev/null || cp ~/.local/share/muzaiten/artwork.sqlite '$(DEMO_STATE_ROOT)/cache/' 2>/dev/null || true"; \
		exit 1; \
	fi
	env QT_QPA_PLATFORM=$(QT_QPA_PLATFORM) MUZAITEN_STATE_ROOT="$(DEMO_STATE_ROOT)" \
		./$(APP) --demo-screens "$(DEMO_SCREEN_DIR)" $(if $(DEMO_SEARCH),--demo-search "$(DEMO_SEARCH)")

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
