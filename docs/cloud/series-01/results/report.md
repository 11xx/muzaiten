# Series 01 return report

## Coordinates

| Field | Value |
| --- | --- |
| Source baseline | `446af93337b9b7f24bffe340f9cf7c7233b03743` (ancestor of the result, verified) |
| Packet commit | `1f0e51e2af291f1d008b8202f8efb0a60f397eef` (`dispatch/cloud-01` head as executed; packet first published at `30660f27`) |
| Result branch | `dispatch/cloud-01-result`, descended from the packet commit |
| Source-change head | `8d090e9ce9cd05b562162131ff7edc92c632a66e` |
| Report commit | the commit adding this file; it follows the source head |

Packet verification: `sha256sum -c docs/cloud/series-01/SHA256SUMS` exited 0
with all 20 entries OK (manifest included). `.claude/settings.json` disables
commit/PR attribution, session links and auto-memory.

## Outcomes

| Task | Outcome | Commits |
| --- | --- | --- |
| 0 Environment | baseline established | none |
| 1 CLI version | repaired | `0fe545a` |
| 2 Scan errors | repaired (one cleanup choice flagged for review) | `6600e51` |
| 3 Feature diagnostics | repaired for current-schema stores; absent/outdated policy returned | `5e6a0ea` |
| 4 Taste boundaries | dry-run: investigated, no defect against the documented promise; near-limit start: repaired | `8d090e9` |

Each source commit carries its code, focused fixtures, guide updates and a
`CHANGELOG.md` Unreleased entry. No Implemented-by trailer was added: the
executing platform forbids model identifiers in repository artifacts, so that
coordinate is omitted here as well. Nothing in this delivery has been
independently reviewed or locally accepted.

## Environment (observed)

- Ubuntu 24.04.5 LTS container, Linux 6.18.44, x86_64, 4 vCPU, 15 GiB.
- SDK `/opt/muzaiten-cloud` from `shell.nix` (nixpkgs
  `e7439b6b14ad3cc35d05608ebca9bce01a25f5f8`): GCC 16.2.0, CMake 4.4.3,
  Ninja 1.13.2, Python 3.14.7, Qt 6.11.2 (all nine modules), TagLib 2.2.1,
  zstd 1.5.7, Chromaprint 1.6.1, GStreamer 1.28.7, GLib 2.88.3. Preflight
  exited 0 (compile/link, function_ref, native audio tools, GStreamer
  elements verified). PipeWire 1.6.9 development files present
  (`PIPEWIRE_FOUND=1`).
- Builds ran as UID 0 via `cloud-env`; every `cloud-test` run executed the
  suite as UID 1000 (`ubuntu`), so permission fixtures were real.
- `CMAKE_BUILD_PARALLEL_LEVEL=2`, `MUZAITEN_STATE_ROOT=<checkout>/agent-state/cloud`;
  tests used `agent-state/cloud-tests`. No user data, credentials or host
  audio/display were used.

## Gates

| Revision tested | Build | Warnings/errors | CTest | QTest totals | Skips |
| --- | --- | --- | --- | --- | --- |
| `1f0e51e` baseline | exit 0 (986 steps, 21m42s) | 0/0 | 86/86, exit 0 | 1536 pass, 0 fail | 3 |
| task 1 tree (= `0fe545a`) | exit 0 | 0/0 | 87/87, exit 0 | 1536 pass | 3 |
| task 2 tree (= `6600e51`) | exit 0 | 0/0 | 87/87, exit 0 | 1537 pass | 3 |
| task 3 tree (= `5e6a0ea`) | exit 0 | 0/0 | 88/88, exit 0 | 1537 pass | 3 |
| task 4 tree (= `8d090e9`) | exit 0 | 0/0 | 88/88, exit 0 | 1538 pass | 3 |
| `8d090e9` exact head, clean tree | exit 0 | 0/0 | 88/88, exit 0 | 1538 pass | 3 |

