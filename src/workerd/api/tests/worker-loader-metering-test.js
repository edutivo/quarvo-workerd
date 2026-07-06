// quarvo: runtime-metering behavior suite (spec: docs/superpowers/specs/
// 2026-07-05-quarvo-runtime-metering-design.md; feature: QUARVO_RUNTIME_METERING).
//
// The same file backs two wd-test configs: worker-loader-metering-test.wd-test runs with the
// env var ON (binding expectMetering="on"), worker-loader-metering-off-test.wd-test runs stock
// (expectMetering="off") and only asserts the API is absent.
//
// Bounds here are deliberately CI-safe directional checks (shared 1-2 core runners):
//   - CPU asserts are SELF-CALIBRATING: the parent measures wall time around awaited spins
//     (parent clocks advance across awaits) and compares the child's cpuMs delta against it.
//     Child-side clocks are Spectre-frozen during execution, so the child cannot self-time.
//   - Steal asserts are differential: ~0 without interference, clearly nonzero with it.
// Tight ±tolerances live in the quiet-host e2e (quarvo/e2e/meter-overhead), per spec §8.
import assert from 'node:assert';

const SPINNER_MODULE = `
  import { WorkerEntrypoint } from "cloudflare:workers";

  // Busy-spin a fixed amount of WORK (not time — clocks are frozen during sync execution).
  // The parent calibrates how much wall time this costs on the host it runs on.
  function spinOnce(iters) {
    let x = 1;
    for (let i = 0; i < iters; i++) x = (x * 1103515245 + 12345) % 2147483648;
    return x;
  }

  export default class extends WorkerEntrypoint {
    ping() { return "pong"; }

    // One synchronous slice.
    spin(iters) { return spinOnce(iters); }

    // n slices with an event-loop yield between them, so other isolates can interleave —
    // this is the "quant A injects interference at a known rate" shape from the quarvo ask.
    async spinSlices(n, iters) {
      let x = 0;
      for (let i = 0; i < n; i++) {
        x ^= spinOnce(iters);
        await new Promise((r) => setTimeout(r, 1));
      }
      return x;
    }
  }
`;

const ECHO_MODULE = `
  import { WorkerEntrypoint } from "cloudflare:workers";

  export default class extends WorkerEntrypoint {
    ping() { return "pong"; }

    // Timer-wakeup class: one setTimeout resume per call (exact lateness metering).
    async waitTick(ms) {
      await new Promise((r) => setTimeout(r, ms));
      return 1;
    }

    // I/O-resume class: each stream chunk arrival resumes the request mid-execution
    // (non-timer, non-first-entry => busy-window estimator path).
    async drain(stream) {
      let total = 0;
      for await (const chunk of stream) {
        total += chunk.byteLength;
      }
      return total;
    }
  }
`;

const ABORT_MODULE = `
  import { WorkerEntrypoint, abortIsolate } from "cloudflare:workers";

  export default class extends WorkerEntrypoint {
    ping() { return "pong"; }
    crash() { abortIsolate("metering epoch test"); }
  }
`;

const makeCode = (mainModule) => ({
  compatibilityDate: '2025-01-01',
  compatibilityFlags: ['experimental'],
  allowExperimental: true,
  mainModule: 'worker.js',
  modules: { 'worker.js': mainModule },
});

// Calibrated so one slice is ~5-40ms depending on host speed; the tests measure rather than
// assume.
const SPIN_ITERS = 4_000_000;

// META-0: feature detection (spec §5.2; quarvo acceptance #3). The ONLY test that runs in the
// off-variant: flag off => the method must not exist and everything else must be stock.
export let featureDetection = {
  async test(ctrl, env, ctx) {
    const stub = env.loader.get('feature-detect', () => makeCode(ECHO_MODULE));
    if (env.expectMetering === 'on') {
      assert.strictEqual(typeof stub.getStats, 'function');
      const s = stub.getStats();
      for (const k of [
        'cpuMs',
        'stolenMs',
        'timerLagMs',
        'lockWaitMs',
        'resumeDelayEstMs',
        'startDelayMs',
        'epoch',
      ]) {
        assert.strictEqual(typeof s[k], 'number', `missing counter ${k}`);
      }
      assert.strictEqual(s.epoch, 1);
    } else {
      assert.strictEqual(
        typeof stub.getStats,
        'undefined',
        'flag off => getStats must not exist'
      );
    }
  },
};

