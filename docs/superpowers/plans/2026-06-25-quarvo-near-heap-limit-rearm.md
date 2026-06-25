# Near-Heap-Limit Re-Arm Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After an over-cap memory eviction, a warm dynamically-loaded-worker isolate re-arms its `memoryMB` cap for subsequent requests — no lingering raised ceiling, no multiplicative ratchet.

**Architecture:** Add a lock-held `IsolateLimitEnforcer::reArmIfNeeded(jsg::Lock&)` hook (default no-op), called at each JS entry in `IoContext::runImpl`. `QuarvoIsolateLimitEnforcer` sets a persistent `rearmPending` flag in its near-heap-limit callback and, on the next entry, lowers V8's old-generation limit back toward the cap (via `RemoveNearHeapLimitCallback` + re-add, no forced GC). The callback's headroom is made additive (not multiplicative) to kill the ratchet.

**Tech Stack:** C++ (workerd), V8 15.0.245.5 embedder API, Bazel, workerd `wd_test` (JS) test harness.

**Spec:** [docs/superpowers/specs/2026-06-25-quarvo-near-heap-limit-rearm-design.md](../specs/2026-06-25-quarvo-near-heap-limit-rearm-design.md)

## Build/validation note (read first)

Per project policy ([validate via warm cache] memory; `quarvo/MAINTENANCE.md`), **do not run cold local Bazel builds** (~3h). The authoritative build+test runs in CI on the warm Bazel cache. Therefore:
- Code/test steps below are written TDD-first (test before implementation) for ordering and intent.
- The single authoritative "run the test" happens in **Task 6** via the warm-cache CI run of `//src/workerd/api/tests:worker-loader-memory-test`. Per-task "expected" notes describe what that run must show.
- If a fast local incremental build against a warm cache is available, the same `bazel test` command applies; otherwise rely on CI.

## File structure

- `src/workerd/io/limit-enforcer.h` — **modify**: add the `reArmIfNeeded` virtual (default no-op) to `IsolateLimitEnforcer`.
- `src/workerd/io/io-context.c++` — **modify**: call `reArmIfNeeded` in the `runImpl` locked scope.
- `src/workerd/server/server.c++` — **modify**: `QuarvoIsolateLimitEnforcer` — `rearmPending` field, set it in the callback, override `reArmIfNeeded`, additive headroom, refreshed comments.
- `src/workerd/api/tests/worker-loader-memory-test.js` — **modify**: add the `REARM-1` test case.
- `quarvo/FEATURES.md` — **modify**: doc refresh (cap re-arms after breach).

---

### Task 1: Add the failing re-arm test (REARM-1)

**Files:**
- Modify: `src/workerd/api/tests/worker-loader-memory-test.js`

The existing MEM-1/2/3 and REG-1 cases never re-test the cap on the same isolate, so they cannot catch a failure to re-arm. REARM-1 adds the discriminating check: after an eviction, a *moderate* over-cap allocation (128 MiB — far below the ≥320 MiB headroom the callback grants during an eviction, but above the 64 MiB cap) must STILL be terminated. If the cap had not re-armed, the raised ceiling would let 128 MiB through and the assertion would fail.

- [ ] **Step 1: Append the REARM-1 case**

Add at the end of `src/workerd/api/tests/worker-loader-memory-test.js` (after `noLimitIsUncapped`, before EOF). Also extend the header invariants comment block.

In the header comment (after the `REG-1:` bullet, around line 20), add:

```js
//   REARM-1: after an over-cap eviction, the same warm isolate re-arms the cap — a *moderate*
//          over-cap allocation (128 MiB, well below the headroom granted during the eviction but
//          above the cap) is ALSO terminated, and the ceiling stays sustained across repeats.
```

Append the test case:

```js
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
```

- [ ] **Step 2: Note expected pre-implementation behavior**

This test depends on Tasks 2–4. Against the current code it would FAIL at the first `allocate(128)` rejection: the ceiling stays raised after the first eviction, so 128 MiB succeeds and `assert.rejects` throws "Missing expected rejection". (Not run locally — see the build/validation note; this is the failing state Task 6 confirms is fixed.)

