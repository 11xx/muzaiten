# muzaiten

- **Stack:** Qt 6 Widgets / **C++26** / CMake+Ninja, wrapped by a `Makefile`. `make build` = incremental build; `make test` = run the suite; `make dev` = build + run against an isolated `./dev-state`; `make rebuild` = clean build (only for CMake/cache changes or stale-build symptoms).
- **Build in the repo's `build/` dir (the `make` default) and keep it** — it's shared between user and agent, so Ninja recompiles only what changed (seconds, not a full rebuild). Don't spin up throwaway `/tmp/*-build` trees; besides being slower, they pile into `/tmp`'s per-user quota (a gcc `Disk quota exceeded` writing to `/tmp/cc*.s` is *that quota*, not a code error or a full disk).
- **Verify each meaningful slice:** `make build && make test` must stay at 0 warnings/errors with all tests green.
- **Never run the app against the user's data.** The agent shares the user's UID, so a bare `./build/muzaiten`, `make run`/`make smoke`, and `make dev` all open real or dev stores that are off-limits. Run with a private, isolated root: `env QT_QPA_PLATFORM=offscreen MUZAITEN_STATE_ROOT="$PWD/agent-state" ./build/muzaiten` (`agent-state/` is gitignored and outranks `MUZAITEN_DEV_STATE`). Need real data? **Copy** it in (`cp ~/.local/share/muzaiten/library.sqlite agent-state/data/`, likewise `state.sqlite`→`agent-state/state/`, `artwork.sqlite`→`agent-state/cache/`), never open in place.

# Release documentation and tags

- Record every notable user-facing change in `CHANGELOG.md` under **Unreleased**
  before creating a release tag. Treat it as the canonical detailed change
  record; release pages should instead summarize highlights, distribution
  assets/checksums, upgrade notes, and link to the matching changelog section.
- Entries through 2026.06.20 are reconstructed development milestones from the
  `master` log, not corresponding upstream releases or date-version tags.
- Check whether README already describes a released behavior; update it only
  when its user-facing documentation is stale or incomplete.
- Create an annotated tag only on the clean, tested release commit. Use the UTC
  date as `YYYY.MM.DD`; for another release on that date, inspect existing tags
  and use the next iteration (`YYYY.MM.DD.1`, then `.2`, and so on). This
  suffix is a release iteration, never a commit count; do not derive release
  tags from the application's `YYYY.MM.DD.N.g<sha>` development identifier.

# Documentation

- `README.md` is a front page: what the app is, install, quick orientation,
  and a docs index. Keep it concise and inviting — depth belongs in `docs/`.
- `docs/` holds the guides (search, radio, playlists, controls, cli,
  state-and-paths, data-safety, features-schema, distribution). When a
  change alters user-facing behavior, update the matching guide in the same
  slice — the same rule as the changelog, and the two complement each
  other: the changelog says what changed, the guides describe the current
  behavior.
- Menu paths, key tables, and CLI verbs quoted in README/docs must match
  the shipped UI exactly; treat a stale doc as a bug, not a nice-to-have.

# Boiling pond

- Proactive Mentorship: When answering questions or implementing code, do not just answer literally. If you spot a fragile pattern (like hardcoded variables or string manipulation for IPs/paths), proactively suggest a more robust, idiomatic, or native tool/filter that achieves the goal safely.

- Assume Growth Mindset: Treat technical queries as a desire to learn the best way, not just "any way that works". If the user's implementation implies unfamiliarity with advanced ecosystem tools, introduce those tools constructively with brief, high-utility examples.
