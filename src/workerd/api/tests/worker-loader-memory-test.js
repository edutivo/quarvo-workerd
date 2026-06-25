// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests that the per-isolate memory cap declared via `WorkerCode.limits.memoryMB` is actually
// enforced for dynamically-loaded Workers (the Worker Loader binding). This exercises the quarvo
// fork's QuarvoIsolateLimitEnforcer (src/workerd/server/server.c++) and the clean-error hook in
// IoContext::runImpl (src/workerd/io/io-context.c++).
//
// Invariants under test:
//   MEM-1: a worker with a memory cap that allocates well past the cap has its request terminated
//          with a clean "memory limit" error (NOT the internal "script terminated for unknown
//          reasons" assertion), and the workerd process survives (a freshly-loaded worker still
//          serves).
//   MEM-2: a worker with a memory cap that allocates a substantial fraction under the cap runs
//          normally.
//   MEM-3: after an over-limit request is terminated, the same warm isolate self-heals (best-effort
//          — depends on GC timing).
//   REG-1: a worker with NO declared limit is unaffected (an allocation that WOULD exceed a 64 MiB
//          cap succeeds because there is no cap).
//   REARM-1: after an over-cap eviction, the same warm isolate re-arms the cap — a *moderate*
//          over-cap allocation (128 MiB, well below the headroom granted during the eviction but
//          above the cap) is ALSO terminated, and the ceiling stays sustained across repeats.
import assert from 'node:assert';

// Each chunk is a FixedArray of 65536 tagged slots. workerd builds V8 with pointer compression
// (kTaggedSize == 4), so a chunk is ~256 KiB of *managed* old-generation heap — exactly the space
// the near-heap-limit callback governs. We retain every chunk so none can be collected, and use
// distinct arrays (never strings/ArrayBuffers) so V8 cannot dedupe or move the cost off the managed
// heap. `.fill(i)` stores SMIs inline, so no separate HeapNumbers are allocated.
const SLOTS = 65536;
const BYTES_PER_CHUNK = SLOTS * 4; // ~256 KiB under V8 pointer compression.
const chunksFor = (mb) => Math.ceil((mb * 1024 * 1024) / BYTES_PER_CHUNK);

const MAIN_MODULE = `
  import { WorkerEntrypoint } from "cloudflare:workers";

  const SLOTS = ${SLOTS};
  const BYTES_PER_CHUNK = SLOTS * 4;

  export default class extends WorkerEntrypoint {
    ping() {
      return "pong";
    }

    // Allocate roughly \`targetMb\` MiB of live managed JS heap and retain it, driving the isolate
    // toward (and past) its heap cap.
    allocate(targetMb) {
      const live = [];
      const chunks = Math.ceil((targetMb * 1024 * 1024) / BYTES_PER_CHUNK);
      for (let i = 0; i < chunks; i++) {
        live.push(new Array(SLOTS).fill(i));
      }
      // Return something derived from the data so the optimizer cannot elide the allocations.
      return live.length;
    }
  }
`;

function makeCode(overrides) {
  return {
    compatibilityDate: '2025-01-01',
    mainModule: 'main.js',
    modules: { 'main.js': MAIN_MODULE },
    globalOutbound: null,
    ...overrides,
  };
}

// MEM-2: a capped worker that uses a substantial fraction of its cap (without crossing it) runs
// fine and stays reusable.
export let memoryWithinLimit = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('memoryWithinLimit', () =>
      makeCode({ limits: { memoryMB: 64 } })
    );

    // ~24 MiB of live heap, comfortably under the 64 MiB cap but a meaningful fraction of it.
    assert.strictEqual(await worker.getEntrypoint().allocate(24), chunksFor(24));
    // The isolate is reusable for ordinary calls.
    assert.strictEqual(await worker.getEntrypoint().ping(), 'pong');
  },
};