- [ ] **Step 3: Commit**

```bash
git add src/workerd/api/tests/worker-loader-memory-test.js
git commit -m "quarvo: add REARM-1 test for memory-cap re-arm after eviction"
```

---

### Task 2: Add the `reArmIfNeeded` hook to the IsolateLimitEnforcer interface

**Files:**
- Modify: `src/workerd/io/limit-enforcer.h`

Non-pure virtual with a default no-op body so existing implementers (`NullIsolateLimitEnforcer`, the test fixture, Cloudflare-internal) need no changes. `jsg::Lock` is already forward-declared (lines 25–27); the empty body does not use the parameter, so no extra include is required. Parameter left unnamed to avoid `-Wunused-parameter`.

- [ ] **Step 1: Add the virtual after `hasExcessivelyExceededHeapLimit()`**

In `src/workerd/io/limit-enforcer.h`, immediately after the line `virtual bool hasExcessivelyExceededHeapLimit() const = 0;` (currently line 112), insert:

```cpp
  // Called at each JS entry, with the isolate lock held, on the isolate thread. Gives the enforcer a
  // chance to re-establish per-isolate limits that a prior event may have relaxed — e.g. restoring a
  // memory cap after V8's near-heap-limit callback raised the heap limit during an over-cap event.
  // Default: no-op. Implementations must be cheap (it runs on the per-request hot path) and idempotent.
  virtual void reArmIfNeeded(jsg::Lock&) const {}
```

- [ ] **Step 2: Verify it compiles (CI / warm-cache build)**

Expected: header compiles; no behavior change yet (no override exists). Confirmed by the Task 6 build.

- [ ] **Step 3: Commit**

```bash
git add src/workerd/io/limit-enforcer.h
git commit -m "quarvo: add IsolateLimitEnforcer::reArmIfNeeded hook (default no-op)"
```

---

### Task 3: Call `reArmIfNeeded` at request entry in runImpl

**Files:**
- Modify: `src/workerd/io/io-context.c++` (in `IoContext::runImpl`, the `runInContextScope` lambda, after the existing `enterJs` call ~line 1326)

This is the verified lock-held, isolate-thread, per-request entry point. `workerLock` is a `Worker::Lock&` which implicitly converts to `jsg::Lock&` (the same conversion used a few lines down: `jsg::Lock& js = workerLock;`). `worker->getIsolate().getLimitEnforcer()` is the exact accessor already used later in the same lambda (line ~1437).

- [ ] **Step 1: Insert the call after `enterJs`**

Find:

```cpp
    auto limiterScope = limitEnforcer->enterJs(workerLock, *this);

    bool gotTermination = false;
```

Replace with:

```cpp
    auto limiterScope = limitEnforcer->enterJs(workerLock, *this);

    // quarvo: re-arm the per-isolate memory cap if a prior request on this warm isolate was evicted
    // for exceeding it. QuarvoIsolateLimitEnforcer's near-heap-limit callback raises V8's heap limit
    // during an over-cap event (to avoid a fatal process OOM while the runaway request unwinds) but
    // does not lower it back, so without this the cap can stay raised for the isolate's lifetime.
    // Restoring here — lock held, before this request runs — keeps the per-request ceiling sustained.
    // No-op (a single relaxed atomic load) for isolates without a memory cap or with no pending event.
    worker->getIsolate().getLimitEnforcer().reArmIfNeeded(workerLock);

    bool gotTermination = false;
```

- [ ] **Step 2: Verify it compiles (CI / warm-cache build)**

Expected: compiles; still no behavior change (the override lands in Task 4). The default no-op is invoked per request.

- [ ] **Step 3: Commit**

```bash
git add src/workerd/io/io-context.c++
git commit -m "quarvo: call reArmIfNeeded at JS entry in IoContext::runImpl"
```

---

### Task 4: Implement re-arm in QuarvoIsolateLimitEnforcer + kill the ratchet

**Files:**
- Modify: `src/workerd/server/server.c++` (class `QuarvoIsolateLimitEnforcer`, ~lines 3018–3133)

