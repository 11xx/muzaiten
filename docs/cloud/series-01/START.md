Execute the bounded muzaiten series in `docs/cloud/series-01/README.md`,
starting from `dispatch/cloud-01`. Read and verify its manifest and checksums,
then read repository instructions and `RETURN.md`.

Run the environment pilot first. Inspect the installed compiler, CMake, Qt,
and dependency versions; provision a disposable environment if needed. Do
not lower project requirements. Establish the unchanged baseline with
`make build && make test`. If it cannot be established within one setup
attempt, return the blocker report and do not start product changes.

After the pilot passes, execute tasks 1 through 4 sequentially. Each task has
its own scope, fixtures, and stop conditions. Fix ordinary in-scope findings;
report unresolved contracts without guessing. Keep source changes in scoped
commits with docs and changelog entries. Run the required build/test checks.

Push your assigned result branch and open a draft PR targeting
`dispatch/cloud-01`; do not merge or push `master`. If PR creation is
unavailable, provide the pushed branch and exact head. Include the return
report and receipt described in `RETURN.md`. Disable automatic private
session-link/coauthor attribution. Keep all runtime data and detailed raw logs
in ignored private state. No additional agents, auto-memory writes, user data,
credentials, signing keys, hardware experiments, or work outside this packet.
