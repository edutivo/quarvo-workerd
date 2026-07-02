# quarvo: cgroup-pressure-driven unified GC (`QUARVO_GC_PRESSURE`) — design

- **Date:** 2026-07-02
- **Status:** approved for implementation
- **Owner:** quarvo fork (quarvo-workerd)
- **Upstream context:** [cloudflare/workerd#6824](https://github.com/cloudflare/workerd/issues/6824) (open, unacknowledged, no fix in flight as of 2026-07-02)

## Problem

workerd's process RSS ratchets up per request and never comes back, plateauing at a
fixed ~610 MiB (independent of the cgroup limit) or OOM-killing tightly-limited pods.
Root cause (diagnosed empirically against 1.20260623.1-quarvo.3): collectable
cppgc/Oilpan garbage — `jsg::Wrappable` objects (Request, `new URL()`, Response,
per-invocation tail-worker events) are allocated every request but never swept,
because:

- workerd runs continuous minor/scavenge GC but only escalates to a major/unified
  (cppgc-sweeping) GC when the JS heap nears its internal limit;
- workerd hardcodes `--noincremental-marking` (`src/workerd/jsg/setup.c++:157`),
  which disables V8's MemoryReducer — the automatic idle major GC — at any request
  rate (verified: `DEFINE_NEG_NEG_IMPLICATION(incremental_marking, memory_reducer)`
  in V8 `flag-definitions.h:2839`; MemoryReducer is never constructed).

Evidence: `usedSize` (JS heap) sawtooths 4–48 MB while RSS ratchets separately (the
garbage is on the cppgc heap); forcing a unified GC reclaims it (263→86 MiB, −67%);
`--max-old-space-size` does not help (caps JS old-space only). In prod the driver is
kubelet liveness/readiness probes (~720 invocations/h → ~5.7 MB/h creep) and every
invoked isolate accumulates: dispatcher, the `logtail` tail worker (~47% of the
ratchet), and quants. `globalThis.gc()` from JS only reaches the calling isolate, so
the fix must live in the C++ embedder.

Upstream will not fix this for us: #6824 has zero maintainer response, and the
maintainers' historical position (kentonv on #49) is that resource management is the
embedder's job.

## Goal

An embedder-driven mechanism that, under real memory pressure, runs a full unified
(V8 + cppgc) GC on the runner's isolates so garbage is swept and freed pages are
returned to the OS — across **all** isolates (static and dynamic), with **zero
request-path cost**, **opt-in**, and **byte-identical behavior when off**.

## Non-goals

- The warm-isolate working-set floor (~13 MB/isolate of live, never-evicted state).
  That needs an idle-isolate-eviction feature and is explicitly out of scope.
- cgroup v1 support, or pressure detection outside cgroups (`/proc` fallbacks).
- Per-isolate pressure thresholds (the trigger is process/cgroup-level).
- Auto-detecting Kubernetes *requests* (not visible in-container; see Configuration).
- Any change to the `WorkerCode.limits.memoryMB` enforcement semantics.

## Verified facts the design rests on

All verified against this repo and the vendored V8 15.0.245.5
(`bazel-quarvo-workerd/external/+http_archive+v8`):

1. **`MemoryPressureNotification(kCritical)` without the isolate lock does not
   collect idle isolates.** From a foreign thread without the lock, V8 only sets a
   stack-guard interrupt (serviced while JS executes) and posts a task to the
   isolate's foreground task runner (V8 `heap.cc:4063-4082`). workerd pumps that
   queue in exactly one place — inside request processing
   (`src/workerd/io/io-context.c++:1389`). For an idle isolate the GC therefore
   runs at the front of the *next request* (a live-request pause) or never.
2. **With the lock held, the same call is a synchronous full unified GC on the
   calling thread** (V8 `api.cc:10807`: a foreign thread holding `v8::Locker`
   counts as the isolate thread → `CheckMemoryPressure()` inline). kCritical runs
   `CollectAllGarbage(kReduceMemoryFootprint, …)` (plus a second pass if ≥8 MB and
   ≥10% of committed memory remains reclaimable), clears compilation caches, and
   sweeps cppgc with `FreeMemoryHandling::kReleaseMemory` — free pages returned to
   the OS. workerd's CppHeap uses **atomic sweeping**
   (`src/workerd/jsg/setup.c++:348-355`), so the sweep completes within the call.
3. **Foreign-thread GC under the full lock stack has an in-tree production
   precedent:** the inspector thread takes a synchronous lock
   (`Impl::Lock` + `TakeSynchronously` inside `jsg::runInV8Stack`) and runs
   `TakeHeapSnapshot` — which forces a full GC — from a non-request thread
   (`src/workerd/io/worker.c++:3354-3419`). GC prologue/epilogue callbacks assert
   lock *ownership*, not thread identity (`worker.c++:1203-1222`).
4. **Idle detection exists:** `Worker::Isolate::getCurrentLoad()`
   (`worker.c++:4417`) is an atomic count of request-path threads holding or
   awaiting the isolate's async lock; 0 ⇒ no request in flight or queued. (It does
   not count inspector `TakeSynchronously` locks; acceptable.)