// META-1: cpuMs tracks injected spin (quarvo acceptance #2). Self-calibrating: compare the
// child's cpuMs delta against parent-measured wall time spent awaiting pure-spin calls.
export let cpuMeter = {
  async test(ctrl, env, ctx) {
    if (env.expectMetering !== 'on') return;
    const stub = env.loader.get('cpu-meter', () => makeCode(SPINNER_MODULE));
    const ep = stub.getEntrypoint();

    // Warm up (module eval + first-request costs land outside the measured window).
    assert.strictEqual(await ep.ping(), 'pong');

    const before = stub.getStats();
    const t0 = Date.now();
    for (let i = 0; i < 3; i++) {
      await ep.spin(SPIN_ITERS);
    }
    const wallMs = Date.now() - t0;
    const after = stub.getStats();
    const cpuDelta = after.cpuMs - before.cpuMs;

    // The spin dominates the RPC wall time; cpu can't exceed wall (single thread) and must be
    // the bulk of it. Lower bound is loose for preemption/noise on shared runners.
    assert.ok(wallMs > 5, `spin too fast to measure (wall=${wallMs}ms); raise SPIN_ITERS`);
    assert.ok(
      cpuDelta > wallMs * 0.4,
      `cpuMs delta ${cpuDelta} should reflect ~${wallMs}ms of measured spin wall`
    );
    assert.ok(
      cpuDelta < wallMs * 1.5,
      `cpuMs delta ${cpuDelta} implausibly exceeds measured wall ${wallMs}ms`
    );
  },
};

// META-2: stolenMs differential (quarvo acceptance #1). Baseline: an echo quant alone reads
// ~0 stolen. Interference: spinner slices concurrent with echo timer waits => echo's
// timerLagMs (exact class) grows; total stolenMs is bounded by interference × concurrency.
export let stolenTimeDifferential = {
  async test(ctrl, env, ctx) {
    if (env.expectMetering !== 'on') return;
    const spinner = env.loader.get('steal-spinner', () => makeCode(SPINNER_MODULE));
    const echo = env.loader.get('steal-echo', () => makeCode(ECHO_MODULE));
    const spinEp = spinner.getEntrypoint();
    const echoEp = echo.getEntrypoint();
    await spinEp.ping();
    await echoEp.ping();

    // --- Baseline: no interference => stolen growth ~ 0. ---
    const b0 = echo.getStats();
    for (let i = 0; i < 5; i++) await echoEp.waitTick(5);
    const b1 = echo.getStats();
    const baselineGrowth = b1.stolenMs - b0.stolenMs;
    assert.ok(
      baselineGrowth < 10,
      `echo stolenMs grew ${baselineGrowth}ms with NO interference (idle steal must be ~0)`
    );

    // --- Calibrate one spinner slice, then inject interference. ---
    const tCal = Date.now();
    await spinEp.spin(SPIN_ITERS);
    const sliceMs = Math.max(1, Date.now() - tCal);

    const s0 = echo.getStats();
    const N_SLICES = 20;
    const N_TICKS = 20;
    const t0 = Date.now();
    const spinWork = spinEp.spinSlices(N_SLICES, SPIN_ITERS);
    let echoDone = (async () => {
      for (let i = 0; i < N_TICKS; i++) await echoEp.waitTick(2);
    })();
    await Promise.all([spinWork, echoDone]);
    const phaseWallMs = Date.now() - t0;
    const s1 = echo.getStats();

    const growth = s1.stolenMs - s0.stolenMs;
    // Lower bound: with ~N_SLICES slices of ~sliceMs each interleaving N_TICKS 2ms timers, a
    // meaningful fraction of ticks must land inside a slice. Keep it loose but decisively
    // above the <10ms baseline.
    assert.ok(
      growth > Math.min(25, sliceMs * 2),
      `echo stolenMs growth ${growth}ms did not register interference ` +
        `(slice=${sliceMs}ms, phase=${phaseWallMs}ms, baseline=${baselineGrowth}ms)`
    );
    // Upper bound: per-continuation accounting can multiply-count one slice across concurrent
    // waiters, but here echo runs ONE tick at a time, so growth is bounded by the phase wall.
    assert.ok(
      growth < phaseWallMs * 1.5,
      `echo stolenMs growth ${growth}ms exceeds plausible bound (~${phaseWallMs}ms phase)`
    );
    // The timer class must carry signal (exact meter), and the sum identity must hold.
    assert.ok(
      s1.timerLagMs > s0.timerLagMs,
      'timerLagMs (exact class) must grow under timer-vs-spin interference'
    );
    const sumErr = Math.abs(
      s1.stolenMs - (s1.timerLagMs + s1.lockWaitMs + s1.resumeDelayEstMs)
    );
    assert.ok(sumErr < 1e-6, `stolenMs != sum of split counters (err=${sumErr})`);
    // Spinner is the thief, not a victim: its own cpu grew, its steal stayed modest.
    const spinStats = spinner.getStats();
    assert.ok(spinStats.cpuMs > 0);
    // Echo does near-zero CPU (quarvo acceptance #2, second half).
    assert.ok(
      s1.cpuMs < Math.max(50, phaseWallMs * 0.25),
      `echo cpuMs ${s1.cpuMs} should stay near zero`
    );
  },
};

