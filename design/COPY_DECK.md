# Hive — Copy Deck

Every piece of written / menu text in the app, on the module glass, and in the
push notifications, pulled out so you can rewrite each line in your own voice.

**How to use:** each table has a **Your version** column — fill it in (or strike a
line to keep it). `{name}`, `{critter}` etc. are values filled in at runtime; keep
the braces where you want the value to land. Emoji shown inline are part of the
current string — keep, change, or drop as you like.

File references (e.g. `Home.tsx:129`) point at where the string lives, so a change
is easy to land afterwards.

---

## 1. Navigation & global chrome  (`App.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Tab bar labels | `Home` · `Module` · `Characters` · `Chambers` · `Diary` · `Feedback` | |
| First load | `Connecting to colony...` | |
| Discovery toast (a conker finds a critter) | `{emoji} {finder} found a {critter}! Tap to see.` | |
| Deed toast (a page of the Annals is written) | `📜 A page of the Annals is written: {title}. Tap to read.` | |
| Gift toast (neighbour care package) | `🎁 A neighbouring kingdom sent a care package! Tap to see it.` | |
| Population-cap toast | `🥚 Your eggs lie dormant for now — more space is needed.` | |
| Firmware update ready | `📡 Module firmware v{n} is ready — tap to update over WiFi` | |
| Firmware updating | `🔄 Updating the wee guys' brains… they'll blink and reboot` | |

---

## 2. Onboarding / empty state  (`Empty.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Headline | `The colony hasn't begun yet.` | |
| Sub-text | `This app is a window onto a Hive Module. The colony lives on the modules themselves — connect to a queen module to see your colony here.` | |
| Primary button | `Run a queen module on this phone` | |
| Primary button (starting) | `starting your colony…` | |
| Secondary button | `Connect to a physical module` | |
| Tertiary link | `Or browse the demo colony` | |
| **Connect modal** — title | `Choose your colony` | |
| Connect modal — sub | `The name is shown on your module's screen, in the bar at the top.` | |
| Connect modal — searching | `Looking for colonies…` | |
| Connect modal — none found | `No colonies found yet — give the module a minute after setup.` | |
| Connect modal — per-colony recency | `active just now` / `active {n}m ago` / `active {n}h ago` / `active {n}d ago` / `never seen` | |
| Advanced toggle | `advanced (local IP)` / `hide advanced` | |
| Advanced IP field placeholder | `Queen's local IP (optional)` | |
| Cancel | `Cancel` | |

---

## 3. Module tab (your own on-phone colony)  (`Module.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Status — loading | `warming up your colony…` | |
| Status — error | `could not start the module` | |
| Status — running | `tap to boop or feed · hold to gather · syncing to the hive` | |

---

## 4. Home screen  (`Home.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Fallback colony name (no name set) | `The Colony` | |
| Header subline | `Queen {name} · {Season} · {weather} · {phase}` | |
| **Right Now** tile heading | `Right Now` | |
| Stat labels | `Population` · `Workers` · `Brood` · `Food` · `Burn rate` · `Reserves` | |
| Care package button (ready) | `Send a care package 🎁` | |
| Care package button (sending / sent / cooldown) | `Sending…` / `Package on its way 🎁` / `Care package sent recently` | |
| Care package caption | `a day's food for the whole colony` | |
| **Field Guide** card title / sub | `Field Guide` / `Every visitor the colony has spotted` | |
| **The Chronicle** card title / sub | `The Chronicle` / `The colony's story, deeds and renown` | |
| **Gallery** card title / sub | `Gallery` / `Works the colony has made` | |
| Modules card | `{n} module(s)` / `{n} online` | |
| Active challenge tag | `Active Challenge` — then `{challenge type} · severity {n}%` | |
| Section headers | `Following` · `Notable Now` | |
| Settings button aria-label | `Settings` | |
| **Wish card** — heading | `{name} is craving a treat` / `{name} wants a boop` | |
| Wish card — idle sub | `A little kindness goes in the diary — it would be the {nth} you've answered.` | |
| Wish card — granted | `Granted — they felt that. The {nth} kindness, written down. ✨` | |
| Wish card — failed | `Couldn't reach the colony — try again.` | |
| Wish card — button | `Grant` | |
| **The colony approaches…** heading | `The colony approaches…` | |
| **On this day** heading / extra | `On this day` / `…and one more remembrance.` | |

---

## 5. Characters — list  (`Characters.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Title | `Characters` | |
| Filter chips | `Alive` · `Deceased` · `All` | |
| Queen card — name / sub | `Queen {name}` / `Mother of the colony · reigning {n} days` | |
| Empty (alive filter) | `No living characters found` | |
| Empty (other) | `No characters found` | |
| Deceased tag | `(deceased)` | |

### Epithets (the "{name} the ___" titles)  (`biography.ts`)

Earned titles win over personality flavour. Order = precedence.

