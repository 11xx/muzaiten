# Hosted build toolchain

`cloud-init.sh` is the cloud environment's Setup script. It installs a pinned
Nix client when one is absent and signed package substitutes, verifies the compiler,
Qt and media capabilities, and installs `cloud-env`. Development commands run
directly from the captured SDK environment; the coding user does not need a
Nix daemon, store writes, container runtime, or dependency installation.

The input revision and hash live in `shell.nix`. Local dependency builds and
remote builders are disabled during provisioning. Package signature checking
remains enabled. The bootstrap archive has a fixed SHA256. The installed SDK
uses about 3.5 GiB; build output remains in the checkout's `build/`.

Use a dedicated cloud environment with the contents of `cloud-init.sh` as its
Setup script. Keep the account's existing network and authentication policy.
Provisioning needs the GitHub and Nix release/cache hosts. `render-init.py`
regenerates the script from its five explicitly selected inputs.

Build and run in the SDK:

```sh
cloud-env bash docs/cloud/series-01/environment/preflight.sh
cloud-env make build
cloud-env make test
cloud-env ./build/muzaitenctl --version
```

Run filesystem permission tests under a normal user. A root process can bypass
the fixture's permission bits and cannot establish those assertions. Record
the actual user ID, tested revision and SDK identity in the return receipt.
Do not skip the permission tests or label a root-only result equivalent.

`cloud-env` retains the caller's home and application-state settings. Runtime
application data must still use an isolated `MUZAITEN_STATE_ROOT`. The SDK
sets the offscreen Qt platform and supplies SVG, fonts, GStreamer codecs and
private PipeWire tools needed by the fixtures.

The platform's cached environment contains files rather than running
processes. This SDK requires no background service for build execution.
Dependencies missing from the signed cache cause provisioning to fail;
compiler/Qt requirements and checks are preserved.

Provisioning reuses an existing Nix client of version 2.34 or newer. It adds
the pinned SDK closure without replacing existing store paths, profiles or
system configuration. An absent installation uses the verified bootstrap
archive. A populated store without a usable client is preserved and causes
setup to stop.
