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
  const ball = f('host_pr_ball', 'number', []);
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

  // ---- throw it somewhere she is NOT ----
  const CELL = 16, W = 30, H = 20;
  let tx = px() > (W * CELL) / 2 ? 2 * CELL : (W - 3) * CELL;
  let ty = py() > (H * CELL) / 2 ? 2 * CELL : (H - 3) * CELL;
  throwBall(tx, ty);
  check('ball is in play after throw', ball() > 0, `bounces=${ball()}`);

  // ---- she should notice it and go ----
  let sawPlaying = false, bats = 0, prev = ball();
  for (let i = 0; i < 600 && ball() > 0; i++) {   // up to 600 ticks ~75s sim
    warp(0.125, 0);
    if (state() === STATE_PLAYING) sawPlaying = true;
    if (ball() !== prev) { if (ball() < prev || ball() === 0) bats++; prev = ball(); }
  }
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

  // ---- regression: a NORMAL colony must be untouched by all of this ----
  seed(7);
  boot(0);
  warp(1800, 0);
  const norm = snapshot();
  check('normal colony still founds (has a queen)', !!norm.queen_name, `queen=${norm.queen_name}`);
  check('normal colony has no ball', ball() === 0);
  const normState = state();
  check('normal colony conker never in STATE_PLAYING', normState !== STATE_PLAYING, `state=${normState}`);

  console.log(failures === 0 ? '\nALL CHECKS PASSED' : `\n${failures} CHECK(S) FAILED`);
  process.exit(failures === 0 ? 0 : 1);
});