| Trigger | Current epithet | Your version |
|---|---|---|
| catcher trait | `the Bug Hunter` | |
| 2+ survival traits | `the Unbreakable` | |
| survived storm | `the Storm-Tested` | |
| survived heatwave | `the Sun-Scorched` | |
| survived cold snap | `the Frost-Hardy` | |
| survived drought | `the Dust-Weathered` | |
| 3+ works crafted | `the Maker` | |
| 10+ crops sown | `the Green-Thumbed` | |
| 5+ parades led | `the Parade-Leader` | |
| 10+ discoveries | `the Keen-Eyed` | |
| elder | `the Elder` | |
| high bravery / low | `the Bold` / `the Timid` | |
| high playfulness / low | `the Merry` / `the Solemn` | |
| high exploration / low | `the Wanderer` / `the Homebird` | |
| high work tempo / low | `the Tireless` / `the Dozy` | |
| high social / low | `the Friendly` / `the Quiet` | |
| high appetite / low | `the Hungry` / `the Sparrow` | |

### Role tags (the one-liner under a name)  (`personality.ts`)

Compound: `The {adjective} {noun}`, from the two strongest personality dims.

| Dim | noun / adjective | Your version |
|---|---|---|
| work tempo | `grafter` / `busy` | |
| exploration | `explorer` / `curious` | |
| route stickiness | `homebody` / `homely` | |
| social | `socialite` / `sociable` | |
| appetite | `forager` / `hungry` | |
| hardiness | `tough nut` / `tough` | |
| playfulness | `joker` / `playful` | |
| bravery | `daredevil` / `brave` | |
| flat personality | `The all-rounder` | |
| pioneer trait | `Pioneer` | |
| elder trait | `Elder` | |

---

## 6. Character profile  (`Characters.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Rename pill | `✎ Rename` | |
| Rename prompt | `New name for {name} (letters, max 15):` | |
| Rename sent | `Sent! {name} will become "{new}" within a minute.` | |
| Rename failed | `Could not reach the colony server — try again later.` | |
| Deceased suffix | ` · Deceased` | |
| Card headings | `Mood & needs` · `Right now` · `Keepsake` · `Traits` · `Personality` · `Bonds` · `Lineage` · `Life Story` | |
| Need bars | `Restlessness` · `Tiredness` · `Loneliness` | |
| No personality (not on LAN) | `Personality data available when connected to the queen (LAN)` | |
| Lineage line | `Tended by {name}` | |
| Memorial (starvation) | `Perished from starvation after {age}.` | |
| Memorial (natural) | `Lived {age}. Died of natural causes.` | |
| Stat labels | `Age` · `Role` | |

### Mood blurbs  (`Characters.tsx`)

| Mood | Current blurb | Your version |
|---|---|---|
| content | `Happy as they are right now.` | |
| restless | `Getting fidgety — could use something to do.` | |
| bored | `Properly bored. Give them a boop, or watch them go looking for fun.` | |
| playing | `Having a great time!` | |
| sleepy | `Worn out — heading for bed. A boop will rouse them if you must.` | |
| happy | `Just had a lovely time — content and glowing.` | |
| lonely | `Feeling a bit alone — would love some company.` | |

### Activity — title + blurb (blurb is prefixed with the conker's name)  (`Characters.tsx`)

| Activity | Title | Blurb (`{name} …`) | Your version |
|---|---|---|---|
| idling | `Pottering about` | `is pottering about with nothing much to do.` | |
| sleeping | `Fast asleep` | `is fast asleep, dreaming of crumbs.` | |
| napping | `Having a nap` | `is curled up for a little daytime nap.` | |
| seeking_company | `Looking for company` | `is feeling a bit alone, off to find a friend to huddle with.` | |
| foraging | `Foraging` | `is out on the trail, foraging for food.` | |
| carrying_food | `Carrying food home` | `is hauling a hard-won morsel back to the nest.` | |
| heading_home | `Heading home` | `is ambling back home.` | |
| eating | `Having a bite` | `is tucking into a well-earned meal.` | |
| chasing_firefly | `Chasing a firefly` | `is darting through the dark, chasing a firefly.` | |
| playing | `Playing chase` | `is full of beans, zooming about after a friend.` | |
| parading | `Leading a parade` | `is leading a merry little parade around the chamber.` | |
| sowing | `Sowing a seed` | `is down in the soil, pressing a fresh seed into a plot.` | |
| gardening | `Minding the garden` | `is minding the garden, waiting for a bare patch to sow.` | |
| heading_to_garden | `Off to the garden` | `is making the long walk over to tend the garden next door.` | |
| tending_brood | `Tending the brood` | `is fussing over the eggs and grubs in the nursery.` | |
| feeding_queen | `Feeding the queen` | `is bringing the queen her royal ration.` | |
| crafting | `Crafting` | `is lost in the muse, crafting a little keepsake.` | |
| mourning | `In mourning` | `is standing vigil for a friend who has passed.` | |
| clearing | `Tidying up` | `is quietly clearing away what the colony can reclaim.` | |
| away | `Away` | `is off in another chamber, out of sight for now.` | |

### Keepsake lines  (`Characters.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Keepsake title | `A {kind}, worn with pride` / `A {kind}, worn for life` | |
| Memorial keepsake (maker known) | `Made by {name}. Worn in their memory, always.` | |
| Memorial keepsake (maker unknown) | `Made by a best friend, now gone. Worn in their memory.` | |
| Gift keepsake (maker known) | `Made for {name} by {maker}.` | |
| Gift keepsake (maker unknown) | `A gift from a best friend.` | |

### Trait descriptions  (`traits.ts`)

