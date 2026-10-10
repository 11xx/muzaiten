# Portable correctness and diagnostics series

This packet authorizes four bounded muzaiten tasks on isolated synthetic data.
The source baseline is `446af93337b9b7f24bffe340f9cf7c7233b03743`.
The publication and pull-request target is `dispatch/cloud-01`.
`master` is integrated and published by the local lead.

Read `AGENTS.md`, this packet, and each task's named source before editing.
Verify the packet from the repository root with
`sha256sum -c docs/cloud/series-01/SHA256SUMS`. The
manifest identifies the source baseline, task order, and hashed packet files;
it is a transport description, not an Arc ledger or approval.
The branch's `.claude/settings.json` disables automatic coauthor/session-link
attribution and auto-memory. [TRANSPORT.md](TRANSPORT.md) describes the
boundary between public Git delivery and private Arc state.

| Order | Task | Contract |
| --- | --- | --- |
| 0 | [Environment](00-environment.md) | Establish the baseline build and test result before source changes. |
| 1 | [CLI version](01-cli-version.md) | Local build identity without IPC or application-store access. |
| 2 | [Scan errors](02-scan-fingerprint-errors.md) | Distinguish a failed fingerprint read from an empty library root. |
| 3 | [Feature diagnostics](03-feature-diagnostic-reads.md) | Investigate diagnostic-store initialization and fix demonstrated defects. |
| 4 | [Taste boundaries](04-taste-boundaries.md) | Investigate dry-run and near-limit radio-start behavior; fix demonstrated defects. |

Run one task at a time in one cloud session. Keep each task in separate
reviewable commits. A failed premise ends that task with evidence, rather than
an invented implementation. Tasks 3 and 4 permit investigation and a repair
supported by a failing baseline fixture; an unresolved behavioral choice
returns to the local lead. Continue an independent task when its prerequisites
remain available.

## Execution rules

- Work on the cloud session's assigned branch descended from this packet.
  Preserve the source baseline and the packet files. Commit and push only that
  result branch. Open a draft PR against `dispatch/cloud-01` when supported.
- Use private `MUZAITEN_STATE_ROOT`, XDG, runtime, and test roots. Use synthetic
  libraries and fixtures. Never import account data, music libraries, service
  credentials, signing keys, or the operator's environment.
- Run `make build && make test` in the checkout's `build/` for each source
  slice. Record the exact tree and environment. Fix relevant failures without
  weakening requirements, adding skips, increasing timeouts to hide races, or
  disabling checks. Classify unrelated/environment failures with evidence.
- Ship affected guides and `CHANGELOG.md` Unreleased entries with behavior
  changes. Preserve unrelated entries and existing UI copy style.
- Keep Git identity as supplied by the environment. Use structured commit
  messages and evidenced `Implemented-by`/`Planned-by` role trailers. Do not
  invent model coordinates or independent review. Unsigned cloud delivery is
  accepted for local signing; signing keys are never transported.
- Exclude AI `Co-Authored-By` trailers and private session URLs from commits,
  PRs, code, and reports. Disable automatic session-link and coauthor
  attribution in the cloud environment before committing. Inspect the actual
  commit message, including the required literal `\n` check.
- Do not merge or push `master`, publish releases, force-push, rewrite the
  source baseline, change routing policy, or modify Arc/environment tools.
  Do not delegate additional sessions or write harness auto-memory.
- Scanner Stage 1, WAL policy, throughput contracts, audio/PipeWire hardware
  work, desktop/theme isolation, performance benchmark waves, rare-Han data,
  licensing decisions, and language-selector design are outside this series.
- Stop on quota refusal; report it without retry loops or model substitution.
  Stop the affected task on missing authority, conflicting edits, private-data
  exposure, or a material contract change.

Read [RETURN.md](RETURN.md) before beginning. A cloud delivery is ready for
local review. Local review, signing, exact-head verification, and Arc
integration determine whether it ships.

## Starting and retrieving

At `https://claude.ai/code`, select `11xx/muzaiten`, base branch
`dispatch/cloud-01`, and an environment capable of satisfying task 0. Paste
the contents of [START.md](START.md). The GitHub chat connector can read this
packet for planning; a Claude Code cloud session executes it.

From a clean checkout of this branch, the CLI equivalent is:

```sh
claude --cloud "Read docs/cloud/series-01/START.md and execute its bounded series."
```

The local lead retrieves code through the pushed branch or PR. Conversation
recovery can use `claude --teleport <session-id>` in a separate clean checkout
of the same repository and account. The local journal records the private
session reference and maps each result to its originating task.
