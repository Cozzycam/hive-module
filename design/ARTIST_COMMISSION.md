# Hive Module — Pixel Art Commission Brief

> **Revised 2026-07-06.** Added **D3. Keepsake hats** — conkers now craft little
> hats for their best friends, worn in the *maker's* colour. This is a live,
> prominent feature (most of the colony is wearing one) currently running on
> programmer-made placeholder shapes, so it's real art we need. Everything else
> below is unchanged; the worker (A) and emotes (B) remain the priority.

## What this is

A physical desk companion: a small 3D-printed module with a 3.5" screen housing a
living virtual ant colony. Little creatures ("Conkers") hatch, work, nap, make best
friends, craft keepsakes and little hats for the friends they love, hold vigils for
their dead, chase fireflies, and survive real weather — the colony lives on the
device 24/7 and the keeper checks in like tending a terrarium. Think *cozy ant farm
meets Tamagotchi meets Dwarf Fortress*.

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

### The body-tone rule (workers, queen, artifacts & hats)

Several things get their **hue applied by the engine at runtime** — your art
supplies *shape and shading only*. This covers worker/queen bodies (each conker's
own hue), the crafted artifacts in D2, and the keepsake hats in D3 (the *maker's*
hue). Paint them all in a **neutral desaturated warm brown ramp** and the engine
re-colors each pixel by its brightness:

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
| `sow` | Leant over the soil, planting — farming is a real profession (the colony's gardeners emerge from personality and earn "the Green-Thumbed") |

**10 frames @ 16×16, neutral body ramp + exempt eye colours.**

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
| Garden crop ×4 stages | soil bed 12×8; plant up to 10×14 | tilled soil bed → sprout → growing → mature-with-berries. The garden module's crops grow through these over real hours — the keeper literally watches them rise, so each stage should read as *progress*. Mature = the payoff frame |
| Mossy stone | 12×8 | milestone: 25 workers born |
| Cairn | 10×9 | milestone: survived first challenge — 3 stacked pebbles |
| Wildflower | 8×10 | milestone: first best friends — one sweet flower |
| Golden seed | 7×5 | milestone: Bug Hunter crowned — treasure gleam |
| Egg | 4×4 | pearly |
| Seed (growing brood) | 6×6 | 2 growth stages if inspiration strikes |

### D2. Artifacts — the works conkers make (priority 4, alongside D)

Conkers craft objects that stand in the chamber permanently. **Sculptures,
cairns and paintings follow the same neutral-ramp rule as worker bodies** —
the engine re-colours each piece into its *maker's* personal hue, so one
sprite renders as every artist's own work. Memorials keep fixed colours.

| Item | Canvas | Palette | Notes |
|---|---|---|---|
| Sculpture ×3 motifs | ~10×12 each | neutral ramp | orb / spire / arch — small statues on plinths. Dignified but charming |
| Cairn | 10×9 | neutral ramp | stacked stones, hand-balanced feel |
| Floor painting ×2-3 motifs | 10×10 | neutral ramp | pigment pressed into the ground — pattern, not picture |
| Memorial stone | 10×10 | **fixed** | a quiet grave marker with a small flower. Carved by a grieving friend, bears a name in the app — the most emotionally loaded sprite in the whole commission. Understated beats ornate |

### D3. Keepsake hats — worn gifts (priority 4, alongside D)

When two conkers become best friends, the muse sometimes moves one to make the
other a **little hat** — a gift, worn on the head for as long as the friendship
lasts (and *forever*, as a memorial, if the maker dies while they're still
friends). Most of the colony ends up wearing one, so these are on screen
constantly, sitting just above the head.

**The maker's-colour rule (important):** a hat is rendered in the *maker's* own
personal hue, not the wearer's — the same neutral-ramp trick as worker bodies and
artifacts (see the body-tone rule and D2). Paint each hat in the **neutral
desaturated warm-brown ramp**, no hue of its own, and the engine re-colours the
whole hat into whoever made it. So Plum's gift reads as Plum's colour on whoever's
head it lands. This is the whole point of the feature — *you can tell who made a
hat by its colour* — so keep them **fully ramp-shaded with no fixed-colour accents**
(a baked gold bud, say, would fight the recolour).

| Item | Canvas | Palette | Notes |
|---|---|---|---|
| Petal hat | 12×9 | neutral ramp | a wide flower-brim with a small petal crown — soft and spring-like. The default, most common gift |
| Seed cap | 10×9 | neutral ramp | a snug rounded dome-cap with a tiny sprout/leaf poking from the top — cute, close-fitting |
| Grass hat | 14×8 | neutral ramp | a wide woven sun-brim (think little farmhand's straw hat) with a stray blade or two — the broadest of the three |

**3 hats @ their sizes, neutral ramp.** Filenames: `hat_petal.png`,
`hat_seed_cap.png`, `hat_grass.png`. Design them to sit **above** the head (the
engine composites them on top of the worker sprite — you don't bake them into the
worker frames), reading clearly against the chamber floor. They scale with the
wearer, so a design that holds up small is ideal.

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
first** (the colony's soul), then C, then D (decor + artifacts + the D3 hats)
and E. The three hats are small and self-contained — a quick, high-impact win if
you want an early morale boost, since the whole colony wears them.