| Trait | Label | Description | Your version |
|---|---|---|---|
| pioneer | `Pioneer` | `First to step into an unexplored chamber.` | |
| elder | `Elder` | `Has lived to a grand old age — the young ones huddle close.` | |
| bonded | `Bonded` | `Made a true friend by spending lots of time side by side. Friendships can fade if they drift apart — or be lost when a friend dies — but the badge is theirs to keep.` | |
| survived_heatwave | `Heatwave Survivor` | `Came through a scorching spell.` | |
| survived_cold_snap | `Cold Snap Survivor` | `Endured a bitter freeze.` | |
| survived_drought | `Drought Survivor` | `Outlasted the hungry, dry days.` | |
| survived_storm | `Storm Survivor` | `Weathered a great storm.` | |
| catcher | `Bug Hunter` | `The colony's reigning critter-catcher — a title that only passes to whoever out-catches the current champion.` | |

### Bonds phrasing  (`Characters.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Bonded trait, mutual | `Best friends with {names}.` | |
| Bonded trait, one-way | `Has a soft spot for {names}.` | |
| Bond list, mutual | `best friends` | |
| Bond list, one-way | `has a soft spot for` | |

---

## 7. Personality dimensions  (`personality.ts`)

Shown on the profile under "what does this mean?". Label + low/high poles + the
plain-English meaning.

| Dim | Label | Low pole | High pole | Meaning | Your version |
|---|---|---|---|---|---|
| work_tempo | `Energy` | `Dozy` | `Busy bee` | `How hard they work. Busy bees forage more and rest less; dozy ones nap and potter.` | |
| exploration | `Curiosity` | `Homebird` | `Adventurer` | `How far they roam. Adventurers find food first — and are the ones who start the zoomies.` | |
| route_stickiness | `Habits` | `Free spirit` | `Routine lover` | `Routine lovers walk the same trusty trails every time; free spirits try new ways.` | |
| social_frequency | `Friendliness` | `Quiet one` | `Social butterfly` | `How often they greet, share food and play. The friendly ones make best friends fastest.` | |
| appetite | `Appetite` | `Light eater` | `Big eater` | `Big eaters snack sooner and keep food for themselves; light eaters share their load with hungry nestmates.` | |
| hardiness | `Toughness` | `Soft shell` | `Tough nut` | `Tough nuts shrug off heatwaves, cold snaps and hungry days better than most.` | |
| playfulness | `Playfulness` | `Serious` | `Playful` | `Playful ones romp and zoom for the joy of it; serious ones only play when they get really bored.` | |
| bravery | `Bravery` | `Timid` | `Brave` | `Brave ones chase fireflies and lead the parades; timid ones keep near the queen and follow along.` | |
| Between poles | `In between` | | | (leaning label when mid-range) | |
| One-line phrase (flat) | `A bit of everything` | | | | |

---

## 8. Life Story lines  (`biography.ts`)

The biography on a character's page, oldest first.

| Beat | Current copy | Your version |
|---|---|---|
| Birth (founder) | `Hatched in the founding days of the colony.` | |
| Birth (normal) | `Hatched.` | |
| Raised by | `Raised by {name} — and takes after them.` | |
| Took a shine | `Took a shine to {name}.` | |
| Best friends | `Became best friends with {name}.` | |
| Drifted apart | `Drifted apart from {name}.` | |
| Trait earned | `Earned "{trait}".` | |
| Vigil | `Stood vigil for {name}.` | |
| Anniversary visit | `Visited {name}'s stone, remembering.` | |
| Carved memorial | `Carved a memorial for {name}.` | |
| Made a gift | `Made a {kind} for {name}.` | |
| Passed away | `Passed away at {n} days old, of a grand old age — {name} was at their side.` (starvation → `hungry to the last`) | |
| Discoveries (agg) | `Has found {list} so far.` | |
| Parades (agg) | `Once led a parade around the chamber.` / `Has led {n} parades around the chamber.` | |
| Sows (agg) | `Sowed their first crop in the garden.` / `Has sown {n} crops in the garden.` | |
| Works (agg) | `Made something that will outlast the day.` / `Has made {n} works — the chamber carries their colours.` | |
| Wishes (agg) | `Once made a wish — and the keeper answered.` / `Has had {n} wishes answered by the keeper.` | |
| Mourned by | `Mourned by {names}.` | |

---

## 9. Diary  (`Diary.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Title | `Diary` | |
| Refresh button | `Refresh` / `Refreshing...` | |
| Category chips | `All` · `Births` · `Deaths` · `Milestones` · `Social` · `Discoveries` · `Challenges` · `Traits` | |
| Day labels | `Today` · `Yesterday` · (else weekday+date) | |
| Empty | `No events to show` | |

### Diary entry lines

| Event | Current copy | Your version |
|---|---|---|
| hatch | `{name} was born.` (founder → ` — a founder`) | |
| death | `{name} passed away, with {name} at their side. Cause: starvation.` (parts optional) | |
| role change | `{name} changed role.` | |
| bond formed | `{name} took a shine to {name}.` | |
| best friends | `{name} and {name} became best friends.` | |
| bond broken | `{name}'s bond with {name} faded.` | |
| milestone | `Colony milestone: {kind} ({value}).` | |
| colony event | `Colony: {kind}.` | |
| discovery | `{name} found a {critter}!` | |
| challenge start | `{type} began (severity {n}%).` | |
| challenge end | `{type} ended.` | |
| trait earned | `{name} earned the "{trait}" trait.` | |
| caretaker assigned | `{name} was assigned a caretaker: {name}.` | |
| vigil | `{name} stood vigil for {name}.` | |
| anniversary | `{name} visited {name}'s stone — remembering.` | |
| parade | `{name} led a parade — {n} joined in.` | |
| crop sown | `{name} sowed a crop in the garden.` | |
| crafted (work) | `{name} finished a {kind}, made {context phrase}.` | |
| crafted (gift) | `{name} made a {kind} for {name}.` | |
| crafted (memorial) | `{name} carved a memorial for {name}.` | |
| art weathered | `{name}'s old {kind} finally crumbled away.` | |
| wish (treat) | `{name} wished for a treat — and the keeper provided.` | |
| wish (boop) | `{name} wanted a boop — and got one, from beyond the glass.` | |