Four sub-edits: add `rearmPending` field; set it in the callback; override `reArmIfNeeded`; make headroom additive (and refresh comments).

- [ ] **Step 1: Add the `rearmPending` field**

Find:

```cpp
  mutable std::atomic<bool> memoryExceeded{false};
```

Replace with:

```cpp
  mutable std::atomic<bool> memoryExceeded{false};

  // Set alongside `memoryExceeded` when the near-heap-limit callback fires; cleared by reArmIfNeeded
  // at the next JS entry once the cap has been restored. Unlike `memoryExceeded` (cleared every
  // completedRequest so the io-context GC-reject path stays per-incident), this must survive the
  // inter-request gap so the *next* request entry can re-arm the cap.
  mutable std::atomic<bool> rearmPending{false};
```

- [ ] **Step 2: Set `rearmPending` in the callback**

Find:

```cpp
    auto& self = *reinterpret_cast<QuarvoIsolateLimitEnforcer*>(data);
    self.memoryExceeded.store(true, std::memory_order_relaxed);
```

Replace with:

```cpp
    auto& self = *reinterpret_cast<QuarvoIsolateLimitEnforcer*>(data);
    self.memoryExceeded.store(true, std::memory_order_relaxed);
    self.rearmPending.store(true, std::memory_order_relaxed);
```

- [ ] **Step 3: Make the headroom additive (and refresh the inline comment)**

Find:

```cpp
    // Grant generous headroom so V8 does not fatally OOM (which aborts the whole process) before
    // TerminateExecution unwinds and frees the runaway allocations. We are tearing the request
    // down, so temporary over-provisioning is fine; AutomaticallyRestoreInitialHeapLimit() restores
    // the cap afterwards. Returning `currentLimit` unchanged risks a fatal OOM in the window before
    // termination lands. (See the "residual fatal-OOM window" note above: a single allocation
    // larger than this grant can still abort.)
    return currentLimit * 2 + (static_cast<size_t>(256u) << 20);
```

Replace with:

```cpp
    // Grant generous headroom so V8 does not fatally OOM (which aborts the whole process) before
    // TerminateExecution unwinds and frees the runaway allocations. We are tearing the request down,
    // so temporary over-provisioning is fine; reArmIfNeeded() restores the cap at the next JS entry
    // (with AutomaticallyRestoreInitialHeapLimit as a backstop). Returning `currentLimit` unchanged
    // risks a fatal OOM in the window before termination lands. (See the "residual fatal-OOM window"
    // note above: a single allocation larger than this grant can still abort.)
    //
    // Additive (not multiplicative) headroom: grow by a fixed amount per fire rather than doubling,
    // so repeated fires before a re-arm grow the ceiling linearly instead of compounding (a
    // `currentLimit*2` grant ratchets multiplicatively across back-to-back over-cap events). V8
    // passes the configured cap as `initialLimit`.
    return currentLimit + kj::max(initialLimit, static_cast<size_t>(256u) << 20);
```

- [ ] **Step 4: Override `reArmIfNeeded`**

Find (the existing `completedRequest` override):

```cpp
  void completedRequest(kj::StringPtr id) const override {
    memoryExceeded.store(false, std::memory_order_relaxed);
  }
```

Insert immediately after it:

```cpp

  // Re-arm the per-isolate memory cap after an over-cap eviction. The near-heap-limit callback raises
  // V8's old-generation limit (to avoid a fatal OOM while the runaway request unwinds) but never
  // lowers it back; V8's AutomaticallyRestoreInitialHeapLimit only restores during a *full* GC with
  // live heap below 50% of the cap, which may never happen on an idle warm isolate. So when an
  // over-cap event is pending, restore the cap explicitly here, at the next JS entry, lock held.
  //
  // RemoveNearHeapLimitCallback(cb, capBytes) lowers the limit to max(capBytes, liveSize + 25%)
  // without forcing a GC; re-adding keeps the cap armed for the next event. If the evicted request's
  // garbage is not yet collected, liveSize is inflated and the limit lands slightly above the cap;
  // the next over-cap allocation's own GC reclaims it and V8 snaps the limit back to the cap. The
  // multiplicative ratchet is eliminated either way. The two V8 calls are back-to-back under the lock
  // with no allocation between them, so no GC can run while the callback is momentarily detached.
  void reArmIfNeeded(jsg::Lock&) const override {
    // Hot path: one relaxed load. No pending eviction (or no cap) -> return immediately.
    if (!rearmPending.load(std::memory_order_relaxed)) return;
    rearmPending.store(false, std::memory_order_relaxed);
    if (isolate == nullptr) return;
    KJ_IF_SOME(mb, memoryMb) {
      size_t capBytes = static_cast<size_t>(mb) << 20;
      isolate->RemoveNearHeapLimitCallback(&nearHeapLimitCallback, capBytes);
      isolate->AddNearHeapLimitCallback(
          &nearHeapLimitCallback, const_cast<QuarvoIsolateLimitEnforcer*>(this));
    }
  }
```

