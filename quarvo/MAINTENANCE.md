# quarvo-workerd — maintenance & rebase guide

This is a thin fork of [cloudflare/workerd](https://github.com/cloudflare/workerd) that adds **real
per-isolate resource-limit enforcement** for workers loaded through the **Worker Loader binding**
(`env.LOADER.get(name, getCode)`). Stock OSS workerd accepts a `limits` object on `WorkerCode` but
ignores it; this fork makes it bite.

The goal is a **minimal, rebaseable patch series** on top of upstream release tags, so that the only
difference between our published artifact and stock workerd at the same tag is the enforcement code.

## What the fork changes

| Area | File | What |
|---|---|---|
| Limit struct | `src/workerd/io/io-channels.h` (`struct ResourceLimits`) | Adds `jsg::Optional<uint32_t> memoryMB` to the JSG struct + `clone()`. |
| Enforcer | `src/workerd/server/server.c++` | Un-`final`s `NullIsolateLimitEnforcer`; adds `QuarvoIsolateLimitEnforcer` (subclass) implementing the memory cap. |
| Plumbing | `src/workerd/server/server.c++` (`Server::WorkerDef`) | Adds `kj::Maybe<uint32_t> dynamicMemoryLimitMb`. |
| Plumbing | `src/workerd/server/server.c++` (`WorkerStubImpl::start`) | Reads `source.limits.memoryMB` → `def.dynamicMemoryLimitMb`. |
| Plumbing | `src/workerd/server/server.c++` (`Server::makeWorkerImpl`) | Constructs `QuarvoIsolateLimitEnforcer(def.dynamicMemoryLimitMb)` instead of `NullIsolateLimitEnforcer`. |
| Tests | `src/workerd/api/tests/worker-loader-memory-test.{js,wd-test}` + `BUILD.bazel` | Memory-cap enforcement test (MEM-1/2/3, REG-1). |

No upstream behavior changes when a worker declares **no** limit: `dynamicMemoryLimitMb == kj::none`
makes `QuarvoIsolateLimitEnforcer` byte-for-byte equivalent to `NullIsolateLimitEnforcer`.

## How enforcement works (the seams)

These are the points to **re-verify on every rebase** — they are where the OSS interface only ships
inert/no-op impls, which is exactly the seam we fill:

1. **`IsolateLimitEnforcer`** (`src/workerd/io/limit-enforcer.h`) — the per-isolate interface. Our
   `QuarvoIsolateLimitEnforcer` overrides `getCreateParams()`, `customizeIsolate()`,
   `hasExcessivelyExceededHeapLimit()`.
2. **`getCreateParams()`** result flows: `makeWorkerImpl` → `WorkerdApi` ctor
   (`src/workerd/server/workerd-api.c++`) → `jsg::IsolateBase` → `newIsolate()`
   (`src/workerd/jsg/setup.c++`) → `v8::Isolate::New(group, params)`. We set
   `params.constraints` here (upstream never does).
3. **`customizeIsolate(v8::Isolate*)`** is called once from `Worker::Isolate::Impl::initIsolate`
   (`src/workerd/io/worker.c++`). We register the V8 near-heap-limit callback here.
4. **Enforcer lifetime**: in `src/workerd/io/worker.h`, `limitEnforcer` is declared **before** `api`,
   so `api` (which owns the V8 isolate) is destroyed first and the enforcer **outlives** the isolate.
   This is why we store a raw `v8::Isolate*` and never call `RemoveNearHeapLimitCallback`.
   **If upstream reorders these members, re-audit the callback lifetime.**
5. **`hasExcessivelyExceededHeapLimit()`** is consumed in `src/workerd/io/io-context.h` (the
   promise-GC-reject path) to produce a clean `"Worker has exceeded memory limit."` error. OSS has
   **no** supervisor that discards an isolate on this flag — hence the self-heal design (below).

## Enforcement design (why self-heal, not discard)

`QuarvoIsolateLimitEnforcer` uses the well-known V8 "near-heap-limit" pattern:

1. Cap the V8 old generation via `ResourceConstraints` (`getCreateParams`).
2. Register `AddNearHeapLimitCallback` + `AutomaticallyRestoreInitialHeapLimit(0.5)`
   (`customizeIsolate`).
3. On the callback: set a flag, `TerminateExecution()` to kill the runaway **request** (not the
   process), and return a temporarily-raised limit so V8 does not fatally OOM before the stack
   unwinds. After the request is torn down and its allocations collected, V8 restores the cap.

This terminates the offending request, keeps the process alive, and preserves the warm isolate (no
idle eviction exists in self-hosted workerd). The shared ~4 GiB pointer-compression cage means the
cap is a per-isolate old-gen ceiling, **not** an address-space partition between isolates.

## Rebasing onto a new upstream tag

```bash
git remote add upstream https://github.com/cloudflare/workerd.git   # once
git fetch upstream --tags
# Recreate the feature branch on the new tag and replay our commits:
git checkout -b quarvo/limit-enforcement-<TAG> <TAG>
git cherry-pick <our enforcement commit(s)>      # or: git rebase --onto <TAG> <oldbase> quarvo/limit-enforcement
```

Then **re-verify the seams** (the experimental Worker Loader API churns):

```bash
# Struct still { cpuMs, subRequests } that we extend with memoryMB?
grep -n -A6 'struct ResourceLimits' src/workerd/io/io-channels.h
# Enforcer interface methods still match our overrides?
grep -n 'getCreateParams\|customizeIsolate\|hasExcessivelyExceededHeapLimit' src/workerd/io/limit-enforcer.h
# makeWorkerImpl still the single isolate factory; still constructs the enforcer?
grep -n 'makeWorkerImpl\|IsolateLimitEnforcer>' src/workerd/server/server.c++
# Enforcer still declared before api (callback-lifetime invariant)?
grep -n 'kj::Own<IsolateLimitEnforcer> limitEnforcer\|kj::Own<Api> api' src/workerd/io/worker.h
# customizeIsolate still called from initIsolate?
grep -n 'customizeIsolate' src/workerd/io/worker.c++
```

If any of these moved, update the patch accordingly before building. Also re-confirm the V8 version
in `build/deps/v8.MODULE.bazel`; the near-heap-limit / `ResourceConstraints` APIs we use are stable,
but a major V8 bump warrants a quick re-check of those signatures.

## Build & test

Build (heavy — multi-GB, needs clang + Bazel; do this in CI or on a large machine, **not** a laptop):

```bash
# Mirrors Dockerfile.release; release config for a stripped, optimized binary.
bazel build --config=release_linux //src/workerd/server:workerd
```

Run the enforcement + regression tests:

```bash
bazel test //src/workerd/api/tests:worker-loader-memory-test \
           //src/workerd/api/tests:worker-loader-limits-test \
           //src/workerd/api/tests:worker-loader-test
```

See `quarvo/INTEGRATION.md` for how the quarvo runtime consumes the published artifact.