### Diary rollups (one line per kind, per day)

| Rollup | Current copy | Your version |
|---|---|---|
| Foraging (1 / many) | `A foraging haul came in ({n}u).` / `{n} foraging hauls brought in {n}u.` | |
| Parades (1 / many) | `A parade romped through the chamber.` / `{n} parades romped through the chamber.` | |
| Discoveries | `The colony found {list}.` | |
| Keeper feeds (1 / many) | `The keeper dropped in a helping of food.` / `The keeper dropped in {n} helpings of food.` | |
| Sows (1 / many) | `{name} sowed a crop in the garden.` / `{n} crops were sown in the garden.` | |
| Tunnel flicker (few / many) | `A neighbour module connected.` / `The tunnel to a neighbour flickered {n} times.` | |

---

## 10. Field Guide  (`FieldGuide.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Title | `Field Guide` | |
| Header (none / some) | `No visitors spotted yet — keep watch on warm days.` / `{n} of {total} visitors spotted.` | |
| Reigning champ suffix | ` · Reigning Bug Hunter: {name}` | |
| Undiscovered name | `???` | |
| List: seen count | ` · seen {n} time(s)` | |
| List: first/last seen | `First spotted by {name}, {date} · last seen {date}` | |
| Close-up stat labels | `spotted` · `first seen` · `last seen` | |
| Close-up first-finder | `First spotted by {name}.` | |
| Close-up recent | `Recent sightings` | |
| Rarity label | `{Rarity} visitor` (+ ⭐ if Rare) | |
| Footer (list) | `Rarer visitors may find their way in as the colony grows…` | |
| Footer (close-up) | `Variants with rarer colourings are rumoured…` | |

### Critter blurbs + hints

| Critter | Rarity | Blurb | Hint (when unseen) | Your version |
|---|---|---|---|---|
| Butterfly | Common | `Flutters in on warm, clear days and drifts among the wee guys.` | `Visits by day, in fair weather. Gardens see the most.` | |
| Beetle | Common | `Trundles through on important beetle business.` | `Visits by day, in fair weather.` | |
| Worm | Common | `Surfaces for a wander between the grains.` | `Visits by day — likes the ground soft.` | |
| Firefly | Uncommon | `Glimmers over the chamber after dusk. The brave give chase — whoever catches the most holds the Bug Hunter title.` | `Only after dark. Watch for the chase!` | |
| Moth | Uncommon | `A dusty grey flier that drifts in when the chamber sleeps.` | `Clear nights only — most keepers never see one arrive.` | |
| Snail | Uncommon | `Takes its sweet time crossing the chamber, rain glistening on its shell.` | `Only ventures out in the rain.` | |
| Ladybird | Rare | `A lucky little dome of red. The colony considers a visit a blessing.` | `A rare fair-weather visitor. Keep watch.` | |
| Dragonfly | Rare | `A teal dart that shimmers over the garden on warm days. The rarest prize.` | `Only ever seen over a garden module, on warm clear days.` | |

---

## 11. Gallery  (`Gallery.tsx`, `artworks.ts`)

| Where / when | Current copy | Your version |
|---|---|---|
| Title | `Gallery` | |
| Sub (empty) | `Nothing made yet — the muse visits when the pantry is deep.` | |
| Sub (has works) | `{n} works standing · {n} ever made.` | |
| Work title | `{maker}'s {kind}` (+ ` · crumbled` if weathered) | |
| Footer | `Works stand in the chamber in their maker's colours. When the chamber fills, the oldest weather away.` | |

### Work descriptions (form + provenance)  (`artworks.ts`)

`{prov}` = provenance phrase (see below).

| Form | Current copy | Your version |
|---|---|---|
| orb | `A rounded orb, shaped by {maker} {prov}.` | |
| spire | `A slender spire, raised by {maker} {prov}.` | |
| arch | `A little arch to pass beneath, built by {maker} {prov}.` | |
| cairn | `A stack of balanced stones, piled by {maker} {prov}.` | |
| memorial | `A carved stone, set by {maker} {prov}.` | |
| paint (diamonds) | `Pigment pressed into the floor in a scatter of diamonds, by {maker} {prov}.` | |
| paint (meander) | `Pigment pressed into the floor in winding trails, by {maker} {prov}.` | |
| lost sculpture | `A sculpture whose form is lost to time, shaped by {maker} {prov}.` | |
| lost painting | `A floor painting whose pattern is lost to time, made by {maker} {prov}.` | |
| petal hat | `A petal hat, woven by {maker} {prov}.` | |
| seed cap | `A seed cap, shaped by {maker} {prov}.` | |
| grass hat | `A grass hat, plaited by {maker} {prov}.` | |

