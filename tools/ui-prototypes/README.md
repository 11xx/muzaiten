# UI prototypes

Throwaway-by-design Qt programs for trying a widget layout before it is wired
into the application. Each one links Qt Widgets directly, carries fake data, has
no moc step and no CMake target, so the loop from edit to screen is about a
second instead of a full rebuild. Nothing in this directory is compiled into
`muzaiten` or covered by `make test`.

A prototype stays only while its design is unsettled. Once the real widget
ships, delete the prototype rather than maintaining a second copy of the layout.

## scrobbler-ui

The scrobbler manager as a list of per-destination rows (one-click enable
toggle, fields edited in place, per-row Test and Remove), and the
listening-history destination filter as a multi-select popup that stays open
while destinations are toggled.

```sh
tools/ui-prototypes/scrobbler-ui/build.sh              # build and run
tools/ui-prototypes/scrobbler-ui/build.sh --build-only # compile only
tools/ui-prototypes/scrobbler-ui/build.sh --shot DIR   # render each tab to DIR/tabN.png
```

`--shot` runs on the offscreen platform plugin, so it needs no display.
