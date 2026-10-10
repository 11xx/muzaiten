# Series 01 repair: feature-diagnostic read policy

## Coordinates

| Field | Value |
| --- | --- |
| Input | `dispatch/cloud-01-result` at `471858c001b10df14553a64539ae9b3042051179` (verified equal to the session's starting HEAD and an ancestor of the result) |
| Result branch | `claude/muzaiten-diagnostic-repair-87irzs` (the session's assigned branch) |
| Source-change head | `e52b83c16bcca9b82fe97f48840f301752be1958`, one commit on the input |
| Tested head | `e52b83c`, clean tree (final gate below) |
| Report head | the commit adding this file and `diagnostic-repair-receipt.json`; it follows the source head and changes only `results/` |

`report.md` and `receipt.json` in this directory are the original series
delivery. They describe source head `8d090e9` and remain snapshots of that
earlier head; their task-3 "contract decision required" finding is what this
repair resolves under the policy the local lead selected.

Packet verification: `sha256sum -c docs/cloud/series-01/SHA256SUMS` exited 0,
20 entries OK. `.claude/settings.json` (unchanged) disables commit/PR
attribution, session links and auto-memory.

## Outcome

Repaired, with one contract issue returned (consumer compatibility, below).

Policy implemented as selected: `status` and `doctor` never create,
initialize, migrate, drop, downgrade or rewrite a feature store. A store with
exactly this build's schema keeps its read-only connection, output and exit 0.
Any other store exits 4 with one stderr line, empty stdout and no provider or
state-store access:

| State | stderr |
| --- | --- |
| absent (file or parent missing) | `feature store absent: PATH does not exist` |
| unsupported | `feature store unsupported: PATH has schema version 99, newer than this build's 5` (also `older`, `records no schema version`, `lacks the T table`, `has an incompatible features table`, `lacks T.generation_id`, `has an unfinished T migration`) |
| unreadable | `feature store unreadable: PATH: CAUSE` (SQLite open/query error, or `not a file`) |

The schema probe lists tables through `sqlite_master` with error checks, so a
query error is reported as unreadable and never as an unsupported verdict.
Nothing in the diagnostic path calls `openFeaturesDatabase` or `initSchema`.
A failed status count after a passing probe is unreadable rather than zero
rows. Default `--features`/`--state` resolution uses new
`AppPaths::dataDirWithoutCreating`/`stateDirWithoutCreating` (the config file
is consulted only if it exists); every other `AppPaths` caller still creates
its directories. Refresh, pack, provider and writer locking are unchanged.

## Fixtures (`tests/test_features_status.py`, status and doctor each)

| Test | Covers |
| --- | --- |
| `test_absent_store_is_not_created` | missing file; missing parent two levels deep; full tree of the private root byte-identical |
| `test_absent_store_at_default_paths_creates_no_directories` | no `--features`/`--state`: a nonexistent `MUZAITEN_STATE_ROOT`, and XDG defaults under an empty private `HOME`; neither gains any entry |
| `test_unsupported_store_is_reported_unchanged` | pre-v3 (schema 2, bliss-era `features` row), schema 99, missing schema version, schema 5 without `track_neighbors`, without `features.energy`, with old-shape `embeddings`, with a leftover `embeddings_v4` |
| `test_unreadable_store_is_reported_unchanged` | non-database file, store cut to one page, store cut after `meta` (probe passes, counts fail), directory at the path, mode-000 copy (UID 1000 only) |
| `test_current_store_is_read_without_writes` | `data_version`, logical dump, meta unchanged |
| `test_read_only_store` | unwritable directory with and without WAL sidecars |
| `test_reads_last_commit_while_writer_holds_transaction` | separate writer process holds `BEGIN IMMEDIATE` after a commit, synchronized over a pipe |
| `test_current_store_consults_provider` | positive control for the provider-run marker every refusal checks |
| `test_refresh_still_creates_store` | refresh from an empty synthetic library creates a store in a missing nested directory and under a fresh state root; status then exits 0 |

Unchanged-store checks keep an observer open to compare `PRAGMA data_version`
and the full `iterdump`, and compare every file's SHA-256 under the private
root. A read-only reader of a WAL store in a writable directory creates an
empty `-wal` and a `-shm` index when they are absent (observed with SQLite
3.53.3); the fixtures allow exactly those, require the `-wal` to stay empty,
and do not digest `-shm`, whose read marks every reader updates.

## Evidence

| Step | Revision | Command | Exit | Result |
| --- | --- | --- | --- | --- |
| preflight | `471858c` | `cloud-env bash docs/cloud/series-01/environment/preflight.sh` | 0 | compile/link, function_ref, audio tools, GStreamer elements verified |
| baseline build | `471858c` | `cloud-env make build` (fresh VM) | 0 | 986 steps, 0 warnings, 0 errors |
| witness | `471858c` binary, new fixtures | `runuser -u ubuntu -- cloud-env python3 tests/test_features_status.py build/muzaiten-features` | 1 | 33 subtest failures in the 4 new refusal tests; the 5 other tests pass |
| baseline probe | `471858c` | private `repair_mutation_probe.py` | 0 | all exit 0: schema 99 rewritten to 5; pre-v3 rows dropped and schema set to 5; absent store and directory created; fresh root gained `data/features.sqlite` and `state/` |
| repair build | working tree = `e52b83c` | `cloud-env make build` | 0 | 253 steps, 0 warnings, 0 errors |
| focused | working tree = `e52b83c` | `runuser -u ubuntu -- cloud-env python3 tests/test_features_status.py build/muzaiten-features -v` | 0 | 9/9 ok, 0 skipped, UID 1000 |
| final probe | working tree = `e52b83c` | private `repair_mutation_probe.py` | 0 | all exit 4 with the causes above; schema 99 and the pre-v3 row kept; nothing created |
| final build | `e52b83c`, clean tree | `cloud-env make build` | 0 | 151 steps (the embedded version identifier changed), 0 warnings, 0 errors |
| final test | `e52b83c`, clean tree | `cloud-test` | 0 | UID 1000; CTest 88/88 passed in 180.45 s, including `test_features_status`; QTest 1538 passed, 0 failed, 1 skipped |

Witness detail: 18 refusal subtests got exit 0 (absent, default-path, and
five unsupported shapes); the pre-v3 and leftover-migration cases exited 4
from a query after the initializing path had run (`no such column`); the
unreadable cases exited 4 with raw SQLite errors and no state label; and the
fresh state root was created.

Environment (observed): Ubuntu 24.04 container, Linux 6.18.44, x86_64. SDK
`/opt/muzaiten-cloud` from `shell.nix`, nixpkgs
`e7439b6b14ad3cc35d05608ebca9bce01a25f5f8`: GCC 16.2.0, CMake 4.4.3, Qt
6.11.2, Python 3.14.7, SQLite 3.53.3, PipeWire 1.6.9 development files found.
`CMAKE_BUILD_PARALLEL_LEVEL=2`, build output in the checkout's `build/`.
Builds ran as UID 0 through `cloud-env`; every test and probe ran as UID 1000
(`ubuntu`). `cloud-test` archived the existing CTest temporary output under
`agent-state/cloud-tests/` as designed. Only synthetic private roots were used.

Skips in the final gate, each an omission and not a pass, identical to the
original delivery's: `SharedOutputRoutingTest` (QSKIP, no `pipewire.conf` in
the headless container, so shared-output routing is not covered),
`test_version` Arch ordering (`vercmp` absent) and `test_fold_han_rare` cached
smoke (pinned rare-Han references not cached). The new fixtures have no skip
under UID 1000; their mode-000 case is skipped only when run as root.
The full suite was not run on the unchanged input; the original delivery
records 88/88 at `8d090e9`, and `471858c` changes only `results/`.

## Docs and changelog

`docs/cli.md` (feature orchestration: diagnostic states table),
`docs/features-schema.md` (meta keys, migration note), `docs/data-safety.md`
(new feature-store diagnostics section), and the existing `CHANGELOG.md`
Unreleased Fixed entry, rewritten because its "initialized as before" sentence
no longer holds. CLI messages use no em-dashes.

## Returned contract issue: first-time semantic setup and model download

Checked at the lead's checkpoint. `MainWindow::configureSemanticProvider` and
`downloadSemanticModel` run `status --features PATH --json` and treat any
non-zero exit as "Semantic provider status check failed" with an empty
`semantic` object. With no store yet (a fresh install before any analysis),
the selected policy makes that call exit 4 without invoking the provider:

- *Download semantic model* is disabled: `finishSemanticStatusRequest`
  cancels on the error, and `showSemanticModelConsent` needs
  `semantic.provider.model`, which only a provider call supplies. The CLI
  `muzaiten-features model download` does not read the store and still works.
- *Semantic provider setup* still opens and can save a chosen or automatic
  provider path, but shows "Provider status unavailable" instead of the
  resolved provider.
- The Analysis status dialog only queries the provider for a store the app
  opened, so absence does not reach it; an app-supported older store (v3/v4
  before its first refresh) now shows "not available (install
  muzaiten-features-clap)", which misnames the cause.