### Provenance phrases  (`artworks.ts`)

| Context | Current copy | Your version |
|---|---|---|
| in memory | `in memory of {name}` | |
| storm | `as a storm battered the colony` | |
| heatwave | `in a scorching spell` | |
| rain | `on a rain-soaked day` | |
| night | `by firefly light` | |
| grief | `in a time of grief` | |
| plenty (default) | `in a time of plenty` | |
| accessory (for someone) | `for {name}` / `as a gift` | |

---

## 12. The Chronicle  (`Saga.tsx`, `chronicle.ts`, `saga.ts`)

### Screen chrome  (`Saga.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Title | `The Chronicle of {colony}` | |
| Sub | `As the chronicle tells it.` | |
| Segments | `Saga` · `Deeds` · `Renown` | |
| Saga empty | `The story hasn't started yet.` | |
| Saga footer | `New chapters write themselves as the weeks turn.` | |
| Deeds — marginalia heading | `Marginalia` | |
| Deeds — marginalia line | `The chronicle notes: {kind} ({value}) — {date}.` | |
| Deeds — truncation note | `The chronicle's records begin {date}. Earlier days live on in the colony's own memory.` | |
| Renown — young colony | `The colony is young. Renown is earned, not given — the first pages are blank on purpose.` | |
| Renown — observances heading | `Observances` | |
| Renown — observances empty | `The calendar is quiet for now; the first anniversaries are still on their way.` | |
| Arc labels | `I — The Founding` · `II — The Growing Season` · `III — The Trials` · `IV — The Long Memory` · `V — Renown` | |

### Saga chapters — composed prose  (`saga.ts`)

Chapter titles:

| Trigger | Current title | Your version |
|---|---|---|
| deaths that week | `The Week We Lost {names}` | |
| challenge | `The Week the {Challenge} Came` | |
| best friends | `The Week of Friendships` | |
| gifts | `The Week of Gifts` | |
| works | `The Week the Muse Visited` | |
| births | `The Week of New Faces` | |
| week 0 | `The Founding` | |
| nothing notable | `A Quiet Week` | |
| (all prefixed) | `Chapter {n} — {title}` | |

Paragraph lines:

| Beat | Current copy | Your version |
|---|---|---|
| Opener (wk 0) | `In the colony's first week,` | |
| Opener (later) | `As the {ordinal} week opened,` | |
| Births (one) | `{opener} {name} hatched.` (wk0 → `hatched into the founding days`) | |
| Births (many) | `{opener} {n} new faces hatched: {names}.` | |
| Founders tag (wk0) | ` These were the founders — the ones every later story leads back to.` | |
| No births (wk0) | `In the colony's first week, the queen laid her founding eggs and the chamber waited.` | |
| No births (later) | `The {ordinal} week came in quietly.` | |
| Challenge | `A {kind} battered/gripped the colony — and passed. {n} came through it, and will not forget.` | |
| Death | `The week took {name} — {callback} in a hungry season, with {name} at their side. {names} stood vigil. {name} carved the memorial that stands still.` | |
| Best friends (one) | `{a} and {b} became inseparable.` | |
| Best friends (many) | `Friendships sealed themselves: {pairs}.` | |
| Drifted apart | `{pairs} drifted apart.` | |
| Bug Hunter (once) | `{name} took the Bug Hunter title.` | |
| Bug Hunter (thrash) | `The Bug Hunter title changed hands {n} times — {name} held it when the week closed.` | |
| Works (one) | `The muse visited {name}, whose {kind} joined the chamber.` | |
| Works (many) | `The makers were busy: {names} left new works in the chamber.` | |
| Gift | `{name} made a {kind} for {name} — worn ever since.` | |
| Anniversary | `{name} walked to {name}'s stone again, as they do.` | |
| Ambient (visitors) | `{n} visitors were spotted` / `visitors beyond counting drifted through` | |
| Ambient (first critter) | `{name} found the colony's first {critter} — the Field Guide begins with them.` | |
| Ambient (join) | `Between times: {bits}.` (bits: `{n} crops sown in the garden`, `parades most days`, `{n} care packages from beyond the border`) | |
| Annals page woven in | `It was this week the Annals gained a page: {title} — {inscription}.` | |
| Era turn | `So ended {era}. What came after, the colony would call {era}.` | |

### Deeds — titles, inscriptions, and "not yet" hints  (`chronicle.ts`)

Recorded deeds read `{title} — {inscription}, {date}.` Rumoured ones show the hint.

