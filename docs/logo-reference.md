# muzaiten logo reference

Anatomy of `packaging/org.11xx.muzaiten.svg` — every element, why it's there, and the
numbers you'd touch to tweak it. The canvas is `128×128` (viewBox `0 0 128 128`); all
coordinates below are in those units.

**Concept:** Benzaiten's biwa floating on her sea, under a night sky, with her shrine
crest hanging where a moon would be. Music (the instrument, the taiko crest) × the
goddess (halo, sea, moon) = *muzaiten*. Design rule throughout: bold filled shapes with
thick dark outlines, no hairline details — the icon must survive 16 px (Paul Rand's
"recognizable at any size").

## Layer stack (back to front)

1. night sky → 2. stars + sparkles → 3. halo → 4. mitsudomoe crest → 5. sea →
6. water shadow → 7. the biwa → 8. inner rim highlight.

---

## Frame & sky

- **Rounded square** `rect 4,4 120×120 rx=26` — modern icon plate shape; the 4 px
  margin keeps grid-aligned launchers from clipping the corners.
- **`#sky` gradient** (vertical): `#3d3779 → #221f51 → #131135`. Purple-indigo night,
  warm enough to not look corporate-navy.
- **Inner rim** (last element): same rect inset, `stroke #ffffff @ 7%` — a barely-there
  gloss edge that makes the plate look finished on dark docks.

## Sky furniture

- **Stars:** four plain circles (`r 1.2–1.6`, `#cfe0ff`, varying opacity). Cheap depth;
  they can vanish at small sizes with no loss.
- **Sparkles:** two four-point "kira" stars built from 4 cubic curves each — gold
  (`#ffe9b0`) at upper-left, pale blue (`#cfe0ff`) at mid-right. The playful, non-serious
  touch; also nods to the goddess-of-fortune sparkle.
- **Halo / moon disc:** `circle (56,52) r=32`, fill `#45407f @ 50%` + gold ring
  (`#ffd98a @ 35%`). This is Buddhist-iconography shorthand: the instrument *is* the
  deity, so it gets her halo. Double-reads as a moon and (for a music app) a vinyl disc.
  Sized so the pegbox and upper body sit inside it.

## Mitsudomoe (三つ巴) — upper right, at `translate(104 36)`

Benzaiten's actual shrine crest (Itsukushima, Enoshima, Chikubushima all use tomoe
variants), painted on taiko drum heads, and read as swirling water — so one mark hits
music + water + the goddess at once. It replaced an earlier generic eighth note.

**Chirality — the two real versions.** A tomoe is a comma (*magatama*); three of them in
a ring make a *mitsudomoe*, and it comes in two mirror-image handednesses:

- **migi (right) mitsudomoe** — tails spiral **clockwise**. (Wikimedia `Mitsudomoe.svg`.)
  Cleaner and more legible at tiny sizes.
- **hidari (left) mitsudomoe** — tails spiral **counterclockwise**. (Wikimedia
  `HidariMitsudomoe.svg`, the Bessho-clan kamon.) Reads better as a moon's swirl.

This logo uses the **migi** version: small-size legibility wins, since the crest is one
of the first things to mush at 16 px. To flip to hidari, add `transform="scale(-1,1)"`
to the crest group (or negate the X in the path). An earlier pass faked the swirl with
arc-only paths and the heads were too small to recognize — this is now a proper tadpole.

**Construction** (one comma path, rotated `0°/120°/240°`, crest radius `R≈9.8`):

- head: a fat round bulb near the rim — `A 3.45 3.45 ... ` near-full circle, top at
  `(0,-9.8)`;
- tail: two cubic curves sweeping clockwise from the head inward, tapering to a point
  near the center at `(0.9, 1.2)` — this is what makes it a comma, not a dot;
- the three tails leave a small triangular swirl-hole at the center; channel width is
  tuned so the pinwheel still reads at 64 px (below that it softens toward a full moon).

Single fill `#ffd98a`, no strokes, one self-contained path per comma = no seams (the old
eighth note was three slightly-misaligned primitives).

## Sea (bottom, clipped to the frame by `clipPath #frame`)

