# Data Safety

muzaiten must assume the user's music library is irreplaceable.

Version `2026.5.23` is read-only toward the music library:

- no tag writes
- no file moves
- no file renames
- no deletes
- no artwork writes into album folders
- no symlink traversal during scans
- no automatic metadata lookup that changes files

Application-owned state belongs under XDG locations:

- config: `$XDG_CONFIG_HOME/muzaiten`
- data: `$XDG_DATA_HOME/muzaiten`
- cache: `$XDG_CACHE_HOME/muzaiten`

Future tag writes must be explicit, previewed, narrowly scoped, verified after
write, and tested against disposable fixture files before they are enabled for a
real library.