| Deed | Title | Inscription (recorded) | Hint (rumoured) | Your version |
|---|---|---|---|---|
| first-hatching | `The First Hatching` | `{name} hatched, first of them all` | (auto) | |
| founders-assembled | `The Founders Assembled` | `{name} opened their eyes, and the {n} founders stood together` | | |
| first-shine | `First Shine` | `{name} took first shine — to {name}` | | |
| first-haul | `The First Haul` | `{name} carried home first haul` | | |
| keeper-provides | `The Keeper Provides` | `the keeper fed the colony by hand for the first time` | | |
| new-cache | `A New Cache` | `{name} found food no one had known about` | | |
| first-visitor | `The First Visitor` | `{name} found first {critter}` | | |
| first-parade | `The First Parade` | `{name} led first parade, {n} strong` | | |
| garden-wakes | `The Garden Wakes` | `{name} pressed first seed into the earth` | | |
| wish-answered | `A Wish, Answered` | `{name} wished, and the keeper answered` | | |
| best-friends | `Best Friends` | `{a} and {b} became true friends` | | |
| name-earned | `A Name Earned` | `{name} earned "{trait}" — the first name given` | | |
| muse-arrives | `The Muse Arrives` | `the muse visited {name}, who left a {kind} in the chamber` | | |
| gift-given | `A Gift Given` | `{name} made a {kind} for {name}` | | |
| beyond-first-chamber | `Beyond the First Chamber` | `{name} crossed into a new chamber, first of them all` | | |
| guide-half | `The Guide Half-Full` | (count-based) | `{N} kinds of visitor have signed the Guide; the rest remain strangers.` | |
| guide-complete | `The Complete Guide` | (all critters) | | |
| ten-strong | `Ten Strong` | `the colony numbered ten — a page ruled for a crowd` | `The colony is {n} strong; the chronicler rules a page for ten.` | |
| raised-by-hand | `Raised by Hand` | `{name} took {name} under a wing` | | |
| third-generation | `The Third Generation` | `{name}, once raised by hand, took {name} under a wing in turn` | | |
| colony-held | `The Colony Held` | `the {kind} passed, and the colony still stood` | | |
| storm-tested | `Storm-Tested` | `{name} came through the storm — and will not forget` | `No storm has yet been outlasted.` | |
| sun-scorched | `Sun-Scorched` | `{name} came through the heatwave …` | `No heatwave has yet been outlasted.` | |
| frost-hardy | `Frost-Hardy` | `{name} came through the cold snap …` | `No cold snap has yet been outlasted.` | |
| dust-weathered | `Dust-Weathered` | `{name} came through the drought …` | `No drought has yet been outlasted.` | |
| all-four-trials | `All Four Trials` | `heat, cold, drought and storm — all four trials endured` | | |
| one-unbreakable | `One Unbreakable` | `{name} survived their second trial — the Unbreakable walks among us` | | |
| first-loss | `The First Loss` | `{name} was first loss` | | |
| first-vigil | `The First Vigil` | `{name} stood vigil for {name}` | | |
| stone-raised | `A Stone Raised` | `{name} carved a stone for {name}` | `No stone yet stands for {name}.` | |
| remembering-day | `Remembering Day` | `{name} walked back to {name}'s stone, remembering` | | |
| long-remembered | `Long Remembered` | `{name} still walks to {name}'s stone, {n} days on` | | |
| peaceful-end | `A Peaceful End` | `{name} passed peacefully, {name} at their side` | | |
| elder-among-us | `An Elder Among Us` | `{name} grew old — the colony's first elder` | | |
| hundredth-visitor | `The Hundredth Visitor` | `the hundredth visitor was written into the Guide` | `The Guide records {n} sightings; the chronicler awaits the hundredth.` | |
| gallery | `A Gallery` | `five works stood in the chamber at once — a gallery` | `{N} works stand in the chamber.` | |
| weathered-not-lost | `Weathered, Not Lost` | `a {kind} faded back into the earth — nothing made here is truly lost` | | |
| title-lineage | `A Title With a Lineage` | `the Bug Hunter title passed to {name}, its third holder` | `The Bug Hunter title has known {n} holders.` | |
| neighbours | `Neighbours` | `word came from beyond the border — a care package from neighbours` | | |
| hundred-days | `A Hundred Days` | `one hundred days since the founding` | `The colony is {n} days old.` | |
| year-of-days | `One Year of Days` | `a full year of days, written down` | `The colony is {n} days old; a year is a long story.` | |
| guide new kind | (marginal) | `the Guide's {nth} kind of visitor signed its pages` | | |

### Renown — family standings  (`chronicle.ts`)

Each "family" is a reputation track. `{standing}` is the current earned phrase;
`{nextHint}` teases the next tier.

| Family | Label | Tier phrases (low→high) | Your version |
|---|---|---|---|
| Makers | `Makers` | `where the muse visits` → `a colony of makers` → `the muse lives here` | |
| Gardeners | `Gardeners` | `green-handed` → `a garden colony` → `where the earth answers` | |
| Rememberers | `Rememberers` | `a colony that remembers` → `long-memoried` → `where no one is forgotten` | |
| Survivors | (survival) | `tested once` → `weatherworn` → `unbowed by seasons` | |
| Naturalists | `Naturalists` | `curious-eyed` → `friends to small things` → `keepers of the complete Guide` | |
| Beloved | (kindness) | `wished-upon and answered` → `well-kept` → `beloved of its keeper` | |
| Companions | `Companions` | `where friendship took root` → `a colony of friends` → `bound tight as roots` | |

Next-tier hints (examples):

| Where / when | Current copy | Your version |
|---|---|---|
| Counter families | `{N} more {noun} and {colony} will be "{phrase}".` | |
| Rememberers (first) | `A stone raised and a remembering walk, and {colony} will be "a colony that remembers".` | |
| Rememberers (mid) | `{N} more remembering walk(s) and {colony} will be "long-memoried".` | |
| Rememberers (top) | `When memory proves itself long, {colony} will be "where no one is forgotten".` | |
| Naturalists | `{N} more kind(s) of visitor and {colony} will be "{phrase}".` | |