Benzaiten is the water goddess; the biwa floats *in front of* her sea.

- **Back swell:** dark band `#24406e` with a `#5d86b8` crest line @ 60%.
- **Front swell:** `#2f6f9e` with a bold 3 px foam crest `#b9e7f7` — offset phase from
  the back band so the two read as rolling waves, not stripes.
- **Spray:** three foam dots above the crest.
- **Water shadow:** dark ellipse `(70,115)` @ 35% under the biwa — grounds the
  instrument in the water instead of floating sticker-style.

## The biwa — everything inside `rotate(-12 64 78)`

The −12° tilt gives motion and lets the bent pegbox break the halo's edge. All parts
share the `3 px #191033` outline that holds the silhouette together at tiny sizes.

- **Neck:** `rect (58.5,22) 11×32`, `#wood` gradient (`#7a4a1e → #3a1d08` — lightened
  so it doesn't melt into the sky).
- **Pegbox:** nested transform `translate(64 25) rotate(-58)` — the sharply bent-back
  pegbox is *the* biwa identifier (total ≈ 70° from vertical after both rotations).
  Box is `11×30 rx=4`.
- **Pegs — exactly 4, staggered 2+2:** a biwa has four strings (some chikuzen have
  five; three strings is the shamisen). Lateral friction pegs alternate sides at
  different heights (`y −23/−12.5` left, `−18/−7.5` right), drawn *under* the box so
  each reads as inserted from one side. An earlier draft used three full crossbars,
  which wrongly read as a 3+3 six-string guitar headstock.
- **Nut:** cream bar `#f0e0b8` at the neck top — visually anchors the strings.
- **Frets:** two `#b07a38` bars; real biwa carry 4–6 tall frets (*chū*), two is enough
  suggestion at this scale.
- **Body:** symmetric teardrop path (top `(64,46)`, widest `~(33..95, 88)`, bottom
  `(64,119)`), `#lute` gold gradient (`#ffdf7e → #eeab3f → #a96a18`) + `#shine` radial
  highlight for a lacquered-wood glow.
- **Sound hole:** one crescent (`rotate(-30)` at `(50,76)`, outer arc `r=7`, inner
  `r=8.75`) — stylized *hangetsu* (half-moon hole) doubling as a waxing moon emblem.
  Authentic biwa have a *pair*, but two crescents + the red tailpiece accidentally
  formed a sleepy face; one off-center moon killed the face and kept the motif.
- **Strings — exactly 4:** `#fff3d0`, 1.5 px, slight splay from nut to tailpiece.
- **Tailpiece:** small `14×8.5` rounded rect in vermillion `#e0492c` — the single hot
  accent (torii/shrine red) against the cool sky; ends of the strings hide under it.

## Palette

| Role | Hex |
|---|---|
| Sky | `#3d3779 / #221f51 / #131135` |
| Outline (everything) | `#191033` |
| Body gold | `#ffdf7e / #eeab3f / #a96a18` |
| Wood | `#7a4a1e / #3a1d08`, pegs `#a9712a` |
| Crest, ring, gold sparkle | `#ffd98a`, sparkle `#ffe9b0` |
| Sea | `#24406e / #2f6f9e`, foam `#b9e7f7`, crest line `#5d86b8` |
| Strings/nut | `#fff3d0 / #f0e0b8` |
| Accent red | `#e0492c` |
| Stars / blue sparkle | `#cfe0ff` |

## Size behavior & regenerating previews

- **512 px:** full story — crest swirl, pegs, frets, spray.
- **64 px:** biwa + moon-ish crest + waves; strings become shimmer.
- **16 px:** tilted gold teardrop + dark bent neck + sea band on purple — the
  irreducible silhouette.

```sh
for s in 512 64 32 16; do
  rsvg-convert -w $s -h $s packaging/org.11xx.muzaiten.svg \
    -o agent-state/logo-preview/$s.png
done
```

Compatibility note: the file sticks to plain SVG 1.1 paths, gradients and one
`clipPath` — no masks or filters — because QtSvg and some icon-theme renderers don't
support them.
