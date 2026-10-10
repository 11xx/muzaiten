# SDK baseline pilot report

Scope: environment baseline only. Tasks 1-4 were not started. No source edits were made. Independent review and local acceptance have not been performed.

Source head: `6fd3362015c7467efa1c2b1ca549b12a65263947`

## Outcome

Task 0 (environment baseline): **blocked at the test step**.

- Source identity, packet checksums, preflight, and the build passed.
- `cloud-test` did not run any tests. It stopped with the precondition message below, so there are no UID 1000 test totals.

## Checks

| Step | Command (wrapper preserved) | Observed exit | Result |
| --- | --- | --- | --- |
| Source identity | `git rev-parse HEAD; id` | 0 | HEAD matches the source above. Current UID is 0 (root). |
| Packet checksums | `sha256sum -c docs/cloud/series-01/SHA256SUMS` | 0 | All entries OK. |
| SDK preflight | `cloud-env bash docs/cloud/series-01/environment/preflight.sh` | 0 | Compiler GCC 16.2.0; CMake 4.4.3; Qt 6.11.2 (Core, Gui, Widgets, Sql, Multimedia, Network, DBus, Concurrent, Test); taglib, libzstd, libchromaprint, GStreamer 1.28.7 and its pbutils/audio components, and glib present. `function_ref` and `qt_compile_link` verified. |
| Build | `cloud-env make build` | 0 (echoed by the wrapper) | 986 steps in the build tree; the final logged step line was 985/986. Compiler warnings: 0. Error-matching lines: 0. |
| Test | `cloud-test` | 1 (echoed by the wrapper) | Stopped before any tests ran. Message: "Test directories are not prepared. Configure the cloud environment Setup script from the checkout." |

The background-task harness reported exit code 0 for the test-wrapper run. The wrapper's own echoed exit value (1) is recorded above because it matches the log.

## Diagnostics kept separate from warnings

CMake configure printed four `pkg-config` diagnostics: "Package libunwind was not found in the pkg-config search path." (four occurrences). These are configure-time package notices, not compiler warnings. They are recorded here separately from the compiler-warning total of 0.

## Test directories and permissions

`build/` and `build/tests/` are owned by root, and `build/Testing/` was present with root ownership. `agent-state/cloud-tests/` and its `runtime/` subdirectory did not exist. The Setup-script step that prepares these for the existing Ubuntu account (UID 1000) was not run from the checkout root.

A repair to these generated directories (ownership to UID 1000 and mode changes) was proposed by a peer session. It was not performed: ownership and permission changes require the user's approval, and this pilot's brief excludes them. The test step is therefore blocked until that preparation is done by an authorized step, or the Setup script is run from the checkout.

## Not done

- Tasks 1-4.
- Any UID 1000 test run and its totals or skips (no tests ran).
- Any source edit, account or permission change, or provisioning in the agent session.
- Pull request.