### Eras  (`chronicle.ts`)

| Era | Current name | Your version |
|---|---|---|
| founding | `The Founding Days` | |
| after first loss | `The Days After {name}` | |
| after a trial survived | `The Weatherworn Days` | |
| colony doubled | `The Crowded Days` | |
| after neighbours | `The Days of Neighbours` | |
| year turns | `The Second Year`, `The Third Year`, … | |

Era summary sentence (composed): joins clauses like `one was lost`, `two hatched`,
`the garden gave three crops`, `the muse visited twice`, `a friendship sealed
itself`, `a trial was endured`.

### Observances (calendar / "On this day")  (`chronicle.ts`)

| Occasion | Current copy | Your version |
|---|---|---|
| Anniversary walk today | `Today {name} walked to {name}'s stone. It has been {n} days.` | |
| Month-mark of a death (1) | `It is a month to the day since the colony lost {name}.` | |
| Month-mark of a death (n) | `It is {n} months to the day since the colony lost {name}.` | |
| Colony age (weeks) | `The colony is {n} weeks old today.` | |
| Colony age (days) | `The colony is {n} days old today.` | |
| Founding Day | `Founding Day — {n} year(s).` | |
| Deed anniversary | `A year ago today, {inscription}.` / `{N} years ago today, {inscription}.` | |

---

## 13. Settings  (`Settings.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Back / Title | `← Back` / `Settings` | |
| **Notifications** heading | `Notifications` | |
| Intro | `Get a buzz on your phone when something happens — a bug discovered, a gift from next door — even when the app is closed.` | |
| On state | `✓ Phone notifications are on` | |
| Test button | `Send a test notification` | |
| Test result (fail) | `Couldn't reach the server — try again.` | |
| Test result (no devices) | `The server has no registered devices — re-pick a preference below.` | |
| Test result (ok) | `Sent to {n} device(s) — should buzz any second.` | |
| iOS hint | `On iPhone, add this app to your Home Screen first (Share → Add to Home Screen), then come back here to switch it on.` | |
| Enable button | `Enable phone notifications` / `Enabling…` | |
| Blocked | `Notifications are blocked — turn them on for this site in your browser/phone settings.` | |
| Error | `Couldn't enable just now — give it another tap.` | |
| **Notification tiers** | `Off` — `Silence.` | |
| | `Milestones only` — `The colony will speak when something matters.` | |
| | `All notable events` — `Hear everything the colony chooses to highlight.` | |
| **Connection** heading | `Connection` | |
| Rows | `Colony ID` · `Queen LAN IP` · `VPS` · `Data source` · `Last update` | |
| Data source values | `LAN (freshest)` · `VPS (cloud cache)` · `Cached (offline)` · `Not connected` | |
| Buttons | `Change module` · `Disconnect` | |
| Connect inline fields | `Queen LAN IP (optional)` · `Colony ID (auto-detect)` | |
| Connect error | `Could not detect a colony.` | |
| Connect buttons | `Cancel` · `Connect` / `Testing...` | |
| **About this colony** heading | `About this colony` | |
| Rows | `Founded` · `Age` · `Population` · `Total workers (ever)` | |
| Raw data link | `View raw data →` | |

---

## 14. Feedback  (`Feedback.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Title | `Feedback` | |
| Intro | `Spotted something odd? Wish the conkers did something? Tell us — it goes straight to the workshop.` | |
| Textarea placeholder | `What happened? What would make it better?` | |
| Sent | `Sent — thank you! 🌰` | |
| Failed | `Couldn't send — try again in a moment.` | |
| Send button | `Send` / `Sending…` | |
| Footer | `Sent with your note: colony {id}, firmware v{n}, app v{n}. No account, no personal data.` | |

---

## 15. Chambers  (`Chambers.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Title / sub | `Chambers` / `{n} modules · {n} online` | |
| Layout card heading | `Colony Layout` | |
| Pheromones toggle | `Pheromones` | |
| Module status | `online` / `offline` | |

### Module roles

| Role | Label | Blurb | Your version |
|---|---|---|---|
| queen | `Queen Module` | `Heart of the colony — source of truth.` | |
| satellite | `Satellite` | `Plain extension chamber.` | |
| garden | `Garden` | `A growing place — wild food sprouts here, and critters visit far more often. The colony's best spot for discoveries.` | |
| food_store | `Food Storage` | `Deep larder. Behaviours coming soon.` | |
| heart_tree | `Heart Tree` | `The colony's living memory. Behaviours coming soon.` | |
| foreign_queen | `Neighbour Kingdom` | `Another colony's queen — borders are politely closed.` | |

### Module detail

| Where / when | Current copy | Your version |
|---|---|---|
| Role heading | `Role` | |
| Role queued | `Role change queued — the queen applies it within ~30s.` | |
| Role send failed | `Couldn't reach the colony — try again in a moment.` | |
| **Ground Tint** heading | `Ground Tint` | |
| Tint names | `Sand` · `Clay` · `Moss` · `Honey` · `Slate` · `Lavender` · `Rose` | |
| Tint queued | `Tint queued — the ground changes within ~30s.` | |
| **Royal Diplomacy** heading | `Royal Diplomacy` | |
| Diplomacy blurb | `Borders stay closed, but a care package may cross. Their queen decides how it feeds her colony.` | |
| Gift button (ready) | `Send them a care package 🎁` | |
| Gift button (states) | `Sending…` / `Caravan dispatched to the border 🎁` / `A gift was sent recently` | |
| **Connections** heading | `Connections` | |
| No connections | `No connections` | |