(`const_cast` because `reArmIfNeeded` is `const` but `AddNearHeapLimitCallback` takes `void* data`; this mirrors the non-const `customizeIsolate`, which passes `this`. The callback only touches `mutable` atomics, so the cast is sound.)

- [ ] **Step 5: Refresh the class-header enforcement-strategy comment**

In the block above the class (currently steps 1–4 of "Enforcement strategy"), replace step 2's restore sentence and steps 3–4 so they describe the explicit re-arm. Find:

```cpp
//   2. customizeIsolate() registers a near-heap-limit callback and asks V8 to automatically
//      restore the original limit once heap usage falls back below threshold.
//   3. When the callback fires, we set `memoryExceeded` and terminate the *currently executing
//      request* (TerminateExecution) rather than letting V8 fatally OOM the whole process, and
//      temporarily raise the limit so V8 does not OOM during the brief window before the stack
//      unwinds. After the runaway request is torn down and its allocations are collected,
//      AutomaticallyRestoreInitialHeapLimit() brings the cap back, leaving the (warm) isolate
//      usable and still capped for the next request.
//   4. IoContext::runImpl() (io/io-context.c++) consults hasExcessivelyExceededHeapLimit() on the
//      synchronous termination path and turns the terminated request into a clean, attributable
//      `OVERLOADED: Worker exceeded memory limit.` rejection (instead of falling through to the
//      "script terminated for unknown reasons" assertion). completedRequest() then resets the flag
//      so it reflects a *per-incident* condition rather than latching for the isolate's lifetime.
```

Replace with:

```cpp
//   2. customizeIsolate() registers a near-heap-limit callback. AutomaticallyRestoreInitialHeapLimit
//      is also set as a backstop, but the primary restore is the explicit re-arm in step 4 (V8's
//      automatic restore only fires during a full GC with live heap < 50% of the cap, so it can
//      never run on an idle warm isolate).
//   3. When the callback fires, we set `memoryExceeded` + `rearmPending` and terminate the
//      *currently executing request* (TerminateExecution) rather than letting V8 fatally OOM the
//      whole process, and temporarily raise the limit by an additive, non-compounding amount so V8
//      does not OOM during the brief window before the stack unwinds.
//   4. reArmIfNeeded(), called at the next JS entry while the isolate lock is held, restores the cap
//      explicitly (RemoveNearHeapLimitCallback + re-add) when `rearmPending` is set, so the
//      per-request ceiling stays *sustained* across requests on a warm isolate rather than drifting
//      up after an eviction.
//   5. IoContext::runImpl() (io/io-context.c++) consults hasExcessivelyExceededHeapLimit() on the
//      synchronous termination path and turns the terminated request into a clean, attributable
//      `OVERLOADED: Worker exceeded memory limit.` rejection (instead of falling through to the
//      "script terminated for unknown reasons" assertion). completedRequest() then resets
//      `memoryExceeded` so it reflects a *per-incident* condition rather than latching for the
//      isolate's lifetime.
```

- [ ] **Step 6: Verify it compiles (CI / warm-cache build)**

Expected: compiles; `QuarvoIsolateLimitEnforcer` now overrides `reArmIfNeeded`. Behavior verified in Task 6.

- [ ] **Step 7: Commit**

