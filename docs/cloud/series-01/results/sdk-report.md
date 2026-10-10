# SDK environment baseline pilot (sanitized)

Status: the original pilot was **blocked at the test step** (witness preserved below). A separate **recovery checkpoint** then ran the test suite: 86 of 86 CTest tests passed. Tasks 1-4 were not started.

## Coordinates

- Source head: `702ab0c7ef0822f5708775cd7e49d04e41460725`
- Packet commit: `702ab0c7ef0822f5708775cd7e49d04e41460725` (pilot source, as instructed)
- Result branch: `claude/muzaiten-final-sdk-baseline-fqw0m5`
- Report commit: follows `source_head`; no source files were edited.
- Operator UID for build and preflight: 0 (root, via `cloud-env`). Test UID: 1000 (via `cloud-test`).

## Checks

| Step | Command | Exit | Result |
| --- | --- | --- | --- |
| 1 | `git rev-parse HEAD` | 0 | `702ab0c7…` matches source |
| 1 | `sha256sum -c docs/cloud/series-01/SHA256SUMS` | 0 | 20 of 20 entries OK |
| 1 | `stat -c "%a %u %g" build build/tests build/Testing agent-state/cloud-tests/runtime` | n/a | `755 1000 1000` for build, build/tests, build/Testing; `700 1000 1000` for runtime |
| 2 | `cloud-env bash docs/cloud/series-01/environment/preflight.sh` | 0 | Compiler GCC 16.2.0; CMake 4.4.3; Qt 6.11.2 (all required components); TagLib 2.2.1; libzstd 1.5.7; libchromaprint 1.6.1; GStreamer 1.28.7; GLib 2.88.3; `function_ref`, Qt compile/link, and native audio tools verified; stderr empty |
| 3 | `cloud-env make build` | 0 | 986 ninja steps; 0 lines matching `warning`; 0 lines matching `error` |
| 4 | `cloud-test` | **2** | Failed before any test ran. CTest could not create `build/Testing/Temporary/LastTest.log`: Permission denied |

## Blocker

`build/Testing/Temporary` is owned by `root:root` (mode 755, created 07:12 during the root-run configure step). The `cloud-test` wrapper runs CTest as UID 1000, which cannot write into it. The parent `build/Testing` is correctly owned by 1000:1000, so setup provisioned the parent but not this subdirectory, or the root-run configure recreated it.

Per the packet's rules, ownership was not changed and no workaround was applied. The fix belongs in setup or root provisioning: create `build/Testing/Temporary` owned by 1000:1000 before `cloud-test`, or run configure so it does not create root-owned test state.

## Test coverage

- CTest totals: **not observed**. No test ran, and `LastTest.log` was not produced.
- Within-test SKIP lines: **not observed**. A separate local run reportedly skipped `SharedOutputRoutingTest::initTestCase` (FHS-only `pipewire.conf` lookup). That is not evidence from this run and is not counted here.
- Warnings and errors from the build: 0 and 0.

## Limits

- The 30-minute pilot limit (07:42Z) was reached after the build, leaving no time to fix provisioning and retry.
- No product tasks were started. Tasks 1-4 are not-started.
- No independent review or local acceptance is claimed.

## Recovery checkpoint (separate from the original pilot)

The original pilot's `cloud-test` failure above is kept as the witness. The recovery checkpoint reused the existing build without rebuilding, and changed no source, permissions, or ownership.

Generated-output maintenance:

- Moved the root-created `build/Testing/Temporary` (mode 755, `root:root`, empty) into a private archive under the ignored `agent-state/cloud-tests/ctest-history.*` directory. The archive is not committed.
- CTest then created a new `build/Testing/Temporary` owned by `ubuntu`, under the already-prepared `build/Testing` (`755 1000 1000`).

Recovery run:

| Step | Command | Exit | Result |
| --- | --- | --- | --- |
| Maintenance | `mv build/Testing/Temporary <archive>/Temporary` | 0 | archived, not deleted |
| Test | `cloud-test` | **0** | UID printed as 1000; `ctest --test-dir build --output-on-failure` |

CTest totals: **86 of 86 passed, 0 failed, 0 not run** (total test time 254.82 s).

Within-test SKIP lines (counted from the new `build/Testing/Temporary/LastTest.log`, not from CTest totals):

- Exactly **one** QTest skip: `SharedOutputRoutingTest::initTestCase() PipeWire configuration pipewire.conf not found`.
- Its suite's totals line reads `0 passed, 0 failed, 1 skipped`. All other QTest totals report 0 skipped.
- This is an optional native-routing fixture that searches only FHS locations. The fixture was not changed, and nothing was suppressed. The skip is recorded as observed coverage.
- The grep for `SKIP` in the log returns 97 hits, but most are `0 skipped` summary lines or test names that contain the word `Skip`. The count of real skips is 1.

Warnings in the recovery test log: 0.

Not claimed: CTest totals are not a substitute for the within-test skip count, and this recovery did not rerun the build.