---

## 16. "While you were away" digest  (`digest.ts`, `DigestCard.tsx`)

| Where / when | Current copy | Your version |
|---|---|---|
| Card heading | `While you were away` | |
| Away label | `in the last hour` / `in the last {n} hours` / `since yesterday` / `over the last {n} days` | |
| Dismiss | `Caught up` | |

Digest lines:

| Beat | Current copy | Your version |
|---|---|---|
| Birth (one) | `{name} hatched while you were away.` | |
| Births (many) | `{names} hatched.` | |
| Death (natural) | `{name} passed away peacefully.` | |
| Death (starved) | `{name} starved. The colony needs food.` | |
| Challenge | `A {challenge struck/passed}.` | |
| Foraging (with top) | `{n} foraging trips brought in {n}u — {name} worked hardest ({n} trips).` | |
| Foraging (simple) | `{n} foraging trip(s) brought in {n}u.` | |
| Best friends | `{a} and {b} became best friends.` | |
| More friendships | `…and {n} more new friendships.` | |
| Parade (one) | `{name} led a parade — {n} joined in.` | |
| Parades (many) | `{n} parades wound through the chamber. Spirits are high.` | |
| Trait | `{name} earned "{trait}".` | |
| Milestone | `Milestone: {m}.` | |

---

## 17. On the module glass (firmware)  (`hud.cpp`, `renderer.cpp`)

### HUD strip

| Where / when | Current copy | Your version |
|---|---|---|
| Population label | `Conkers` | |
| Day counter label | `Day` | |
| Food label | `days food` | |
| Day-phase labels | `night` · `dawn` · `day` · `dusk` | |
| Weather labels | `clear` · `cloudy` · `overcast` · `fog` · `drizzle` · `rain` · `heavy rain` · `snow` · `storm` | |
| Phase labels | `founding` · `growing` · `mature` | |
| Version | `v{n}` | |

### Story-beat banners (the line that scrolls under the HUD)

| Beat | Current copy | Your version |
|---|---|---|
| Best friends | `{name} & {name} are best friends` | |
| Trait earned | `{name} earned '{trait}'` | |
| Vigil (named) | `{name} stands vigil for {name}` | |
| Vigil (unnamed) | `{name} stands vigil for a friend` | |
| Heatwave start | `A heatwave bakes the colony!` | |
| Cold snap start | `A cold snap grips the colony!` | |
| Drought start | `A drought parches the colony!` | |
| Storm start | `A storm batters the colony!` | |
| Heatwave end | `The heatwave has broken` | |
| Cold snap end | `The cold snap has thawed` | |
| Drought end | `The drought has broken` | |
| Storm end | `The storm has passed` | |
| Crop sown | `{name} sowed a crop in the garden` | |
| Memorial carved | `{name} carved a memorial for {name}` | |
| Gift made | `{name} made {a petal hat/…} for {name}` | |
| Work finished | `{name} finished {a sculpture/cairn/painting/…}` | |
| Art crumbled | `{name}'s old {kind} crumbled away` | |
| Keepsake set aside | `{name} quietly sets the {keepsake} aside` | |
| Keepsake worn (named) | `{name} will always wear {name}'s gift` | |
| Keepsake worn (self) | `{name} will wear it always` | |
| Trait names (glass) | `Pioneer` · `Elder` · `Bonded` · `Heatwave Survivor` · `Cold Snap Survivor` · `Drought Survivor` · `Storm Survivor` · `Bug Hunter` | |

---

## 18. Push notifications  (`vps/server.js`)

| Trigger | Current copy | Your version |
|---|---|---|
| Hatch | `🌱 {name} hatched!` | |
| Death | `🌂 {name} passed away.` | |
| Best friends | `⭐ {name} & {name} are best friends now!` | |
| Trait | `🏅 {name} earned "{label}"!` | |
| Challenge start | `⚠️ A {kind} has hit the colony — they could use a care package.` | |
| Challenge end | `☀️ The {kind} has passed. The survivors will remember.` | |
| Discovery | `{name} found a {critter}!` | |
| Neighbour gift | `A neighbouring kingdom sent a care package!` | |
| Eggs dormant | `Your eggs lie dormant for now — more space is needed.` | |
| Notification title (default) | `Hive` | |
| Test push | `🐜 Test — the colony can reach you!` | |

### Evening digest ("Today in the colony", 8pm)  (`vps/server.js`)

| Where / when | Current copy | Your version |
|---|---|---|
| Push title | `Today in {colony}` | |
| Death fragment | `🌂 we said goodbye to {names}` | |
| Best friends fragment | `⭐ {name} & {name} became best friends` | |
| Trait fragment | `🏅 {name} earned "{label}"` | |
| Hatch fragment (one) | `🌱 {name} hatched` | |
| Hatch fragment (many) | `🌱 {n} little ones hatched` | |
| Challenge fragment | `⚠️ a {kind} struck` | |
| Discoveries fragment | `🔍 {n} critters found` | |
| Parades fragment | `🎉 {n} parades` | |
| Quiet day (nothing) | `A quiet, contented day in the colony.` | |
