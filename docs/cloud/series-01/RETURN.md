# Return contract

Commit a concise `docs/cloud/series-01/results/report.md` and
`docs/cloud/series-01/results/receipt.json` alongside the delivered source.
Push the cloud session's assigned branch. A draft PR targets
`dispatch/cloud-01`; PR creation failure must not prevent branch delivery.

The report records:

- Source baseline, packet commit, result branch, and final source-change head.
- Each task's outcome: repaired, investigated with no confirmed defect,
  contract decision required, blocked, or not started. Name its commits.
- Exact fixture discrimination and baseline/final outcomes, with command
  exits, warning/error totals, test totals/skips, and environment identity.
- Changed guides/changelog sections and any unresolved material risk.
- Declared/observed implementation model and effort when established; omit
  unavailable coordinates. Never use placeholders, claim an independent
  review, or assert local acceptance.

Use a receipt with `schema: "cloud-dispatch-receipt/1"`, `series`,
`source_base`, `packet_commit`, `result_branch`, `source_head`, and `tasks`.
Each task has `id`, `outcome`, `commits`, `checks`, and `open_findings`.
Each check identifies the tested revision, command, exit code, environment,
and whether evidence is observed, unavailable, or only declared. A report
commit necessarily follows `source_head`; distinguish those coordinates.
The receipt is an untrusted delivery claim for local inspection, not input to
Arc import or an authorization record.

Keep raw logs and fixture databases under ignored private state. Include
sanitized useful summaries and hashes in the report, with detailed logs
available for local retrieval if needed. Do not commit archives, binaries,
state stores, service credentials, home paths, private network/device
identifiers, private session links, or raw transcripts. Preserve a failing
witness; never label an omitted check as passed.

The local lead fetches the delivered branch into a separate checkout, compares
its ancestry and diff with this packet, captures private provenance in Arc,
signs retained history locally, reviews the content, reruns exact-head gates,
and integrates selected source commits into local `master`. Packet/report
commits stay on the dispatch branch unless separately selected. Publication
to remote `master` follows local acceptance.