5. **Safe cross-thread isolate references exist:**
   `Worker::Isolate::WeakIsolateRef` (`AtomicWeakRef`, invalidated in `~Isolate`,
   `tryAddStrongRef()` designed for cross-thread use — already used by the GC-reject
   path in `io-context.h:1550-1567` and the inspector registrar,
   `server.c++:2524-2543`).
6. **The enforcer cannot deregister itself safely:** `Worker::Isolate` destroys
   `api` (and with it the `v8::Isolate`) *before* `limitEnforcer`
   (`worker.h:535-537`), so a registry keyed on enforcer lifetime holds a dangling
   `v8::Isolate*` during teardown. Weak refs eliminate that class of bug.

## Design overview

```
                 ┌──────────────────────────────────────────────┐
 env vars ──────▶│ QuarvoGcPressureReclaimer                    │
 (once, startup) │  · config (thresholds, min interval)         │
                 │  · mutex-guarded Vector<Own<WeakIsolateRef>> │
                 │  · kj::Thread (only when QUARVO_GC_PRESSURE=on)
                 └───────┬──────────────────────────────────────┘
                         │ every 1s tick:
                         │  read /sys/fs/cgroup/memory.current, memory.max
                         │  over threshold && min-interval elapsed?
                         ▼
              for each registered isolate (lazily pruning dead refs):
                tryAddStrongRef() ── dead? prune, continue
                getCurrentLoad() > 0? ── busy? skip (next round)
                isolate->memoryPressureReclaim()
                   └─ runInV8Stack → Impl::Lock(TakeSynchronously)
                      → v8Isolate->MemoryPressureNotification(kCritical)
                re-read memory.current; below threshold? stop early
```

`Server::makeWorkerImpl` registers every new isolate's `getWeakRef()` with the
reclaimer (adjacent to the inspector registrar registration,
`server.c++:5310-5312`). This covers static workers (dispatcher, logtail) and
dynamic worker-loader isolates (quants) alike.

## Components

### 1. `QuarvoGcPressureReclaimer` — new files `src/workerd/server/quarvo-gc-pressure.{h,c++}`

Fork-owned files (plus a BUILD.bazel entry) to keep the upstream-file diff minimal.

- `static kj::Maybe<kj::Own<QuarvoGcPressureReclaimer>> tryCreateFromEnv()` —
  returns `kj::none` unless `QUARVO_GC_PRESSURE` is on; parses the other vars with
  fail-safe-to-off semantics (any parse error ⇒ `KJ_LOG(WARNING)` + off).
- `void registerIsolate(kj::Own<const Worker::Isolate::WeakIsolateRef> ref)` —
  mutex-guarded append.
- Owns the reclaim thread: started in the constructor, stopped via stop-flag +
  condvar, joined in the destructor (a GC in progress delays shutdown by at most
  one pause, tens of ms).