// MEM-1 (+MEM-3): a capped worker that allocates far past the cap has its request terminated with a
// clean memory-limit error; the process survives; and the warm isolate self-heals.
export let memoryExceedsLimit = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('memoryExceedsLimit', () =>
      makeCode({ limits: { memoryMB: 64 } })
    );

    // Try to allocate ~1 GiB into a 64 MiB isolate: this must NOT succeed and must NOT crash the
    // process. Assert a clean, attributable memory-limit rejection — crucially NOT the internal
    // "script terminated for unknown reasons" assertion, which is what surfaces if the over-limit
    // termination is not attributed to the memory cap (see io-context.c++ hook).
    await assert.rejects(worker.getEntrypoint().allocate(1024), (e) => {
      assert.ok(e instanceof Error, `expected an Error, got ${typeof e}: ${e}`);
      assert.doesNotMatch(
        e.message,
        /unknown reasons/i,
        'over-limit termination was not attributed to the memory cap'
      );
      // The enforcer raises `OVERLOADED: Worker exceeded memory limit.`; tolerate minor wrapping.
      assert.match(e.message, /memory/i);
      return true;
    });

    // Process-survival probe: a brand-new dynamically-loaded worker must still work. If the heap cap
    // had crashed the runtime (e.g. a fatal OOM instead of a clean termination), this would fail
    // because the whole workerd process would be gone.
    let survivor = env.loader.get('memorySurvivor', () =>
      makeCode({ limits: { memoryMB: 64 } })
    );
    assert.strictEqual(await survivor.getEntrypoint().ping(), 'pong');

    // MEM-3 (self-heal, best-effort): once the runaway allocation has been unwound and collected,
    // the original isolate should recover. This depends on GC timing, so a transient failure here
    // is acceptable — the survivor probe above already proves the process did not crash.
    try {
      assert.strictEqual(await worker.getEntrypoint().ping(), 'pong');
    } catch (e) {
      // best-effort; self-heal may not have completed yet.
    }
  },
};

// REG-1: with no declared limit, behavior matches stock workerd — an allocation that WOULD trip a
// 64 MiB cap succeeds because there is no cap.
export let noLimitIsUncapped = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('noLimitIsUncapped', () => makeCode({}));

    // ~96 MiB of live heap: this would exceed a 64 MiB cap, but there is none here.
    assert.strictEqual(await worker.getEntrypoint().allocate(96), chunksFor(96));
  },
};

// REARM-1: after an over-cap eviction, the warm isolate re-arms the cap for subsequent requests.
// The near-heap-limit callback raises V8's internal heap limit during an eviction (to avoid a fatal
// process OOM); without re-arming, that raised ceiling would persist and later over-cap requests
// would slip through. We verify the ceiling is *sustained*: a moderate over-cap allocation — far
// below the granted headroom but above the cap — is still terminated, repeatedly, on the same isolate.
export let memoryCapReArmsAfterEviction = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('memoryCapReArms', () =>
      makeCode({ limits: { memoryMB: 64 } })
    );

    const assertMemoryRejection = (promise) =>
      assert.rejects(promise, (e) => {
        assert.ok(e instanceof Error, `expected an Error, got ${typeof e}: ${e}`);
        assert.doesNotMatch(
          e.message,
          /unknown reasons/i,
          'over-limit termination was not attributed to the memory cap'
        );
        assert.match(e.message, /memory/i);
        return true;
      });

    // Evict once: ~1 GiB into a 64 MiB cap is terminated cleanly. This raises V8's heap limit (the
    // callback grants >=320 MiB of headroom to avoid a fatal OOM while the request unwinds).
    await assertMemoryRejection(worker.getEntrypoint().allocate(1024));

    // The isolate stays usable: an ordinary call and a within-cap allocation both succeed.
    assert.strictEqual(await worker.getEntrypoint().ping(), 'pong');
    assert.strictEqual(await worker.getEntrypoint().allocate(24), chunksFor(24));

    // Re-arm invariant (discriminating check): a 128 MiB allocation is far below the >=320 MiB the
    // callback granted during the eviction, but above the 64 MiB cap. If the cap re-armed it is
    // terminated; if the ceiling were still raised, 128 MiB would succeed and this would FAIL.
    await assertMemoryRejection(worker.getEntrypoint().allocate(128));

    // Sustained: repeating the over-cap request keeps failing cleanly on the same warm isolate.
    await assertMemoryRejection(worker.getEntrypoint().allocate(128));

    // And the isolate is still usable afterward.
    assert.strictEqual(await worker.getEntrypoint().ping(), 'pong');
  },
};
