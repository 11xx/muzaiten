# Series 01 repair: store-free semantic provider status for GUI callers

## Coordinates

| Field | Value |
| --- | --- |
| Input | `claude/muzaiten-diagnostic-repair-87irzs` at `9d430facf327946ea9ba7e1448b31b3de555bad4` (diagnostic repair report head) |
| Result branch | `claude/muzaiten-diagnostic-repair-87irzs`, fast-forward only |
| Source-change head | `7b22fb9d3a55863056f37da1e33153eb4566ddf3`, one commit on the input |
| Tested head | `7b22fb9`, clean tree (final gate below) |
| Report head | the commit adding this file and `semantic-provider-repair-receipt.json`; it follows the source head and changes only `results/` |

`diagnostic-repair.md` and its receipt stay as the snapshot of source head
`e52b83c`; their returned contract issue is what this repair closes, using
option 1 as the local lead selected.

## Premise

`tools/features-clap/src/muzaiten_features_clap/protocol.py` answers
`status` without a `features` parameter with capabilities, model, runtime
and device, and calls the store status only when `features` is present.
Confirmed in the source before coding; no provider code or protocol changed.

## Change

- `muzaiten-features status --semantic-only [--state PATH] [--provider PATH] [--json]`
  resolves and opens no feature store, invokes the provider's `status` with
  empty parameters through the same discovery and status handling, and
  prints only `{"semantic": ...}` (exit 0 whether or not a store exists).
  It reads the saved provider path from `state.sqlite` read-only, as before,
  through the non-creating default. `doctor --semantic-only` and
  `status --semantic-only --features PATH` exit 2.
- Plain `status`/`doctor` are unchanged from `e52b83c`: the store is
  classified first; absent, unsupported and unreadable stores exit 4 with no
  provider call.
- `MainWindow` setup/download status requests and the Analysis status
  dialog's provider row now run `status --semantic-only --json`. The
  cancellation, generation, path-change (still including the feature-store
  path) and 45 s timeout guards and saved-provider discovery are unchanged.
  The dialog's unused feature-path parameter was removed.

## Evidence (all tests and probes as UID 1000)

| Step | Revision | Command | Exit | Result |
| --- | --- | --- | --- | --- |
| caller witness | working tree = `7b22fb9` | real helper, synthetic provider, fresh root: old GUI argv `status --features ABSENT --json` vs `status --semantic-only --json` | 4 / 0 | old argv: exit 4, no stdout, provider never invoked (so no consent was reachable); new: `semantic` only, `model-missing`, provider ran `capabilities`, `status`; no root created |
| build | working tree = `7b22fb9` | `cloud-env make build` (warm) | 0 | 0 warnings, 0 errors |
| focused native | working tree = `7b22fb9` | `python3 tests/test_features_status.py build/muzaiten-features -v` | 0 | 11/11 ok, 0 skipped |
| focused GUI | working tree = `7b22fb9` | `build/tests/test_main_window_navigation semanticStatusRealHelperWithoutFeatureStore semanticStatusHeartbeat semanticStatusSetupResults` | 0 | 11 passed, 0 failed, 0 skipped |
| final build | `7b22fb9`, clean tree | `cloud-env make build` | 0 | 152 steps (version identifier changed), 0 warnings, 0 errors |
| final test | `7b22fb9`, clean tree | `cloud-test` | 0 | UID 1000; CTest 88/88 passed, including `test_features_status` and `test_main_window_navigation`; QTest 1540 passed, 0 failed, 1 skipped |

New fixtures:

- `test_semantic_only_reads_no_store`: default paths under a fresh
  (nonexistent) root, a root holding a schema-99 store and a root holding a
  non-database file; with a logging synthetic provider (exit 0, only
  `semantic`, state `model-missing`, requests exactly `capabilities` then
  `status` with `{}` parameters) and with an absent provider
  (`provider-missing`, provider never run). The private root's files are
  byte-identical afterwards (only the provider's own log changes), so no
  feature, state or config store or directory was opened or created.
- `test_semantic_only_is_status_only_and_storeless`: `doctor --semantic-only`
  and `--features` with `--semantic-only` exit 2 without running the provider.
- `semanticStatusRealHelperWithoutFeatureStore(setup|download)`: a real
  `MainWindow`, the real built `muzaiten-features` found on `PATH`, and a
  synthetic provider via `MUZAITEN_FEATURES_CLAP`, with no feature store.
  Setup reaches `semantic.provider.setup` showing `model-missing`; download
  reaches `semantic.model.consent` with the provider's model details. The
  provider's `status` request has no `features`. Canceling closes the dialog
  and the helper; the provider log holds only `capabilities` and `status`
  (no `model-download`), and no feature store exists afterwards.
- `semanticStatusHeartbeat` now asserts the new argv; its cancellation and
  path-generation companions pass unchanged.

The GUI fixture was not run against the pre-change GUI code (that needed a
second rebuild within the wall bound); the caller witness above runs the
pre-change argv against the same real helper and synthetic provider instead.

Skips in the final gate, each an omission and not a pass, unchanged from
the earlier gates: `SharedOutputRoutingTest` (QSKIP, no `pipewire.conf`),
`test_version` Arch ordering (`vercmp` absent) and `test_fold_han_rare` cached
smoke (pinned references not cached). The new native and GUI fixtures have
no skips under UID 1000.

## Docs and changelog

`docs/cli.md` (example and the `--semantic-only` paragraph),
`docs/semantic-analysis.md` (provider discovery), `docs/data-safety.md`
(feature-store diagnostics) and the existing Unreleased Fixed entry in
`CHANGELOG.md`, whose "setup and download report a failed check" consequence
no longer holds. `docs/controls.md` and `docs/features-schema.md` describe
nothing that changed. CLI copy uses no em-dashes.

## Residual findings

- The Analysis status dialog's provider row has no dedicated GUI fixture; it
  is covered by the native semantic-only fixtures and its compile.
- Pre-existing, unchanged: the GUI does not forward a `--state-dir` or
  `--state-root` given to the app on its command line, so the helper reads
  the saved provider path from its own default state (environment
  overrides are inherited and do agree). Forwarding `--state` would change
  the caller contract; not needed for this regression.
- Unchanged from the earlier report: a WAL store with a missing `-shm` in an
  unwritable directory is reported unreadable.

## Bounds

Wall clock, measured: continuation started 23:21:41 UTC; repair built and focused fixtures green by 23:28; final gate 23:28:53 to 23:33:34; report committed afterwards, before the stop.
The absolute stop is 23:54:41 UTC. Native list-price cost: not measured here;
no billing or token figures are available, so none is claimed.

## Private evidence (not committed)

Under `agent-state/cloud/logs/` (SHA-256): warm build `3f87369f`,
focused runs `b3be0ab2`, caller witness
`3c24897c`, final build `42c77063`,
final test `9aa7dff9`, final CTest `LastTest.log`
`b1562394`. Full digests are in the receipt.
Nothing here has been independently reviewed or locally accepted.
