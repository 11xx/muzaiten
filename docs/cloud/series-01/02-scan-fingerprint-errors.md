# Abort a scan when its fingerprint read fails

`Database::trackFingerprints` returns an empty `QHash` when its SQL query
fails. `ScanController::startScan` passes the map into `ScanPipeline`, where
empty fingerprints also mean an empty/new root. This is a source-confirmed
ambiguity; durable consequences require a caller-visible fixture.

Inspect `src/db/Database.cpp` and `.h`, `src/ui/ScanController.cpp` and `.h`,
`src/scanner/ScanPipeline.cpp` and `.h`, ingest-session setup, metadata-fill
ownership, and scan/navigation tests. Account for `ensureIngestSession` and
earlier setup paths before choosing a failure injection.

Build a deterministic synthetic database fixture in which earlier scan setup
succeeds and specifically the fingerprint SELECT fails. Prove that the
baseline reaches this path. Use a narrow test seam or controlled schema/SQL
failure; do not add sleeps or break an earlier guard and call that coverage.

Propagate an explicit read result/error. If the read fails, report one useful
error and abort before scanner enumeration, placeholder ingestion, tag work,
or missing-file marking. Restore usable UI/controller ownership and preserve
queued-root order, background-fill behavior, cancellation, and subsequent
successful scans. A successful empty query remains a valid empty root. Cover
both that case and the targeted failure at the real ScanController boundary.

Allowed changes: the database fingerprint result API and callers, controller
error cleanup, focused test fixtures/wiring, relevant data-safety/control
guidance, and `CHANGELOG.md`. Do not redesign scanner delivery, move database
connections across threads, alter WAL/checkpoints, introduce new throughput
thresholds, or consume the separate startup-latency work. Return any material
cleanup/ownership contract conflict to the local lead. Run full build/test
gates; this data-lifecycle slice requires local content review.
