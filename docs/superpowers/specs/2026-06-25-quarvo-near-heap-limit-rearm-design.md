# quarvo near-heap-limit re-arm — design

**Date:** 2026-06-25
**Status:** approved, ready to plan

## Problem

`QuarvoIsolateLimitEnforcer` ([server.c++:3062](../../../src/workerd/server/server.c%2B%2B#L3062))
enforces a dynamically-loaded Worker's `WorkerCode.limits.memoryMB` by capping the V8
old-generation heap and registering a near-heap-limit callback. When a request approaches the cap
the callback sets `memoryExceeded`, calls `TerminateExecution()`, and returns a **raised** heap
limit (`currentLimit * 2 + 256MB`) so V8 does not fatally OOM the whole process during the brief
window before the runaway request's stack unwinds. The request is turned into a clean
`OVERLOADED: Worker exceeded memory limit.` rejection.

**The bug (reported):** once a request has been evicted by OOM, the warm isolate **does not re-arm**
the per-request cap — subsequent requests can run against the raised ceiling instead of `memoryMB`.

### Root cause (verified against vendored V8 15.0.245.5)

The callback raises the limit via `SetMaximumSizes()` but **never lowers it back itself**.
Restoration is delegated entirely to V8's `AutomaticallyRestoreInitialHeapLimit(0.5)`
([server.c++:3092](../../../src/workerd/server/server.c%2B%2B#L3092)), which only restores under
narrow conditions:

- The reset runs **only in the `MARK_COMPACTOR` (full-GC) epilogue**, gated by
  `OldGenerationSizeOfObjects() < initial_max_old_generation_size_threshold_` (= 0.5 × cap), via
  `limits()->MaybeResetMaximumSizes()`
  (`src/heap/heap.cc:1574-1581`, `src/heap/heap-controller.cc:451-455`).
- Scavenges / young-gen GCs **never** reset (`src/heap/heap.cc:1590`).
- `SetMaximumSizes()` writes only `max_old_generation_size_`; it does not touch `using_initial_limit_`
  (`src/heap/heap-controller.cc:440-449`), so the raised value persists.

Therefore the raised ceiling persists for the isolate's lifetime when, after an eviction, **either**:

1. **No full GC runs** — e.g. the warm isolate goes idle (only scavenges between requests). This is
   the primary reported symptom.
2. **Live old-gen stays ≥ 50% of the cap** — every full GC skips the reset.

Plus a **compounding ratchet** (confirmed): each callback returns `currentLimit*2 + 256MB`, and V8
applies it only if it exceeds the current max (`src/heap/heap.cc:4140`). On a second incident
`currentLimit` is the *already-raised* value, so back-to-back runaway requests multiply the ceiling
before any reset.

### Verified threading constraint

V8 heap APIs must be called on the isolate thread with the isolate lock held.
`completedRequest()` is **lock-free** — it runs in the `IncomingRequest` destructor
([io-context.c++:329](../../../src/workerd/io/io-context.c%2B%2B#L329)) after the `Worker::Lock` is
released — so it cannot safely touch V8. The clean, lock-held, per-request, isolate-thread injection
point is `IoContext::runImpl`'s locked scope, alongside the existing
`limitEnforcer->enterJs(...)` call at
[io-context.c++:1326](../../../src/workerd/io/io-context.c%2B%2B#L1326).

## Goals / non-goals

**Goal:** after any over-cap eviction, a warm isolate re-arms `memoryMB` enforcement for subsequent
requests — no lingering raised ceiling, no multiplicative ratchet (a *sustained per-request
ceiling*).

**Non-goals (explicitly deferred):**
- **External / ArrayBuffer memory accounting.** Verified that ArrayBuffer backing stores are tracked
  as external memory and never trip the old-gen near-heap-limit callback (`enforce_global_heap_limit`
  defaults `false`), so `memoryMB` does not bound them. This stays the documented "Planned hardening"
  gap (`quarvo/FEATURES.md`); not addressed here.
- **Closing the residual fatal-OOM window** for a single allocation larger than the granted headroom.
  Unchanged.
- **Forcing a full GC** to guarantee an exact-to-cap restore. Decided against (see "Decisions").

## Decisions (locked with the requester)

1. **Scope:** re-arm only; external/ArrayBuffer accounting deferred.
2. **Re-arm mechanism:** restore the limit **without** forcing a GC. We proactively lower the limit
   at request entry (which does not need a GC); the rare exact-to-cap completion is left to V8's own
   next full GC. No new V8 patch.

## Design

Three small, cohesive pieces.

### (a) Persistent "incident" flag

Add `mutable std::atomic<bool> rearmPending{false}` to `QuarvoIsolateLimitEnforcer`, set in
`nearHeapLimitCallback` alongside the existing `memoryExceeded`.

It is **separate** from `memoryExceeded` on purpose: `memoryExceeded` must keep being cleared in
`completedRequest()` (so the io-context GC-reject path does not misclassify later unrelated dropped
promises on a self-healed isolate — see the existing comment at
[server.c++:3100-3105](../../../src/workerd/server/server.c%2B%2B#L3100)). The re-arm, however,
happens at the *start of the next request*, after `memoryExceeded` is already cleared, so it needs
its own flag that survives the inter-request gap.

### (b) Lock-held re-arm hook

Add to the `IsolateLimitEnforcer` interface ([limit-enforcer.h](../../../src/workerd/io/limit-enforcer.h))
a **non-pure** virtual with a default no-op body:

```cpp
// Called at each JS entry, with the isolate lock held, on the isolate thread.
// Default: no-op. QuarvoIsolateLimitEnforcer overrides it to restore the per-isolate memory cap
// after an over-cap eviction raised V8's heap limit. Must be idempotent and cheap (it is gated by
// a flag, so the no-incident path is a single relaxed atomic load).
virtual void reArmIfNeeded(jsg::Lock& lock) const {}
```

Non-pure so existing implementers (`NullIsolateLimitEnforcer`, `test-fixture.c++`, and Cloudflare's
internal enforcer) need no changes. The exact lock parameter type follows the interface convention
used by the other `enter*Js` methods (`jsg::Lock&`); the call site passes the lock already held in
the `runImpl` scope.

Call it from `IoContext::runImpl`'s locked scope, immediately after the existing
`limitEnforcer->enterJs(workerLock, *this)` at
[io-context.c++:1326](../../../src/workerd/io/io-context.c%2B%2B#L1326):

```cpp
worker->getIsolate().getLimitEnforcer().reArmIfNeeded(workerLock);
```

(`getLimitEnforcer()` is the same accessor already used a few lines later at
[io-context.c++:1437](../../../src/workerd/io/io-context.c%2B%2B#L1437).)

`QuarvoIsolateLimitEnforcer` overrides it:

```cpp
void reArmIfNeeded(jsg::Lock&) const override {
  // Hot path: a single relaxed load. No incident pending -> return immediately.
  if (!rearmPending.load(std::memory_order_relaxed)) return;
  rearmPending.store(false, std::memory_order_relaxed);
  if (isolate == nullptr) return;
  size_t capBytes = static_cast<size_t>(KJ_ASSERT_NONNULL(memoryMb)) << 20;
  // Lower the (possibly raised/ratcheted) old-gen limit back toward the cap and re-register the
  // callback. RemoveNearHeapLimitCallback -> RestoreHeapLimit sets the limit to
  // max(capBytes, SizeOfObjects()+25%); no GC is forced. The two calls are back-to-back under the
  // lock with no allocation between them, so no GC can run in the unregistered window.
  isolate->RemoveNearHeapLimitCallback(&nearHeapLimitCallback, capBytes);
  isolate->AddNearHeapLimitCallback(&nearHeapLimitCallback, this);
}
```

`Remove…(cb, heap_limit)` is the documented V8 primitive for *lowering* the limit (there is no public
direct setter); it removes the callback and calls `RestoreHeapLimit(heap_limit)`
(`src/heap/heap.cc:2104-2110`, `4094-4106`). We immediately re-add the callback.
`AutomaticallyRestoreInitialHeapLimit(0.5)` was set once in `customizeIsolate()` and persists across
remove/re-add, so V8's own reset path remains active too.

### (c) Kill the ratchet in the callback

Change the callback return from multiplicative to **additive** headroom. V8 passes `initialLimit`
(= our cap) as the third arg:

```cpp
// was: return currentLimit * 2 + (static_cast<size_t>(256u) << 20);
return currentLimit + kj::max(initialLimit, static_cast<size_t>(256u) << 20);
```

First-fire headroom is essentially unchanged (generous); repeated fires before a re-arm now grow the
limit **linearly** instead of **exponentially**.

## Behavior characterization

- **Idle case (primary symptom):** the next request's entry restores the limit immediately — no full
  GC needed, because `RemoveNearHeapLimitCallback` lowers the limit synchronously. ✅
- **Uncollected-garbage case:** if the incident's garbage is not yet collected at entry,
  `SizeOfObjects()` is inflated, so the limit lands tighter-but-not-exactly-at-cap (`live+25%`). That
  tighter limit makes V8 eager to GC, so the **first significant allocation** of the next request
  triggers a full GC that collects the garbage and V8's `MaybeResetMaximumSizes` snaps the limit back
  to the cap. Residual weak window ≈ one GC into the next request; the ratchet is already dead.
- **Repeated over-cap requests:** each fails cleanly and re-arms before the next — sustained ceiling.

## Threading & cost

- **No new lock.** `reArmIfNeeded` runs inside the isolate lock the request already holds (received
  as a parameter, a proof-of-lock token); it acquires nothing. `rearmPending` is a lock-free relaxed
  atomic, and in practice both accessors (the callback during GC, and `reArmIfNeeded` at entry) run
  on the same isolate thread.
- **Hot path:** one virtual call (same kind already made for `enterJs`/`completedRequest`) + one
  relaxed atomic load that returns `false`. No GC, no lock acquisition, no syscall, no allocation.
- **Rare path** (request following an actual eviction): `RemoveNearHeapLimitCallback` (walks a
  1-element vector, recomputes the limit via a couple of relaxed atomic stores) +
  `AddNearHeapLimitCallback` (one vector push). O(1), microseconds, no GC.

## Testing

Add a re-arm regression case to
[worker-loader-memory-test.js](../../../src/workerd/api/tests/worker-loader-memory-test.js) (the
existing MEM-1/2/3 / REG-1 cases never re-test the cap on the same isolate).

**REARM-1 — sustained per-request ceiling on one warm isolate:**
1. On a single warm worker stub, trip the cap → assert a clean `OVERLOADED` / `/memory/i` error
   (and not `/unknown reasons/i`), matching MEM-1's assertions.
2. On the **same stub**, allocate within the cap → assert it still succeeds (isolate is reusable).
3. On the **same stub**, trip the cap **again** → assert it **also** fails cleanly (proves the cap
   re-armed rather than having been raised).
4. Repeat the trip a few times to assert the ceiling is sustained, not just restored once.

Each `stub` call is a separate request on the same isolate, so `completedRequest()` and the next
entry's `reArmIfNeeded()` run between iterations — exercising the re-arm path. The "without forced
GC" choice still yields deterministic failures because each over-cap allocation drives its own GC,
completing any pending restore before it can exceed the cap.

## Docs to update

- The `QuarvoIsolateLimitEnforcer` header comment
  ([server.c++:3026-3061](../../../src/workerd/server/server.c%2B%2B#L3026)): update the enforcement-
  strategy step about restoration to describe the explicit re-arm, and note the additive (non-
  ratcheting) headroom.
- `quarvo/FEATURES.md`: reflect that the cap re-arms after an over-cap event on a warm isolate;
  leave the external/ArrayBuffer accounting row as planned hardening.

## Validation

Per the project workflow, build + run `worker-loader-memory-test` in CI on the **warm Bazel cache**
— not locally, not from a cold cache.

## File-level change list

- `src/workerd/io/limit-enforcer.h` — add `virtual void reArmIfNeeded(jsg::Lock&) const {}`.
- `src/workerd/io/io-context.c++` — call `reArmIfNeeded(workerLock)` in the `runImpl` locked scope
  (~line 1326).
- `src/workerd/server/server.c++` — add `rearmPending` field; set it in `nearHeapLimitCallback`;
  override `reArmIfNeeded`; change callback return to additive headroom; refresh the header comment.
- `src/workerd/api/tests/worker-loader-memory-test.js` — add REARM-1.
- `quarvo/FEATURES.md` — doc refresh.

## Risks & residual gaps

- **Brief weak window** in the uncollected-garbage case (one GC into the next request). Accepted per
  the "without forced GC" decision; bounded and self-correcting, and the ratchet is eliminated.
- **External/ArrayBuffer memory** remains unbounded by `memoryMB` (deferred).
- **Single-allocation fatal-OOM window** unchanged (documented).
- **Interface addition** is a non-pure virtual default-no-op → no blast radius on other
  `IsolateLimitEnforcer` implementers.