- cgroup reader: `/sys/fs/cgroup/memory.current` and `memory.max` (cgroup v2
  unified hierarchy — what k8s pods and docker containers expose, and the numbers
  the OOM killer acts on). `memory.max == "max"` ⇒ the PCT threshold is inert.
  Files unreadable ⇒ warn once, thread exits (feature inert).

### 2. `Worker::Isolate::memoryPressureReclaim()` — small fork delta in worker.{h,c++}

```c++
// NOTE(quarvo): synchronously take the isolate lock and run a critical
// memory-pressure GC (full unified V8+cppgc collection; frees pages to the OS).
// Called from the quarvo GC-pressure reclaimer thread; mirrors the inspector's
// TakeHeapSnapshot locking pattern (foreign thread + synchronous full lock).
void memoryPressureReclaim() const;
```

Implemented in worker.c++ (where `Impl::Lock` is visible):
`jsg::runInV8Stack` → `Impl::Lock(*this, Worker::Lock::TakeSynchronously(kj::none),
stackScope)` → `MemoryPressureNotification(v8::MemoryPressureLevel::kCritical)`.
Holding the lock makes the collection synchronous on the reclaimer thread (fact 2)
and satisfies every GC-callback invariant (fact 3).

### 3. Server integration — ~4 lines in server.c++

- `kj::Maybe<kj::Own<QuarvoGcPressureReclaimer>> quarvoGcPressureReclaimer` member,
  initialized from `tryCreateFromEnv()` at Server construction.
- In `makeWorkerImpl`, after the `Worker::Isolate` is created:
  `KJ_IF_SOME(r, quarvoGcPressureReclaimer) r->registerIsolate(isolate->getWeakRef());`

When the feature is off the member is `kj::none`: no thread, no registry, and the
only added work per isolate creation is one Maybe check — byte-identical runtime
behavior, mirroring how `QuarvoIsolateLimitEnforcer` with `memoryMb == kj::none`
behaves exactly like the stock `NullIsolateLimitEnforcer` (`server.c++:5235-5239`).

## Configuration

| Variable | Default | Meaning |
|---|---|---|
| `QUARVO_GC_PRESSURE` | `off` | Master switch (`on`/`1`/`true` enable; anything else is off). Off ⇒ structurally inert. |
| `QUARVO_GC_PRESSURE_THRESHOLD_PCT` | `60` | Trigger when `memory.current` ≥ this percent of cgroup `memory.max`. Inert when `memory.max` is unlimited. Range 1–100. |
| `QUARVO_GC_PRESSURE_THRESHOLD_MB` | unset | Absolute trigger in MiB compared against `memory.current`. When both thresholds resolve, the **lower byte value wins**. |
| `QUARVO_GC_PRESSURE_MIN_INTERVAL_MS` | `30000` | Minimum time between reclaim rounds. Floors GC frequency even if usage stays above threshold (a genuinely large working set must not thrash). |

The internal pressure-check tick is fixed at 1 s (not exposed; it is cheap — two
small file reads).

**Kubernetes guidance (goes in FEATURES.md/INTEGRATION.md):** the container only
sees the K8s *limit* (`memory.max`). The *request* is a scheduler-side concept not
exposed in-pod, so it cannot be auto-detected; feed it in via the Downward API:

```yaml
env:
  - name: QUARVO_GC_PRESSURE
    value: "on"
  - name: QUARVO_GC_PRESSURE_THRESHOLD_MB
    valueFrom:
      resourceFieldRef:
        resource: requests.memory
        divisor: 1Mi
```

This makes RSS sawtooth around the *guaranteed* memory while the limit remains the
hard ceiling. On large or unlimited pods, `_MB` is also the way to cap "unnecessary
memory usage" (e.g. `256`) since `_PCT` of a huge limit would never trip.

## Reclaim loop algorithm

Every tick (1 s), on the reclaimer thread:

1. Read `memory.current` and `memory.max`; compute effective threshold =
   min(PCT-derived bytes if available, MB-derived bytes if set). Neither
   resolvable ⇒ warn once, thread exits.