Task gates ran on the working tree immediately before each commit, with the
same content as the commit. The same three skips are present in every run
and are omissions, not passes: `SharedOutputRoutingTest` (QSKIP, no
`pipewire.conf` in the headless container, so shared-output routing is not
covered), `test_version` Arch ordering (`vercmp` absent) and
`test_fold_han_rare` cached smoke (pinned rare-Han references not cached;
outside this series).

## Task 1: CLI version

Baseline witness (new `tests/test_cli_version.py` against baseline binaries,
exit 1): `muzaitenctl --version` exited 1 with "no reply from muzaiten" after
connecting to the fixture's listening socket and created `state/state`;
`muzaiten-features --version` exited 2, `unknown command "--version"`.

Repair: a sole `--version` argument prints `<binary> <MUZAITEN_VERSION>` from
the generated `Version.h` and exits 0 before any path, store, provider or
socket resolution, matching Qt's `muzaiten --version` format. The fixture
installs the binaries into a private prefix with local-only install rules
(it removes the `install_local_manifest.txt` this leaves in `build/`; the
real `install_manifest.txt` is untouched), compares both CLIs with the
installed app's version, and proves no new entry under the private roots and
no accepted connection on a live listener; the listener accepts the
connection of an ordinary command, the positive control. Help, no-argument
and unknown-command exits are unchanged (0/1 for ctl, 0/2 for features).
Docs: `docs/cli.md`, `docs/distribution.md`; changelog Added.

## Task 2: Scan fingerprint errors

Discriminator: a second connection renames `tracks.metadata_scanned`, which
fails only the fingerprint SELECT (the fixture asserts that SELECT fails and
`COUNT(*)` succeeds); `ensureIngestSession` and earlier setup issue no SQL.
Baseline witness: `scanAbortsWhenFingerprintReadFails` failed with the scan
pipeline started despite the failed read (test exit 1).

Repair: `Database::trackFingerprints` returns
`std::expected<QHash, QString>`, keeping the `prepare` error (exec alone only
reported "No query"). `ScanController::startScan` reads before any scan
state; on failure it reports one status-bar error naming the root and the
SQLite error, records it as the source root's last-scan status, clears the
rest of the queued run, resets forced rescan and resumes the placeholder
fill as a finished run would. No thread, ingest session, progress UI, fill
pause, placeholder, tag read or missing mark occurs. After restoring the
column, the same queued roots scan in order, the vanished track is marked
missing and both new files get placeholders. An empty successful read is
covered at the database API.

Flag for local review: ending the remaining queued roots on a read failure
(rather than continuing with the next root) is a cleanup choice I made
because a failing library read would most likely fail every root; it is
documented in `docs/data-safety.md`.

## Task 3: Feature diagnostics

Fixtures (private probe, logs retained) cover a current store, read-only
media, absent store and parent, pre-v3 and newer (99) schemas, and a writer
in its own process that commits a row and then holds `BEGIN IMMEDIATE`,
synchronized over a pipe. Baseline, `status` and `doctor` alike:

| Case | Exit | Observed |
| --- | --- | --- |
| current | 0 | commits writes (`PRAGMA data_version` changes); `dsp_version` rewritten |
| current, read-only directory | 4 | "attempt to write a readonly database" |
| writer mid-transaction | 4 after 5.0 s | "database is locked"; no output |
| absent store and parent | 0 | creates directory and store |
| pre-v3 | 0 | drops the old `features` rows, converts to WAL, schema 5 |
| newer schema 99 | 0 | rewrites `schema_version` to 5 |

This conflicts with documented behavior: `features-schema.md` says the app
treats the store as read-only, but the app's Analysis status dialog runs
`status`, which writes without the writer lock and, during a refresh, fails
so that the dialog says the provider is not installed; the rewritten
`dsp_version` hides the mismatch `muzaitenctl features-status` is documented
to report.

