# Report local build identity from both CLI binaries

`muzaitenctl --version` currently reaches ordinary command/IPC handling.
`muzaiten-features --version` reaches unknown-command handling. Both binaries
should report the same build identity source as the application, locally.

Inspect `src/cli/MuzaitenCtl.cpp`, `src/indexer/FeaturesMain.cpp`,
`src/app/main.cpp`, `src/core/Version.h.in`, `cmake/MuzaitenVersion.cmake`,
`tools/version.py`, and the CLI target definitions in `CMakeLists.txt`.

Implement `--version` before socket connection, configuration/database access,
provider discovery, or normal command dispatch. Use the existing generated
version source rather than duplicating date/commit logic. Preserve every
ordinary command, option, error, and exit code. Follow the application's
established version-output convention.

Verification must execute both built CLIs with no running application and
private absent state/runtime roots, prove exit 0 and the expected build
identity, and prove that the command creates no application state or IPC
connection. Check normal help/unknown-command behavior for regression. Reuse
or extend `tests/test_version.py` and the relevant CLI fixtures; cover the
installed-binary interface without installing into an account's live prefix.

Allowed changes: these CLI entry points, generated-header wiring, focused
version/CLI tests, `docs/cli.md`, affected distribution guidance, and
`CHANGELOG.md`. Do not change release versions, create a release/tag, or use
IPC to ask another process for its version. Run full build/test gates.