No consumer change can restore these under the selected rules: the provider
details exist only from a provider invocation, which an absent store must not
trigger, and creating the store first is excluded. So no consumer code was
changed. A bounded fix needs a contract decision, for example:

1. A store-independent provider check, such as `status --semantic-only`,
   that resolves no store path, reports only the `semantic` object, and is
   what the GUI's setup and download checks run. Store diagnostics keep exit 4.
   Cost: one CLI option, its docs, a two-line GUI argument change, fixtures.
2. Keep the GUI as is and document that a first native analysis must precede
   GUI model download (CLI download remains available).
3. Let the setup/download path, but not the store diagnostics, tolerate exit
   4 `absent` by reporting it explicitly; this improves the message but still
   cannot offer consent without provider details.

The Unreleased changelog entry states the current consequence.

## Remaining findings

- The contract issue above (decision required).
- A WAL store whose `-shm` is missing in an unwritable directory is now
  reported as unreadable (exit 4, explicit cause) instead of failing through
  the writable path; reading it would need a WAL policy change (outside this
  series).
- Reading a WAL store in a writable directory leaves an empty `-wal` and a
  `-shm`, as any SQLite reader does; no committed write occurs.

## Bounds

Wall clock, measured: execution start 22:42:09 UTC; baseline build done 23:04 (fresh VM, about 22 min); repair build and focused fixtures done 23:10; final gate done 23:14:31 (32 min 22 s after start); report committed afterwards, within the 75-minute bound. One repair
pass, no test failures to repair (one rerun of the focused command after I
passed its arguments in the wrong order; that run executed no fixtures).
Native list-price cost: not measured; no billing or token figures are
available in this environment, so no estimate is claimed.

## Private evidence (not committed)

Under `agent-state/cloud/logs/` (SHA-256): checksums `00759102`, preflight `e1f667b6`,
baseline build `b3aec233`, baseline `muzaiten-features` binary `26ee50e4`,
witness `869ba616`, baseline probe `1b1aef00`, repair build `7b7ed754`,
focused run `63103b4b`, final probe `5ada3b09`, final build `e7843c4b`,
final test `9d0a2ce2`, final CTest `LastTest.log` `f5d36b63`
(copied to `logs/final-ctest/`). Full digests are in the receipt.
Probe script: `agent-state/cloud/probes/repair_mutation_probe.py`.
Nothing here has been independently reviewed or locally accepted.
