// Check the keeper's ball: she chases it, bats it, it runs out, and the chase
// pays in bond + relief. Run:  node host/test_ball.js   (needs a node-enabled
// build: see the sed of build_web.sh in host/README — -sENVIRONMENT=web,node).
//
// Exists because the ball is a new AntState with its own task-picker entry, and
// the last time a state was added without one it silently never ran for 5
// releases (STATE_FARMING / _target_still_valid, v177-181).
const path = process.argv[2] || 'C:/claude/emtmp/hive_test.js';
const createHiveModule = require(path);

let failures = 0;
function check(label, cond, detail) {
  const ok = !!cond;
  if (!ok) failures++;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail !== undefined ? `  (${detail})` : ''}`);
}

createHiveModule().then((M) => {
  const f = (n, ret, args) => M.cwrap(n, ret, args);
  const seed = f('host_seed', null, ['number']);
  const bootInc = f('host_boot_incubation', null, []);
  const boot = f('host_boot', null, ['number']);
  const warp = f('host_warp', null, ['number', 'number']);
  const setNow = f('host_set_now', null, ['number']);
  const throwBall = f('host_throw_ball', null, ['number', 'number']);
  const tintConker = f('host_tint_conker', null, ['number', 'number']);
  const setTitle = f('host_set_colony_title', null, ['string']);
  const buyDecor = f('host_buy_decor', 'number', ['number']);
  const bugs = f('host_bugs', 'number', []);
  const shopJson = f('host_shop_json', 'string', []);
  const ownedMask = f('host_owned', 'number', []);
  const unequip = f('host_unequip', 'number', []);
  const water = f('host_water', 'number', []);
  const bondPeakNew = f('host_bond_peak_new', 'number', []);
  const boop = f('host_boop', null, ['number', 'number']);
  const moveDecor = f('host_move_decor', 'number', ['number','number','number','number']);
  const decorAt = (x, y) => f('host_decor_at', 'number', ['number','number'])(x, y) === 1;
  const tapFb = f('host_tap', null, ['number', 'number']);
  const foodPiles = f('host_pr_foodpiles', 'number', []);
  const setTintOk = f('host_set_tint', 'number', ['number', 'number', 'number']);
  const flowering = f('host_flowering', 'number', []);
  const interactReady = f('host_interact_ready', 'number', ['number']);
  const ball = f('host_pr_ball', 'number', []);
  const ballX = f('host_pr_ball_x', 'number', []);
  const ballY = f('host_pr_ball_y', 'number', []);
  const critterX = f('host_pr_critter_x', 'number', []);
  const critterY = f('host_pr_critter_y', 'number', []);
  const roomX = f('host_room_x', 'number', []);
  const roomY = f('host_room_y', 'number', []);
  const roomW = f('host_room_w', 'number', []);
  const roomH = f('host_room_h', 'number', []);
  const state = f('host_pr_state', 'number', []);
  const bond = f('host_pr_bond', 'number', []);
  const boredom = f('host_pr_boredom', 'number', []);
  const social = f('host_pr_social', 'number', []);
  const hatched = f('host_pr_hatched', 'number', []);
  const px = f('host_pr_px', 'number', []);
  const py = f('host_pr_py', 'number', []);
  const conkers = f('host_conkers', 'number', []);
  const snapLen = f('host_snapshot', 'number', []);
  const snapPtr = f('host_snapshot_ptr', 'number', []);
  // host_snapshot() returns a BYTE LENGTH and leaves the JSON at host_snapshot_ptr().
  // Copy via HEAPU8.slice before decoding — TextDecoder refuses views onto a
  // resizable heap buffer (the Android-Chrome crash class from the v215 notes).
  const snapshot = () => {
    const n = snapLen();
    if (n <= 0) return null;
    const bytes = M.HEAPU8.slice(snapPtr(), snapPtr() + n);
    return JSON.parse(new TextDecoder().decode(bytes));
  };

  const STATE_PLAYING = 12;

  // The colony persists in MEMFS between boots (that's the real behaviour — a
  // reload restores her). Blocks that need a genuinely NEW princess have to
  // clear it first, or they inherit the previous block's bond and purse.
  const wipeColony = () => {
    const rm = (dir) => {
      let entries = [];
      try { entries = M.FS.readdir(dir); } catch { return; }
      for (const e of entries) {
        if (e === '.' || e === '..') continue;
        const p = `${dir}/${e}`;
        const st = M.FS.stat(p);
        if (M.FS.isDir(st.mode)) { rm(p); try { M.FS.rmdir(p); } catch { /**/ } }
        else { try { M.FS.unlink(p); } catch { /**/ } }
      }
    };
    rm('/colony');
  };

  // ---- hatch a princess and let her settle ----
  seed(4242);
  setNow(Math.floor(Date.now() / 1000));
  bootInc();
  warp(3600, 1);                       // an hour of attended care → hatched
  check('princess hatched', hatched() === 1 && conkers() === 1, `conkers=${conkers()}`);

  // Let her get bored and lonely so the relief is measurable.
  warp(3600, 1);
  const bore0 = boredom(), soc0 = social(), bond0 = bond();
  console.log(`      before throw: boredom=${bore0} social=${soc0} bond=${bond0}`);

  // ---- throw it a short hop away, exactly as localModule.throwBall does ----
  // (a far-corner throw is what shipped first and it landed off-screen every time)
  const FBW = 480, FBH = 320, MARGIN = 24;
  const clamp = (v, hi) => (v < MARGIN ? MARGIN : v > hi - MARGIN ? hi - MARGIN : v);
  const tx = Math.round(clamp(px() + 65, FBW));
  const ty = Math.round(clamp(py() + 40, FBH));
  throwBall(tx, ty);
  check('ball is in play after throw', ball() > 0, `bounces=${ball()}`);

  // ---- she should notice it and go ----
  // The Nest shows a 174x232 window of the 480x320 chamber, centred on her. If
  // the ball ever sits outside that, the keeper sees nothing happen — which is
  // exactly how the first version shipped. Track the worst offset all chase.
  const HALF_W = 174 / 2, HALF_H = 232 / 2;
  let sawPlaying = false, bats = 0, prev = ball();
  let worstDx = 0, worstDy = 0, maxStep = 0, totalRoll = 0;
  let lastBx = ballX(), lastBy = ballY();
  for (let i = 0; i < 600 && ball() > 0; i++) {   // up to 600 ticks ~75s sim
    warp(0.125, 0);
    if (state() === STATE_PLAYING) sawPlaying = true;
    if (ballX() >= 0) {
      worstDx = Math.max(worstDx, Math.abs(ballX() - px()));
      worstDy = Math.max(worstDy, Math.abs(ballY() - py()));
      if (lastBx >= 0) {
        const step = Math.hypot(ballX() - lastBx, ballY() - lastBy);
        maxStep = Math.max(maxStep, step);
        totalRoll += step;
      }
      lastBx = ballX(); lastBy = ballY();
    }
    if (ball() !== prev) { if (ball() < prev || ball() === 0) bats++; prev = ball(); }
  }
  check('ball stayed inside the Nest window all chase',
        worstDx <= HALF_W && worstDy <= HALF_H,
        `worst offset ${Math.round(worstDx)}x${Math.round(worstDy)}px vs ${HALF_W}x${HALF_H}`);
  // It must ROLL, not teleport: a bat should show up as a run of small
  // frame-to-frame steps, never one big jump to a new cell.
  check('ball never teleports (biggest single-tick move is a roll)',
        maxStep > 0 && maxStep < 24,
        `biggest step ${Math.round(maxStep)}px/tick`);
  check('ball actually travelled after a bat', totalRoll > 40, `rolled ${Math.round(totalRoll)}px`);
  check('she entered the chase (STATE_PLAYING)', sawPlaying);
  check('she batted it at least twice', bats >= 2, `bats=${bats}`);
  check('ball ran out and cleared', ball() === 0, `bounces=${ball()}`);

  const bore1 = boredom(), soc1 = social(), bond1 = bond();
  console.log(`      after chase:  boredom=${bore1} social=${soc1} bond=${bond1}`);
  // She self-manages boredom with zoomies, so it's usually already 0 by the time
  // we throw — the relief is applied in the same arrival block as the social
  // relief below, which IS observable, so assert the weaker invariant here.
  if (bore0 > 0) check('play relieved boredom', bore1 < bore0, `${bore0} -> ${bore1}`);
  else check('play never made her more bored', bore1 <= bore0, `${bore0} -> ${bore1}`);
  check('play relieved loneliness', soc1 < soc0, `${soc0} -> ${soc1}`);
  check('play deepened the keeper bond', bond1 > bond0, `${bond0} -> ${bond1}`);

  // ---- she must not be stuck in the chase once it's over ----
  warp(30, 0);
  check('she left STATE_PLAYING when the ball stopped', state() !== STATE_PLAYING, `state=${state()}`);

  // ---- loneliness must top out at "missing you", never full despair ----
  // The point of the plateau: opening the app after a long absence must not
  // always show a maxed bar, or the bar says nothing and just reads as cruelty.
  seed(99);
  bootInc();
  warp(3600, 1);
  warp(12 * 3600, 1);                  // half a day alone, fed but never played with
  const lonelyLater = social();
  console.log(`      loneliness after 12h alone: ${lonelyLater}`);
  check('loneliness plateaus below max', lonelyLater <= 80, `social=${lonelyLater}`);
  check('loneliness still rises enough to be worth tending', lonelyLater >= 55, `social=${lonelyLater}`);

  // ---- recolour is cosmetic and sticks ----
  const snapA = snapshot();
  const her = snapA && snapA.lilguys && snapA.lilguys[0];
  if (her) {
    const nameBefore = her.name;
    tintConker(her.id, 77);
    const snapB = snapshot();
    const her2 = snapB.lilguys.find((c) => c.id === her.id);
    check('recolour applied', her2 && her2.tint_seed === 77, `tint=${her2 && her2.tint_seed}`);
    check('recolour left her identity alone', her2 && her2.name === nameBefore, `name=${her2 && her2.name}`);
  } else {
    console.log('      (no lilguys in snapshot — skipping tint check)');
  }

  // ---- rearranging her room (#45) ----
  warp(8 * 86400, 1);                          // earn enough to buy something
  if (bugs() >= 4 && buyDecor(0) === 1) {
    // Find it: sweep the room for the piece we just bought.
    const rx2 = roomX(), ry2 = roomY(), rw2 = roomW(), rh2 = roomH();
    let at = null;
    for (let y = ry2; y < ry2 + rh2 && !at; y += 16)
      for (let x = rx2; x < rx2 + rw2 && !at; x += 16)
        if (decorAt(x, y)) at = { x, y };
    check('a bought piece is findable in her room', !!at, at ? `at ${at.x},${at.y}` : 'not found');
    if (at) {
      // Somewhere clear, well away from where it stands.
      let dest = null;
      for (let y = ry2 + 16; y < ry2 + rh2 - 16 && !dest; y += 16)
        for (let x = rx2 + 16; x < rx2 + rw2 - 16 && !dest; x += 16)
          if (!decorAt(x, y) && Math.abs(x - at.x) + Math.abs(y - at.y) > 40) dest = { x, y };
      check('moving it to a clear cell works', moveDecor(at.x, at.y, dest.x, dest.y) === 1,
            `${at.x},${at.y} -> ${dest.x},${dest.y}`);
      check('it is actually there now', decorAt(dest.x, dest.y), 'moved');
      check('and gone from where it was', !decorAt(at.x, at.y), 'vacated');
      check('moving from bare floor moves nothing', moveDecor(at.x, at.y, rx2 + 32, ry2 + 32) === 0, 'refused');
      // Outside the room would put it where she can't reach or see it.
      check('cannot be dragged outside her room',
            moveDecor(dest.x, dest.y, rx2 - 48, ry2 + 32) === 0, 'refused');
    }
  } else {
    console.log('      (could not afford a piece to move this run)');
  }

  seed(4242);
  bootInc();
  warp(3600, 1);

  // ---- the Boop tool must NOT litter her room with food ----
  seed(1234);
  bootInc();
  warp(3600, 1);
  while (foodPiles() > 0) { warp(60, 0); }     // let her clear anything lying about
  const pilesBefore = foodPiles();
  boop(8, 8);                                  // a corner she is not standing in
  check('booping empty floor drops no food', foodPiles() === pilesBefore,
        `${pilesBefore} -> ${foodPiles()}`);
  tapFb(8, 8);                                 // the Food tool still does
  check('the food tool still drops food', foodPiles() > pilesBefore,
        `${pilesBefore} -> ${foodPiles()}`);

  // ---- room colour must actually reach the renderer ----
  // It silently did nothing for a release: host_set_tint passed module id 0
  // while topology_my_id() is 1, so it never matched "me".
  check('setting the room colour reports success', setTintOk(150, 176, 124) === 1, 'applied');

  // ---- interaction cooldowns: the act always works, the PAYOFF is gated ----
  seed(808);
  bootInc();
  warp(3600, 1);
  warp(4 * 3600, 0);                          // let loneliness build up
  const lonelyBefore = social();
  const bondBefore = bond();
  water();
  const afterFirst = social();
  check('watering lifts her spirits', afterFirst < lonelyBefore, `${lonelyBefore} -> ${afterFirst}`);
  check('watering flowers her', flowering() > 0, `${flowering()}s left`);
  check('water is now resting', interactReady(2) > 0, `${interactReady(2)}s`);

  warp(600, 0);                               // 10 min — still INSIDE the 30-min cooldown
  const lonelyAgain = social();
  const bondMid = bond();
  water();
  check('a second watering does NOT lift her spirits again', social() === lonelyAgain,
        `${lonelyAgain} -> ${social()}`);
  check('...but it still builds the bond a little', bond() >= bondMid, `${bondMid} -> ${bond()}`);
  check('...and it still works (she flowers again)', flowering() > 0, `${flowering()}s`);
  check('bond grew overall across both', bond() > bondBefore, `${bondBefore} -> ${bond()}`);

  seed(4242);
  bootInc();
  warp(3600, 1);

  // ---- her room: she must never leave it, and it must not starve ----
  // The whole point of the static view is that everything is on screen. If she
  // can walk out, the keeper watches an empty room; if critters spawn outside,
  // the life (and, later, the shop currency) happens where she can't reach it.
  seed(31337);
  bootInc();
  warp(3600, 1);
  const rx = roomX(), ry = roomY(), rw = roomW(), rh = roomH();
  console.log(`      room: ${rw}x${rh}px at ${rx},${ry}`);
  check('room is portrait and smaller than the chamber', rw < 480 && rh < 320 && rh > rw, `${rw}x${rh}`);

  let strayed = null;
  for (let i = 0; i < 4000; i++) {          // ~8 sim-minutes of ordinary pottering
    warp(0.125, 1);
    if (i % 40 === 0) throwBall(px() + 40, py() + 40);   // keep her moving about
    const x = px(), y = py();
    if (x < rx - 8 || x > rx + rw + 8 || y < ry - 8 || y > ry + rh + 8) { strayed = `${x},${y}`; break; }
  }
  check('she never leaves her room', strayed === null, strayed ? `strayed to ${strayed}` : 'stayed in');

  // Visitors must land INSIDE the room. The room is about a quarter of the
  // chamber, so an unclamped spawn would put most bugs where she can neither
  // see nor catch them — and they're the shop's entire income.
  let seen = 0, outside = 0, worst = null;
  for (let i = 0; i < 3000; i++) {
    warp(2, 1);                              // ~100 sim-minutes total
    const cx = critterX(), cy = critterY();
    if (cx < 0) continue;
    seen++;
    if (cx < rx || cx > rx + rw || cy < ry || cy > ry + rh) { outside++; worst = `${cx},${cy}`; }
  }
  console.log(`      critter sightings: ${seen}, outside the room: ${outside}`);
  check('visitors were actually seen', seen > 0, `${seen} sightings`);
  check('every visitor was inside her room', outside === 0, worst ? `e.g. ${worst} vs room ${rx},${ry} ${rw}x${rh}` : 'all in');

  // ---- a normal colony must still have the WHOLE chamber ----
  seed(7);
  boot(0);
  const fullW = roomW(), fullH = roomH();
  check('a colony still gets the full chamber', fullW === 480 && fullH === 320, `${fullW}x${fullH}`);

  seed(4242);
  bootInc();
  warp(3600, 1);

  // ---- the decor shop ----
  // The two rules that matter: you can't buy what you can't afford, and
  // spending must never touch the LIFETIME catch tally the Bug Hunter title
  // reads from (that would make decorating demote her).
  seed(555);
  bootInc();
  const bugsAtBoot = bugs();
  console.log(`      purse restored at boot: ${bugsAtBoot}`);
  // Affordability is the rule that matters, and it has to hold either way round.
  const DEAREST = 5, DEAREST_PRICE = 15;
  const r = buyDecor(DEAREST);
  if (bugsAtBoot < DEAREST_PRICE) check('a purchase beyond the purse is refused', r === 0, `purse ${bugsAtBoot} < ${DEAREST_PRICE}`);
  else check('a purchase within the purse is allowed', r === 1, `purse ${bugsAtBoot}`);
  check('a refused purchase costs nothing', bugs() === (r === 1 ? bugsAtBoot - DEAREST_PRICE : bugsAtBoot));
  warp(3600, 1);

  warp(7 * 86400, 1);                       // a week of care — she catches things
  const earned = bugs();
  console.log(`      bugs after a week of care: ${earned}`);
  check('a week of catching earns something', earned > 0, `bugs=${earned}`);
  check('but not a runaway pile (v201 economy stays cut)', earned < 120, `bugs=${earned}`);

  const snapBefore = snapshot();
  const catchesBefore = (snapBefore.lilguys?.[0]?.catches) ?? 0;
  if (earned >= 4) {
    const ok = buyDecor(0);
    check('bought the cheapest item', ok === 1, `returned ${ok}`);
    check('purse was debited', bugs() === earned - 4, `${earned} -> ${bugs()}`);
    const snapAfter = snapshot();
    const catchesAfter = (snapAfter.lilguys?.[0]?.catches) ?? 0;
    check('spending did NOT touch her lifetime catch tally (Bug Hunter safe)',
          catchesAfter === catchesBefore, `${catchesBefore} -> ${catchesAfter}`);
    check('snapshot reports the purse', typeof snapAfter.bugs === 'number', `bugs=${snapAfter.bugs}`);
  } else {
    console.log('      (not enough bugs earned to test a purchase this run)');
  }
  check('unknown item id is refused', buyDecor(99) === 0, 'refused');

  // Wearables: she carries ONE, so buying another swaps it rather than stacking.
  const shop = JSON.parse(shopJson());
  const wearables = shop.filter((i) => i.wear !== 0);
  check('shop sells things to carry as well as place', wearables.length >= 4, `${wearables.length} wearables`);
  warp(10 * 86400, 1);                      // enough purse for a couple of keepsakes
  const w1 = wearables[0], w2 = wearables[wearables.length - 1];
  if (bugs() >= w1.price + w2.price) {
    buyDecor(w1.id);
    const worn1 = (snapshot().lilguys?.[0]?.accessory) ?? null;
    check('she carries what was bought', !!worn1, `accessory=${worn1}`);
    buyDecor(w2.id);
    const worn2 = (snapshot().lilguys?.[0]?.accessory) ?? null;
    check('a second keepsake swaps, never stacks', !!worn2 && worn2 !== worn1, `${worn1} -> ${worn2}`);
    // You pay once: swapping BACK to something you own must be free, or buying a
    // second keepsake silently costs you the first.
    const purseBefore = bugs();
    buyDecor(w1.id);
    const wornBack = (snapshot().lilguys?.[0]?.accessory) ?? null;
    check('swapping back to an owned keepsake is free', bugs() === purseBefore, `${purseBefore} -> ${bugs()}`);
    check('...and she is carrying it again', wornBack === worn1, `${wornBack}`);
    check('owned bitmask records both', (ownedMask() & (1 << w1.id)) !== 0 && (ownedMask() & (1 << w2.id)) !== 0,
          `mask=${ownedMask()}`);
    check('putting it down leaves her carrying nothing', unequip() === 1 && !(snapshot().lilguys?.[0]?.accessory));
  } else {
    console.log(`      (purse ${bugs()} too small to test swapping)`);
  }

  seed(4242);
  bootInc();
  warp(3600, 1);

  // ---- colony title (#40): a display name that never becomes an address ----
  const idBefore = (snapshot() || {}).colony_id;
  setTitle('Ambers Lot  ');
  const titled = snapshot();
  // trailing spaces trimmed, so " " can't masquerade as a name and hide the id
  check('colony title applied + trimmed', titled.title === 'Ambers Lot', `title=${JSON.stringify(titled.title)}`);
  check('colony_id is NOT changed by titling', titled.colony_id === idBefore, `${idBefore} -> ${titled.colony_id}`);

  setTitle('');
  const cleared = snapshot();
  check('blank title falls back to the id', !cleared.title, `title=${JSON.stringify(cleared.title)}`);
  check('colony_id survives clearing', cleared.colony_id === idBefore);

  setTitle('   ');
  check('whitespace-only title cannot masquerade as a name', !(snapshot().title || '').trim());

  // Non-ASCII is stripped (the record is a fixed char buffer, and the glass font
  // is CP437) — a smart-quoted name must degrade, never corrupt the manifest.
  setTitle('Amber’s Lot');
  check('non-ASCII stripped from title', snapshot().title === 'Ambers Lot', `title=${JSON.stringify(snapshot().title)}`);

  // ---- regression: a NORMAL colony must be untouched by all of this ----
  seed(7);
  boot(0);
  warp(1800, 0);
  const norm = snapshot();
  check('normal colony still founds (has a queen)', !!norm.queen_name, `queen=${norm.queen_name}`);
  check('normal colony has no ball', ball() === 0);
  const normState = state();
  check('normal colony conker never in STATE_PLAYING', normState !== STATE_PLAYING, `state=${normState}`);

  // ---- full bond fires its celebration exactly once, ever ----
  // NB: no "before full bond" check here. The registry keeps a RAM cache that
  // outlives a file wipe, so blocks in one process share a princess — and asking
  // bondPeakNew() early would CONSUME the one-shot we're about to assert.
  wipeColony();
  seed(2468);
  bootInc();
  warp(3600, 1);
  for (let i = 0; i < 60 && bond() < 100; i++) {   // care for her until she's devoted
    water();
    warp(2 * 3600, 1);
  }
  check('a well-loved princess reaches full bond', bond() >= 100, `bond=${bond()}`);
  check('the celebration fires', bondPeakNew() === 1, 'fired');
  check('...and never fires again', bondPeakNew() === 0, 'latched');
  // The latch persists, so a reload can't replay it.
  bootInc();
  check('...not even after a reload', bondPeakNew() === 0, 'still latched');

  seed(4321);
  bootInc();


  console.log(failures === 0 ? '\nALL CHECKS PASSED' : `\n${failures} CHECK(S) FAILED`);
  process.exit(failures === 0 ? 0 : 1);
});
