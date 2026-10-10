# Establish read semantics for feature-store diagnostics

`status` and `doctor` use the feature-store opening/initialization path in
`src/indexer/FeaturesMain.cpp`. Initialization can upsert metadata without the
writer lock used by mutation commands. No existing data-loss incident is
asserted. Establish the actual diagnostic contract before proposing a repair.

Inspect `openFeaturesDatabase`, schema initialization, `runStatus`, command
dispatch, `src/features/FeatureStoreLock.cpp`, native refresh, feature-pack
operations, `docs/cli.md`, `docs/features-schema.md`, and their tests.

Use isolated deterministic fixtures for:

1. An existing, current-schema store, including a read-only filesystem case.
2. An absent store and its parent directory.
3. An outdated or incompatible schema.
4. A concurrent native writer, synchronized at a meaningful committed-row
   boundary rather than by sleeps.

Record logical contents, filesystem changes including sidecars, command
output/exit codes, and whether readiness blocks behind the writer. Account
for SQLite WAL behavior when interpreting byte changes. A clean byte digest
alone does not prove no writes occurred.

If fixtures demonstrate a conflict with documented diagnostic behavior,
implement the smallest current-schema read-only opening path and explicit
handling for other states. Preserve responsiveness, status/doctor wire
formats, and mutation-command locking. If absent/outdated-store behavior is
undocumented or requires a new policy, return the options and evidence without
silently selecting a migration or creation policy.

Allowed changes after a demonstrated defect: diagnostic opening/query paths,
focused tests, matching guides, and `CHANGELOG.md`. No blanket schema upgrade,
writer-lock redesign, long-refresh blocking, provider/network calls, or
unrelated pack changes. Return a report-only result if no defect is established.