```bash
git add src/workerd/server/server.c++
git commit -m "quarvo: re-arm memory cap after over-cap eviction + additive headroom"
```

---

### Task 5: Update FEATURES.md

**Files:**
- Modify: `quarvo/FEATURES.md`

- [ ] **Step 1: Update the capability-matrix row**

Find:

```markdown
| `memoryMB` per-isolate cap | accepted, ignored | **enforced** (self-heal on breach) | ✅ Shipped |
```

Replace with:

```markdown
| `memoryMB` per-isolate cap | accepted, ignored | **enforced** (cap re-arms after breach) | ✅ Shipped |
```

- [ ] **Step 2: Update the "Semantics on violation" paragraph**

Find:

```markdown
  `"Worker has exceeded memory limit."` error and **the workerd process survives**. The warm isolate
  self-heals (V8 restores the cap after the request's allocations are collected) and remains capped
  for subsequent requests. This is a **soft per-isolate ceiling**: all isolates share one
```

Replace with:

```markdown
  `"Worker has exceeded memory limit."` error and **the workerd process survives**. The warm isolate
  re-arms the cap at the next request entry — the enforcer restores V8's heap limit explicitly
  (rather than waiting for a full GC), so the per-request ceiling is **sustained** for subsequent
  requests instead of drifting up after a breach. This is a **soft per-isolate ceiling**: all
  isolates share one
```

- [ ] **Step 3: Commit**

```bash
git add quarvo/FEATURES.md
git commit -m "quarvo: docs — memoryMB cap re-arms after breach"
```

---

### Task 6: Validate via warm-cache CI

**Files:** none (validation only)

- [ ] **Step 1: Push the branch and trigger the warm-cache build+test**

```bash
git push -u origin quarvo-near-heap-limit-rearm
```

The CI build runs on the warm Bazel cache (per `quarvo/MAINTENANCE.md`). The relevant test target is:

```
bazel test //src/workerd/api/tests:worker-loader-memory-test --test_output=errors
```

- [ ] **Step 2: Confirm expected output**

Expected: the `worker-loader-memory-test` target PASSES with all cases green — `memoryWithinLimit` (MEM-2), `memoryExceedsLimit` (MEM-1/3), `noLimitIsUncapped` (REG-1), and the new `memoryCapReArmsAfterEviction` (REARM-1). REARM-1 passing — specifically both `allocate(128)` rejections — confirms the cap re-armed (the discriminating check that fails on the current, un-fixed code).

- [ ] **Step 3 (if red): debug with systematic-debugging**

If REARM-1 fails or flakes, use the `superpowers:systematic-debugging` skill. Likely suspects, in order: (a) the `allocate(128)` probe slipping through because the residual limit (`liveSize+25%`) exceeded 128 MiB — inspect via a smaller cap or larger probe; (b) `reArmIfNeeded` not invoked (call-site/lock issue); (c) `rearmPending` cleared too early. Do not loosen the assertion to make it pass.

---

## Self-review

**Spec coverage:**
- Re-arm flag (`rearmPending`) → Task 4 Step 1–2. ✓
- Lock-held `reArmIfNeeded` hook + call site → Task 2, Task 3. ✓
- Restore-without-forced-GC via remove/re-add → Task 4 Step 4. ✓
- Additive (non-ratcheting) headroom → Task 4 Step 3. ✓
- REARM-1 test → Task 1. ✓
- Header comment + FEATURES.md doc updates → Task 4 Step 5, Task 5. ✓
- Warm-cache CI validation → Task 6. ✓
- External/ArrayBuffer accounting → correctly **out of scope** (deferred per spec); no task, by design. ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"/"similar to". All code shown in full. ✓

**Type/name consistency:** `reArmIfNeeded(jsg::Lock&) const` — identical signature in interface (Task 2), call site `reArmIfNeeded(workerLock)` (Task 3), and override (Task 4). `rearmPending` spelled consistently. `nearHeapLimitCallback`, `memoryMb`, `isolate`, `capBytes` match the existing class members/usage. Test export `memoryCapReArmsAfterEviction` is self-contained. ✓
