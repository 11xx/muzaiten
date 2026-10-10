# Check taste-seed preview and radio-start boundaries

Two reported boundaries need deterministic discrimination against the
documented behavior. They are independent; keep any repairs in separate
commits and do not treat either report as a proven failure.

## Import dry-run

`TasteProfiles::importProfile` in `src/reco/TasteProfile.cpp` returns from
dry-run after file validation/store loading, before same-name and store
capacity checks. Compare the promises in `docs/cli.md`, `docs/radio.md`, and
`docs/data-safety.md` with the real CLI and store behavior.

Use fixtures for a duplicate name without replacement, explicit replacement,
and a full 32-seed store followed by a 33rd import. Compare dry-run with the
real operation and prove no mutation on dry-run/rejection. If the documented
promise is only file validation, retain it and report that result. If it
promises applicability, repair the validation ordering without importing,
taking a long-lived write lock, or changing replacement consent. Return an
ambiguous policy to the local lead.

## Near-limit radio start

Inspect `TasteProfiles::withAliases`, `encode`, the canonical store bound, and
`AppCore::startPoolRadio` in `src/app/AppCore.cpp`. Alias normalization and
floating-share re-encoding can change the size of an accepted profile before
radio starts. Establish whether a profile accepted near the 64-KiB limit
really fails to start, and whether its failure is visible to the caller.

Build an accepted near-bound fixture that exercises real alias/normalization
and AppCore start handling. Record the accepted and re-encoded sizes and
caller outcome on the baseline. If the hypothesis does not reproduce, report
the discriminator and result. If it does, preserve canonical serialization,
aggregate privacy, consent, library/playability scope, and bounded storage;
provide an explicit caller outcome and the narrowest correction. Do not raise
the limit or alter the radio algorithm to make the fixture pass.

Allowed changes: the named taste store/start paths, focused taste/CLI/AppCore
fixtures, affected guides, and `CHANGELOG.md`. No large-library performance
work, asynchronous preview redesign, embeddings, extra telemetry, or broader
profile formats. Run full build/test gates for every source repair.
