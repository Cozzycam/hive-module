# Hive Module — Pixel Art Commission Brief

## What this is

A physical desk companion: a small 3D-printed module with a 3.5" screen housing a
living virtual ant colony. Little creatures ("Conkers") hatch, work, nap, make best
friends, hold vigils for their dead, chase fireflies, and survive real weather —
the colony lives on the device 24/7 and the keeper checks in like tending a
terrarium. Think *cozy ant farm meets Tamagotchi meets Dwarf Fortress*.

The art style is **chunky warm pixel art**: sprites are authored tiny and
integer-upscaled ~2× on a 480×320 screen, so every pixel reads. Existing
placeholder art is functional but programmer-made — your job is to make the
colony's cast feel *alive and loveable* within the constraints below.

## Hard constraints (the contract — art delivered this way plugs straight in)

| Constraint | Value |
|---|---|
| Canvas | Exact sizes below, per item. Author at native size — no downscaling |
| Colors | Any RGB, but **no anti-aliasing at the sprite edge** and **no pure magenta** |
| Transparency | Pure magenta `#FF00FF` background (colorkey) — hard edges only |
| Facing | **One facing only** (facing right). The engine mirrors for left |
| Frames | Separate image per frame (PNG per frame is fine; we convert) |
| Naming | `worker_walk_a.png`, `worker_walk_b.png`, `critter_moth_a.png` … |
| Ceiling | Nothing renders larger than 96×96 on screen (engine limit — respected by the sizes below) |

### The body-tone rule (workers + queen only)

Every Conker gets its **own unique hue applied by the engine at runtime** — your
art supplies *shape and shading only*. Paint worker/queen bodies in a **neutral
desaturated warm brown ramp** and the engine re-colors each pixel by its
brightness:

- shadow ≈ `#4A3B2F` · base/midtone ≈ `#665441` (aim mid-luma ~98/255) · highlight ≈ `#8C7660`
- Use **3–5 ramp steps**, no hue variation within the body (hue gets replaced), no gradients
- **Eyes are exempt**: use exactly `#E6EFD6` (bright) and `#CEE7AD` (shaded) for
  eye highlights — the engine preserves these two values verbatim so eyes stay bright
- Critters, emotes, props, decor are **full-colour** — paint them as they should look

## Deliverables

### A. Worker Conker — 16×16, the star (priority 1)

The current worker is 10×10; you get a 16×16 canvas (~2.5× the pixels) rendered at
the same on-screen size (~32px). One character, many states. Movement bob/waddle
is procedural — you supply poses, not motion in-betweens.

| Frame | Notes |
|---|---|
| `idle` | Neutral stand. The workhorse frame |
| `walk_a`, `walk_b` | Leg alternation; engine flips between them while moving |
| `carry` | Hauling a food bundle on their back (bundle is part of the frame, in **full colour** — warm honey/amber) |
| `groom` | Leaning in, tending a friend |
| `sleep` | Curled up (engine floats Zz above) |
| `mourn` | Head bowed, antennae drooped — used at vigils for dead friends. Make it quietly sad |
| `celebrate` | A little jump/cheer — earned a title, made a best friend |
| `eat` | Munching |

**9 frames @ 16×16, neutral body ramp + exempt eye colours.**

### B. Emote glyphs — 8×8, full colour (priority 2)

Float above heads. Must read at 1× on a 480×320 screen from arm's length.

`heart` (love/afterglow) · `z` (drowsy) · `music_note` (playing) · `question` ·
`exclaim` · `dots` (bored …) · `gloom_cloud` (lonely — small grey raincloud) ·
`sparkle` (title earned) · `tear` (grief) · `star` (best friends)

**10 glyphs @ 8×8.**

### C. Critters — full colour, 2 frames each (priority 3)

The collection game. Native sizes; rendered ~1.5–2×.

| Critter | Canvas | Frames | Character |
|---|---|---|---|
| Butterfly | 10×8 | wings open / closed | warm pink-orange, cheerful |
| Beetle | 8×6 | walk a/b | dark bronze, busy |
| Worm | 8×5 | wriggle a/b | dusty pink, humble |
| Firefly | 6×6 | glow bright / dim | golden glow — night star |
| Moth | 10×8 | wings open / closed | dusty grey-cream, gentle — night visitor |
| Snail | 8×7 | 1–2 (it's a snail) | brown shell with whorl, rain-glisten |
| Ladybird | 6×6 | walk a/b | red + black dots — the lucky one |
| Dragonfly | 12×8 | wings shimmer a/b | teal dart — the rare garden prize |

**~15 frames total.**

### D. Environment & milestone decor — full colour (priority 4)

| Item | Canvas | Notes |
|---|---|---|
| Food pile ×3 states | 12×8 each | small / half / full mound — seeds & berries, warm |
| Wild sprout | 8×8 | little green shoot (garden food spawns) |
| Mossy stone | 12×8 | milestone: 25 workers born |
| Cairn | 10×9 | milestone: survived first challenge — 3 stacked pebbles |
| Wildflower | 8×10 | milestone: first best friends — one sweet flower |
| Golden seed | 7×5 | milestone: Bug Hunter crowned — treasure gleam |
| Egg | 4×4 | pearly |
| Seed (growing brood) | 6×6 | 2 growth stages if inspiration strikes |

### E. Queen — 44×44 (priority 5)

Regal but warm; same neutral-ramp + eye rule as workers. `idle_a`, `idle_b`
(subtle breathe), `lay` (egg-laying pose). **3 frames @ 44×44.**

## Animation timing

The engine renders at 30fps and flips frames itself (walk alternates ~4×/sec;
critter wings ~2–4×/sec). You never need to deliver timing — poses only.

## Don'ts

- No anti-aliasing against the transparent background (colorkey = hard edges)
- No magenta anywhere in visible art
- No hue variation inside worker/queen bodies (the engine replaces hue)
- No baked drop shadows outside the silhouette
- No text in sprites

## References

- `design/sprites/AMBERS_GUY.json`, `design/sprites/MIPPY.json` — 16×16 character
  concepts in the intended direction (viewable in our web sprite editor)
- Photos/video of the live device available on request — ask and we'll shoot the
  current colony so you can see scale, palette and vibe in situ

## Delivery & rounds

PNG per frame, native size, magenta background, zipped with the naming scheme
above. We convert and flash within a day — you'll get photos of your art
*running on the real device* for revision rounds. Suggested phasing: **A + B
first** (the colony's soul), then C, then D/E.
