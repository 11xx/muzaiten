# Cloud SDK baseline pilot report

Status: **blocked at task 0**. The pilot stopped at step 3 (`make build`), which failed at CMake configure. Step 4 (`cloud-test`) was not run, so there are no test counts and no UID 1000 evidence. Tasks 1-4 were not started.

Source: `225b1139e5b5687b23d33faf448dffe92a38f35b` (verified equal to `git rev-parse HEAD`).
Result branch: `claude/muzaiten-sdk-baseline-9wbyzp`.

## Checks

| # | Command | Exit | Result |
|---|---------|------|--------|
| 1 | `git rev-parse HEAD` | 0 | `225b1139e5b5687b23d33faf448dffe92a38f35b` |
| 1 | `id` | 0 | UID 0 (root). The pilot's cloud-env steps run as root. |
| 1 | `sha256sum -c docs/cloud/series-01/SHA256SUMS` | 0 | 20 of 20 entries OK |
| 2 | `cloud-env bash docs/cloud/series-01/environment/preflight.sh` | 0 | Compiler GCC 16.2.0, CMake 4.4.3, Qt 6.11.2 components, taglib 2.2.1, libzstd 1.5.7, libchromaprint 1.6.1, GStreamer 1.28.7. `function_ref`, `qt_compile_link`, native audio tools, and GStreamer elements reported verified or available. |
| 3 | `cloud-env make build` | 2 | Failed at configure. 1 CMake error block, 1 CMake warning block, 0 compiler warnings, nothing compiled. |
| 4 | `cloud-test` | not run | Stopped after the step 3 failure, per the pilot rule. |

## Build failure

CMake reported that `Qt6Gui` was found but set `Qt6Gui_FOUND` to FALSE, because its dependency `WrapOpenGL` was not found. The `FindOpenGL` probe reported these missing variables: `OPENGL_opengl_LIBRARY`, `OPENGL_glx_LIBRARY`, `OPENGL_INCLUDE_DIR`. CMake then stopped with "Configuring incomplete, errors occurred!" (make target `configure`, exit 2).

This points to a missing OpenGL development package in the cloud environment, which is not verified here. The source was not compiled, so nothing in the source is implicated.

## Findings

1. **Environment gap.** The cloud environment cannot configure the Qt6 GUI component, so `make build` fails before compilation. The fix belongs in the environment setup, not in this pilot. This pilot made no changes to provision, install, or alter permissions.
2. **Preflight coverage gap.** `preflight.sh` reported Qt compile and link as verified, but it did not exercise the `Qt6Gui`/`WrapOpenGL` configure path, so it passed while the real configure failed. Preflight should configure the same Qt components the build requires.
3. **Missing evidence.** The SDK version was not recorded in this pilot. The UID 1000 test run and all test counts and skips are absent, because step 4 did not run.

## Not claimed

- No independent review.
- No local acceptance.
- Cloud signatures: unavailable.
- No tasks 1-4 work.
- No PR opened.
