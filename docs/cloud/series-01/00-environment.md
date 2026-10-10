# Establish the executable baseline

Read `CMakeLists.txt`, `Makefile`, `tests/CMakeLists.txt`, and
`docs/distribution.md`. Required build inputs include:

- CMake 4.0 or later, Ninja, and a compiler supporting the project's C++26 mode.
- Qt 6.11 or later: Concurrent, Core, DBus, Gui, Multimedia, Network, Widgets,
  Sql, and Test, with the SQLite driver.
- pkg-config, TagLib, zstd, Chromaprint, GStreamer audio/pbutils, GLib/GObject.
- Python and the test suite's media/tool dependencies. Native PipeWire support
  is optional; report whether it is present and the resulting test coverage.

The hosted Ubuntu environment uses the pinned SDK described in
[environment/README.md](environment/README.md). Configure its Setup script
before starting the task. Record the SDK identity, actual versions, and user
ID. The compiler and runtime commands use `cloud-env`.

Keep build output in the checkout's `build/`, with bounded parallelism such as
`CMAKE_BUILD_PARALLEL_LEVEL=2`. Do not provide embedded service credentials.
Run tests with isolated synthetic state and no host audio/display mounts.
Headless fixtures may own private audio graphs; hardware-dependent omissions
remain omissions, not passes. Use the suite's existing isolation helpers.

Before any source edit, run:

```sh
export MUZAITEN_STATE_ROOT="$PWD/agent-state/cloud"
export CMAKE_BUILD_PARALLEL_LEVEL=2
cloud-env bash docs/cloud/series-01/environment/preflight.sh
cloud-env make build
cloud-test
```

Record the unchanged source revision, command exit codes, warnings/errors,
test totals and skips, and environment identity. If the first setup attempt
cannot establish the baseline, return a precise blocker and proposed setup
recipe. Do not alter minimum versions, suppress failures, copy the local
machine's build, or start product changes on an unverified environment.