2. If `memory.current` < threshold, or `now − lastRound < MIN_INTERVAL_MS`: sleep.
3. Otherwise run a **round**:
   - Snapshot the registry under the mutex (prune entries whose
     `tryAddStrongRef()` fails).
   - For each live isolate, serialized (never stack pauses):
     - `getCurrentLoad() > 0` ⇒ skip (busy; it will be caught next round, and a
       busy isolate is executing JS where V8's own allocation-driven GCs run).
     - `isolate->memoryPressureReclaim()` (locked, synchronous, full unified GC).
     - Re-read `memory.current`; if below threshold, stop early.
   - `lastRound = monotonic now`; one `KJ_LOG(INFO)` line: usage before/after,
     isolates reclaimed/skipped/pruned. This is the prod observability for the
     sawtooth.

Exceptions anywhere in the round are caught (`kj::runCatchingExceptions`), logged,
and never crash the server; the loop continues on the next tick.

## Interaction with existing memoryMB enforcement (must not regress)

- The reclaimer never touches near-heap-limit callbacks, `memoryExceeded`,
  `rearmPending`, or `completedRequest` — commits #6/#7 behavior is unchanged.
- Beneficial interaction: `AutomaticallyRestoreInitialHeapLimit(0.5)` restores a
  raised heap limit only during a full GC with live heap < 50% of cap — which
  previously could never happen on an idle warm isolate. Reclaimer-driven full GCs
  give it that opportunity; `reArmIfNeeded` at next JS entry remains the guaranteed
  backstop.
- Acceptance: MEM-1/2/3, REG-1, REARM-1 in
  `src/workerd/api/tests/worker-loader-memory-test.js` all stay green, including a
  new variant with the reclaimer forced on (below).

## Testing & validation (acceptance criteria)

1. **Regression, feature off (default):** full existing quarvo test set green
   (`worker-loader-memory-test@`, `worker-loader-limits-test@`,
   `worker-loader-test@`). No behavior delta is possible by construction (no
   thread, no registry), matching the REG-1 philosophy.
2. **Non-interference, feature on under churn (new wd_test variant):** run the
   memory-limit suite with `QUARVO_GC_PRESSURE=on`,
   `QUARVO_GC_PRESSURE_THRESHOLD_MB=1` (always over threshold ⇒ a reclaim round
   every interval), `QUARVO_GC_PRESSURE_MIN_INTERVAL_MS=250`. Asserts memoryMB
   semantics survive constant background unified GCs. The `wd_test` rule's `env`
   attr (`build/wd_test.bzl`) already plumbs env vars to the workerd under test.
3. **E2E ratchet proof (new job in `.github/workflows/quarvo-image-test.yml`):**
   run the published image in `docker run --memory=256m` with a config exercising
   all three isolate classes — a dispatcher-style worker, a **tail worker**
   (logtail analog), and a **worker-loader (quant-style) dynamic isolate** — under
   a sustained healthz-style request loop, sampling the container's
   `memory.current`:
   - feature **off**: usage climbs substantially and does not return (reproduces
     the ratchet; assert final usage ≫ warm baseline);
   - feature **on** (threshold ≈40% = ~102 MiB): usage stays bounded, sawtoothing
     at/below the threshold for the whole run and never approaching the 256 MiB cap.
   Aggregate boundedness under load that invokes all three isolate classes is the
   proof that dispatcher + tail + dynamic isolates are all being swept (if any
   class weren't, its share of the ratchet would keep climbing).
4. **Build & test venue:** in CI on the warm Bazel disk cache (squash PR to
   quarvo-main ⇒ cache-warm `quarvo-release` run). Never built locally (host
   cannot build workerd; disk-tight). Note: `quarvo-release` currently only
   *builds*; running the quarvo wd_test targets in CI requires adding a
   `bazel test` step (reusing the same disk cache) to the cache-warm path or a
   small dedicated workflow — part of this work.

## Release & handback

1. Feature branch → squash PR to `quarvo-main` (`quarvo:` subject prefix), which
   warms the Bazel cache.
2. **Release-workflow improvement (part of this work):** `quarvo-release.yml`'s
   `merge` job currently prints the multi-arch INDEX digest only in the
   `imagetools inspect` log. Capture it explicitly (e.g.
   `docker buildx imagetools inspect --format '{{json .Manifest}}'` → digest) into
   the job summary and a small artifact, so digest-pinning is copy-pasteable.
3. Tag `v1.20260623.1-quarvo.4` → GHCR multi-arch publish.
4. Dispatch `quarvo-image-test` against the new tag (including the new ratchet
   job) — green gate.
5. Mirror to ACR via `quarvo/ops/ghcr-to-acr.sh` (`az acr import`, preserves the
   index digest; needs `az login` + GHCR PAT).
6. **Hand back to the consumer repo (quarvo):** image tag
   `1.20260623.1-quarvo.4`, the multi-arch **index digest** for
   `COPY --from=edutivo.azurecr.io/edutivo/quarvo-workerd@sha256:…` pinning, and
   the four env vars (with the Downward API snippet) the runner deployment should
   set.

## Documentation plan

Per `quarvo/FEATURES.md` §"Adding a new feature to this page" and the AGENTS.md
changelog rule:

- **FEATURES.md** — capability-matrix row + full section (What it is / OSS workerd
  vs quarvo / How to use (env table + k8s snippet) / Semantics / Limitations &
  gotchas / Verify-test).
- **INTEGRATION.md** — new consumption-contract section: the env vars the runner
  deployment sets, Downward API pattern, and note that the feature is process-wide
  (not per-`limits` key).
- **MAINTENANCE.md** — new seam notes: `quarvo-gc-pressure.{h,c++}`,
  `Worker::Isolate::memoryPressureReclaim()`, registration point in
  `makeWorkerImpl`, and the release-workflow digest capture.
- **README.md** — quick-glance capability row.
- **CHANGELOG.md** — `## [Unreleased]` → **Added** bullet (user-perspective,
  relative to upstream, linking the PR); on release, cut
  `## [1.20260623.1-quarvo.4]` with Upstream base + Image lines.

## Alternatives considered

- **Lock-free `MemoryPressureNotification(kCritical)` (the literal upstream-issue
  ask):** rejected — for idle isolates the GC defers into the next live request or
  never runs (fact 1). It converts "free reclaim while idle" into "a pause taxed on
  the next request" and abandons quiet isolates.
- **`LowMemoryNotification()` under the lock:** runs 2–7 back-to-back full GCs
  until root counts stabilize. Strictly more pause for marginal extra reclaim;
  kCritical's 1–2 collections already release pages given workerd's atomic cppgc
  sweep. Not needed; can be revisited if e2e shows residue.
- **Main-event-loop timer instead of a thread:** no new thread and fairness-queue
  locks, but every pause stalls the entire event loop (all isolates' I/O), and
  reclaim is delayed exactly when the loop is congested — which correlates with
  pressure. Rejected.
- **Enforcer-lifecycle registry (original task sketch):** use-after-free window on
  teardown (fact 6); replaced with weak refs.
- **`--v8-flags` passthrough / re-enabling incremental marking + MemoryReducer:**
  upstream sets `--noincremental-marking` deliberately (past bugs; 128 MB
  production heaps); re-enabling it in a fork is a large, risky behavioral delta
  and MemoryReducer's cadence is not pressure-aware. Rejected.

## Known limitations (documented, accepted)

- cgroup v2 only; elsewhere the feature warns once and stays inert.
- An isolate that is always mid-request is never reclaimed by this mechanism
  (skip-if-busy). Its own allocation-driven GCs still run.
- A request arriving for an isolate during its GC window waits for the lock (tens
  of ms; narrow window, pressure- and interval-gated). Same trade the inspector
  makes.
- `memory.current` includes page cache; in practice the workerd container's usage
  is dominated by the process, and the kernel reclaims cache before OOM. The
  threshold semantics are "as the OOM killer sees it", which is what matters.
- The synchronous lock bypasses the async-lock fairness queue (same as the
  inspector); with the idle gate this is theoretical.
- Does not address the warm-isolate working-set floor (~13 MB/isolate live) —
  separate idle-isolate-eviction feature.