// META-3: the I/O-resume (estimator) class — stream chunks resume the child mid-request while
// a spinner interferes => resumeDelayEstMs grows (spec §4.3 busy-window estimator).
export let ioResumeEstimator = {
  async test(ctrl, env, ctx) {
    if (env.expectMetering !== 'on') return;
    const spinner = env.loader.get('io-spinner', () => makeCode(SPINNER_MODULE));
    const echo = env.loader.get('io-echo', () => makeCode(ECHO_MODULE));
    const spinEp = spinner.getEntrypoint();
    const echoEp = echo.getEntrypoint();
    await spinEp.ping();
    await echoEp.ping();

    const s0 = echo.getStats();
    const { readable, writable } = new IdentityTransformStream();
    const drained = echoEp.drain(readable);
    const spinWork = spinEp.spinSlices(15, SPIN_ITERS);
    const writer = writable.getWriter();
    const chunk = new Uint8Array(1024);
    for (let i = 0; i < 15; i++) {
      await writer.write(chunk);
      await new Promise((r) => setTimeout(r, 2));
    }
    await writer.close();
    assert.strictEqual(await drained, 15 * 1024);
    await spinWork;
    const s1 = echo.getStats();

    // Each chunk arrival is a non-timer resume; with a spinner monopolizing the thread, the
    // busy-window estimator must attribute nonzero steal to them. Directional bound only —
    // this is the estimated class (spec §4.4); the quiet-host e2e owns tighter tolerances.
    assert.ok(
      s1.resumeDelayEstMs > s0.resumeDelayEstMs,
      `resumeDelayEstMs must grow when stream resumes contend with a spinner ` +
        `(before=${s0.resumeDelayEstMs}, after=${s1.resumeDelayEstMs})`
    );
  },
};

// META-4: getStats mid-traffic neither throws nor perturbs, and counters are monotonic
// (quarvo acceptance #5).
export let readMidTraffic = {
  async test(ctrl, env, ctx) {
    if (env.expectMetering !== 'on') return;
    const echo = env.loader.get('mid-traffic', () => makeCode(ECHO_MODULE));
    const ep = echo.getEntrypoint();

    const inflight = [];
    for (let i = 0; i < 10; i++) inflight.push(ep.waitTick(5));
    let last = -1;
    for (let polls = 0; polls < 5; polls++) {
      const s = echo.getStats();
      assert.ok(s.stolenMs >= last, 'stolenMs must be monotonic');
      last = s.stolenMs;
      await new Promise((r) => setTimeout(r, 3));
    }
    const results = await Promise.all(inflight);
    assert.deepStrictEqual(results, new Array(10).fill(1));
  },
};

// META-5: epoch increments when the isolate is respawned under a live loader key (spec §5.2 —
// quarvo's M9 evict/respawn scenario, exercised via abortIsolate() like the upstream
// abortIsolateDynamic test; note an over-memoryMB-cap request does NOT evict the isolate,
// by design — see worker-loader-memory-test.js MEM-3/REARM-1).
export let epochAcrossRespawn = {
  async test(ctrl, env, ctx) {
    if (env.expectMetering !== 'on') return;
    const getStub = () => env.loader.get('epoch-test', () => makeCode(ABORT_MODULE));

    let stub = getStub();
    const ep = stub.getEntrypoint();
    assert.strictEqual(await ep.ping(), 'pong');
    const s1 = stub.getStats();
    assert.strictEqual(s1.epoch, 1);
    assert.ok(s1.cpuMs > 0, 'first epoch accumulated cpu');

    await assert.rejects(() => ep.crash());

    stub = getStub();
    assert.strictEqual(await stub.getEntrypoint().ping(), 'pong');
    const s2 = stub.getStats();
    assert.strictEqual(s2.epoch, 2, 'respawn under a live key must bump epoch');
    assert.ok(
      s2.cpuMs < s1.cpuMs + 1000,
      'fresh epoch restarts counters (sanity: not accumulated across respawn)'
    );
  },
};
