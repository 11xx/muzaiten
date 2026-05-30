.DEFAULT_GOAL := help

-include .env

.PHONY: help configure build rebuild test smoke run dev clean distclean

BUILD_DIR                  ?= build
CMAKE                      ?= cmake
CTEST                      ?= ctest
CMAKE_GENERATOR            ?= Ninja
QT_QPA_PLATFORM            ?= offscreen
MUZAITEN_LASTFM_API_KEY    ?=
MUZAITEN_LASTFM_SHARED_SECRET ?=
APP := $(BUILD_DIR)/muzaiten

help:
	@printf '%s\n' \
		'Targets:' \
		'  make configure  Configure the CMake build directory' \
		'  make build      Configure and build the project' \
		'  make test       Build and run the test suite' \
		'  make smoke      Build and run the offscreen startup smoke test' \
		'  make run        Build and launch the app with --verbose (XDG dirs)' \
		'  make dev        Build and launch with isolated ./dev-state (MUZAITEN_DEV_STATE)' \
		'  make clean      Remove build outputs from $(BUILD_DIR)' \
		'  make distclean  Alias for clean' \
		'' \
		'Variables (override on command line or in .env):' \
		'  BUILD_DIR=build-archlinux' \
		'  CMAKE_GENERATOR=Ninja' \
		'  MUZAITEN_LASTFM_API_KEY=...' \
		'  MUZAITEN_LASTFM_SHARED_SECRET=...'

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -G $(CMAKE_GENERATOR) \
		-DMUZAITEN_LASTFM_API_KEY="$(MUZAITEN_LASTFM_API_KEY)" \
		-DMUZAITEN_LASTFM_SHARED_SECRET="$(MUZAITEN_LASTFM_SHARED_SECRET)"

build: configure
	$(CMAKE) --build $(BUILD_DIR)

rebuild: clean build

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

smoke: build
	timeout 2s env QT_QPA_PLATFORM=$(QT_QPA_PLATFORM) ./$(APP); \
	status=$$?; \
	if [ $$status -ne 0 ] && [ $$status -ne 124 ]; then \
		exit $$status; \
	fi

run: build
	./$(APP) --verbose

# Dev launcher: MUZAITEN_DEV_STATE points all dirs at ./dev-state (CWD-relative),
# so data/state/cache are isolated in the repo and shared across every build dir.
dev: build
	MUZAITEN_DEV_STATE=1 ./$(APP) --verbose

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