Repair: when the store exactly matches this build's schema (checked by
read-only queries), status and doctor use a read-only `query_only`
connection; a sidecar-less store in an unwritable directory is opened
`immutable=1`, since no writer can create its WAL there. Output format is
unchanged. After the fix: current and read-only cases exit 0 with no commit
and unchanged metadata; the writer case exits 0 at once with the committed
row and without the uncommitted one. `tests/test_features_status.py` (baseline
witness: 5 subtest failures) pins these.

Contract decision required (not changed): absent, pre-v3 and newer stores
still take the initializing path, so a diagnostic creates a store, destroys
pre-v3 feature rows and downgrades a newer store's `schema_version` (which
would then pass the app's own supported-version check). Options: (a) keep
this, now documented in `docs/cli.md`; (b) report a state such as `absent`
or `unsupported-schema` without touching the file, with a distinct exit
code; (c) create only when absent and refuse other schemas. Mutation-command
locking, provider calls and refresh were not changed.

## Task 4: Taste boundaries

Import dry-run (real CLI, isolated root): a duplicate name without
`--replace` and a 33rd seed both exit 0 on dry-run, while the real imports
exit 1 ("already exists", "At most 32"); replacement dry-run and real both
succeed. The store digest was unchanged by every dry-run and rejection. A
dry-run on a fresh root creates only the empty `config` directory through
path resolution. `docs/cli.md`, `radio.md` and `data-safety.md` promise that
dry-run inspects and previews the profile, not that the import will apply, so
the ordering was retained. Policy note for the lead: `radio-weights import
--dry-run` and `pack import --dry-run` document applicability checks; aligning
taste dry-run would be a new policy.

Near-limit start: a seed of 256 two-byte-letter genres at 1/256 plus decades
is accepted at 65,037 bytes. Aliasing one genre to `other` drops it and
renormalizes 255 shares to 1/255, which serializes past 65,536 bytes. On the
baseline the seed started without the alias and with it returned
`unavailable-taste-seed` (the same reply as an absent seed; the GUI said
"seed or candidate pool is unavailable"). Repair: `startTasteRadio` returns
`Started`, `Unavailable` or `TooLarge`, deciding `TooLarge` from the canonical
serialization before any session state; IPC replies
`radio: taste-seed-too-large` and the GUI names the cause. The limit,
canonical serialization, aggregate content, consent, pool scope and start
algorithm are unchanged; radio, queue and saved session state stay unchanged
on refusal (fixture `nearLimitSeedThatGrowsUnderAliases`).

## Docs and changelog

`CHANGELOG.md` Unreleased: Added (CLI version) and three Fixed entries.
Guides: `docs/cli.md`, `docs/distribution.md`, `docs/data-safety.md`,
`docs/features-schema.md`, `docs/radio.md`. UI copy uses no em-dashes.

## Private evidence (not committed)

Under `agent-state/cloud/logs/` (SHA-256 prefixes): baseline build `4a407916`,
baseline test `e4785f55`, baseline LastTest `3a068743`, preflight `eb1ec3c2`;
witnesses t1 `0a055642`, t2 `edc51b99`, t3 `0b9e3bad`, t4 `39abc55a`;
task test logs t1 `36226c0d`, t2 `7a314393`, t3 `583ec2fd`, t4 `f73c9263`;
feature-store probes baseline `a7dba0cc`, final `e1973964`; taste dry-run probe
`a6eb4d3e`; final exact-head build `69581e6e`, test `d13fb259`, LastTest
`6aa8524a`. Probe scripts are in `agent-state/cloud/probes/`.

## Residual risks

- Task 2 cleanup choice and task 3 absent/outdated policy, above.
- The read-only status path depends on SQLite's read-only WAL behavior; a
  store whose WAL exists but whose shared-memory file is missing in an
  unwritable directory still falls back and fails as before.
- PipeWire shared-output routing remained uncovered in this container.
