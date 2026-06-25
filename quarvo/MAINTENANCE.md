# quarvo-workerd — maintenance & rebase guide

This is a thin fork of [cloudflare/workerd](https://github.com/cloudflare/workerd) that adds **real
per-isolate resource-limit enforcement** for workers loaded through the **Worker Loader binding**
(`env.LOADER.get(name, getCode)`). Stock OSS workerd accepts a `limits` object on `WorkerCode` but
ignores it; this fork makes it bite.

> New here? Start at [README.md](README.md) for the doc map. For **what the fork exposes and how to
> use it**, see [FEATURES.md](FEATURES.md); for **consuming/pinning** the artifact, see
> [INTEGRATION.md](INTEGRATION.md). This doc is for **maintaining and rebasing** the fork.

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

## Cold-build gotchas (fork-specific fixes vs. stock upstream)

Upstream builds with Cloudflare's **Bazel remote cache**, so its CI rarely compiles anything from
scratch. A fork has no access to that cache, so a **cold** build exposes things upstream's cache
hides. The fixes that live in this fork because of that:

1. **Toolchain (`quarvo/Dockerfile`)** — `node:trixie` (Debian 13) has **no `software-properties-common`**
   and ships **LLVM 19 natively**, so we install `clang-19 lld-19 libc++-19-dev libc++abi-19-dev
   libunwind-19-dev libclang-rt-19-dev` straight from Debian (not via `apt.llvm.org/llvm.sh`). And
   workerd does **not** declare `bazel`/`bazelisk` as a pnpm dependency, so we install
   `@bazel/bazelisk` explicitly and call `bazel` directly (not `pnpm exec bazel`). `CC=/usr/bin/clang-19`.

2. **Broken TypeScript migration at v1.20260623.1 → `noCheck` stopgap** (`tools/base.tsconfig.json`).
   Upstream commit `5e2a624c5` bumped `typescript` to `6.0.2` + `@types/node` to `>=25.5.0` and set
   `tsconfig ignoreDeprecations:"6.0"`, **without** landing the matching `src/node` node-compat fix —
   so the node-compat TypeScript does not type-check against its own pinned deps (e.g. `http.AgentOptions`
   has no `noDelay` in any `@types/node` 24/25/26). Upstream's remote cache serves prebuilt `src/node`,
   so their CI never runs `tsc` fresh; a cold fork build does and fails. At the time this fork was cut,
   `upstream/main` == our base, i.e. **there was no newer tag with the fix** (it is in upstream's future).
   The stopgap is `"noCheck": true` in `tools/base.tsconfig.json`: every `ts_project` **transpiles** the
   (working) code without the broken type gate. The emitted JS is correct; only type-checking is skipped.

   **Remove `noCheck` when you rebase onto an upstream tag where the migration is complete.** Quick test
   for "is it fixed yet": at the new tag, `git show <tag>:pnpm-lock.yaml | grep -A2 '  typescript:'`
   should show a version consistent with `package.json` (i.e. `6.0.x`, not the stale `5.9.3`), and a
   cold `bazel build //src/node:node@tsproject` should pass with `noCheck` removed.

The `quarvo/Dockerfile` builds `//src/node:node@tsproject` **first** as a fast-fail: a deps/TS
regression surfaces in ~minutes instead of after the ~3 h V8 compile.

## Releasing via GitHub Actions (public repo)

The repo is public, so GitHub-hosted Actions minutes and a public GHCR package are free. The
`.github/workflows/quarvo-release.yml` workflow builds `quarvo/Dockerfile` and pushes
`ghcr.io/<owner>/quarvo-workerd:<version>`.

1. **Land the workflow on the default branch.** `workflow_dispatch` only appears, and tag triggers
   only fire, when the workflow file exists on the repo's default branch. Merge the
   `quarvo/limit-enforcement` branch (which contains the workflow) into your default branch first.

2. **Pick a runner** (edit `runs-on:` in the workflow). A cold V8 build is heavy — upstream uses a
   16-core runner. For a public repo:
   - Free first attempt: leave `ubuntu-latest`; the workflow's disk-reclaim step frees ~25-30 GB so
     it usually fits, but expect ~1 h+ and possible RAM pressure.
   - Reliable/fast: switch to a larger runner your org enables (e.g. `ubuntu-22.04-16core`) or a
     self-hosted runner (`runs-on: [self-hosted, linux, X64]`) on a ≥8-core / ≥32 GB / ≥60 GB host.

3. **Permissions.** The workflow already sets `permissions: packages: write` and logs in to GHCR
   with the automatic `GITHUB_TOKEN` — no secrets needed. (Settings → Actions → General → Workflow
   permissions should allow read/write, or rely on the per-job block.)

4. **Trigger a build** either way:
   - Tag: `git tag v1.20260623.1-quarvo.1 && git push origin v1.20260623.1-quarvo.1`
   - Manual: Actions tab → "quarvo-release" → Run workflow → enter the version.

5. **Make the package public + linked.** After the first push, open the org's Packages →
   `quarvo-workerd` → Package settings → set visibility **Public** and link it to this repo, so
   quarvo can `COPY --from=ghcr.io/...` without auth.

6. **Verify:** `docker run --rm ghcr.io/<owner>/quarvo-workerd:<version> --version`, then a smoke
   test loading a worker with `limits:{ memoryMB: 64 }` (see worker-loader-memory-test.js).

## CI build caching (fast rebuilds)

A cold vendored-V8 build is ~3 h per arch, but ~99% of the ~8,500 build actions (V8, abseil, ICU,
boringssl) are **identical across our fork's commits** — only the enforcement C++ (`io-channels.h`,
`server.c++`, `io-context.c++`) ever changes. `quarvo-release.yml` therefore persists Bazel's
**`--disk_cache`** across runs so a rebuild that only touches our patch finishes in **minutes**
(cache hits for V8; only the changed files recompile + relink). The build is deterministic enough to
cache well: Bazel keys every action by a content hash (`--disk_cache` is content-addressed), the
version string comes from `src/workerd/io/release-version.txt` (a source file, not build time), and
the build does no SCM/timestamp stamping. (Note: the workerd source does **not** itself redact
`__DATE__`/`__TIME__` — `grep -rn __DATE__ .bazelrc build/` is empty — so don't rely on that; the
caching wins come from the action-hash + source-version properties above.)

### Ref-scoping: why the workflow runs on `quarvo-main`, not just on tags

GitHub scopes the Actions cache **per ref**. A run can restore caches created in **its own ref** or
the **default branch** — nothing else. Every release is a **new `v*-quarvo.*` tag = a new ref**, so a
previous release's cache is *invisible* to the next release (we verified this the hard way: a build on
tag `…quarvo.2` saved a cache scoped to that tag's ref, and a build on a different tag missed it and
rebuilt cold). The only ref every release can read is the **default branch (`quarvo-main`)**.

So the workflow triggers on **both** tags **and** pushes to `quarvo-main`:

- **Push to `quarvo-main` = cache-warm run.** Builds and **saves** the disk cache under the
  default-branch scope. Publishes nothing (the image-push + `merge` are gated off via `IS_RELEASE`).
- **Tag push / manual dispatch = release run.** **Restores** the default-branch cache (warm), builds,
  pushes the image by digest, assembles the manifest. It does **not** save (saving under a tag ref
  would be useless and would burn the 10 GB budget).

**Operational flow: merge to `quarvo-main` (warms the cache) → tag the release (runs warm → fast).**
The first `quarvo-main` build after a V8-affecting change is cold (~3 h); subsequent ones — and the
releases that follow — are minutes. (A manual `workflow_dispatch` on `quarvo-main` both publishes
*and* refreshes the cache, since it runs on the default-branch ref.)

How a single run fits together (see the comments in `.github/workflows/quarvo-release.yml` and
`quarvo/Dockerfile`):

1. **`actions/cache/restore`** pulls the per-arch disk cache into `~/bazel-disk-cache` (from this
   ref or, for a tag release, from the `quarvo-main` scope).
2. The **`export` target** of `quarvo/Dockerfile` runs the heavy compile inside the validated
   `node:trixie` builder, seeding `--disk_cache` from that directory (passed in as the
   `bazelcache` build context) and emitting **both** the `workerd` binary **and** the updated disk
   cache to a local dir via `--output type=local`.
3. *(release runs only)* The thin **`quarvo/Dockerfile.runtime`** is built from that extracted binary
   and pushed by digest — so the expensive compile runs **exactly once** per build.
4. **`actions/cache/save`** persists the updated cache **only on `quarvo-main`**, only on a cold miss
   (exact-key entries are immutable), and only if the build succeeded (split restore/save so a broken
   build can't poison the key with a partial/empty cache).

Key design points / knobs:

- **Cache key** — `bazel-disk-${arch}-${hashFiles('.bazelversion','.bazelrc','MODULE.bazel','quarvo/Dockerfile')}`.
  **Exact key, no `restore-keys`**: a prefix match lets Bazel snowball the cache larger every run
  (upstream `_bazel.yml` documents the same choice). **Per-arch** because amd64/arm64 action hashes
  differ. `quarvo/Dockerfile` is in the hash so a toolchain/recipe change rotates the key — note the
  flip side: a **comment-only edit to the Dockerfile also rotates the key and forces one cold ~3 h
  rebuild**. That's an accepted trade for never serving a stale cache after a toolchain bump; the
  Dockerfile changes rarely.
- **10 GB GitHub cache budget is shared across the WHOLE repo (LRU), not per key.** `quarvo/Dockerfile`
  drops cache entries >100 MB (`find /bazel-disk-cache -size +100M -type f -delete`, GNU find) before
  export — keeping the many small action outputs (the bulk of hits) and dropping the few huge ones
  (they re-run; acceptable). Mirrors upstream's "Drop large Bazel cache files" step. **But the trim is
  a per-file filter, not a total-size cap, and there are TWO caches (amd64 + arm64) competing for the
  one repo budget.** If their combined trimmed size exceeds the budget, GitHub silently evicts the
  least-recently-used entry — so a later run's restore can MISS and eat a full cold build for that
  arch. **Measured (cold `-quarvo.2` build, both arches): ~1.5 GB on disk / ~293 MiB compressed per
  arch → ~0.6 GB compressed for the pair, comfortably under 10 GB with plenty of headroom — no
  eviction risk at this size.** The workflow logs `du -sh` of the cache (and `df -h /`) on every run so
  you can re-check after a V8 bump. If it ever grows past the budget, lower the trim threshold (e.g.
  `+50M`) or move to a remote cache (below). Also confirm the repo's actual cache cap — GitHub raised
  it above 10 GB on some plans in late 2025; don't assume.
- **No `cache-from/to: type=gha`.** Docker *layer* caching is the wrong level here — the whole
  compile is one `RUN` layer, busted by any source commit. The disk cache replaces it. (The few-minute
  toolchain setup — apt + `pnpm install` — does re-run each job since runners are ephemeral; that's
  negligible against even a warm relink.)
- **Peak disk.** The heavy `export` build runs in a buildkit container whose state (Bazel
  `output_base` for a full vendored-V8 build is tens of GB) lives on `/`, and the `--output type=local`
  export writes the binary + trimmed cache to `$RUNNER_TEMP` (also on `/`) **while that state is still
  resident** — so right after the export is the tightest disk moment (the workflow logs `df -h /`
  there). The "Reclaim disk space" step frees ~25–30 GB to make room, and `docker buildx prune -af`
  runs afterwards. If a build dies ~3 h in with a confusing compile/link error, suspect ENOSPC: bump to
  a larger runner, or relocate the docker data-root / export dir to the runner's larger `/mnt` volume.
- **`pnpm install` is non-frozen** and can drift `@types/node` between runs; that only re-runs the
  cheap `//src/node` tsproject action, not V8. Adding `--frozen-lockfile` (the lockfile
  `pnpm-lock.yaml` exists) would make even that a stable hit and the build reproducible — left off for
  now because a fork lockfile that's slightly out of sync would then hard-fail the build; revisit once
  the lockfile is known-consistent.

**Verify the win** (mind the ref-scoping — the cache only crosses runs via `quarvo-main`):

1. Push the branch's build-relevant changes to **`quarvo-main`** → the cache-warm run goes cold (~3 h)
   and saves the default-branch cache. (Watch with `gh run watch <id> --repo edutivo/quarvo-workerd`.)
2. Push a fresh **release tag** cut from `quarvo-main` (`git tag v1.20260623.1-quarvo.N quarvo-main &&
   git push origin <tag>`) → it should **restore** that cache and finish in **minutes**, recompiling
   only what changed since the warm build + relinking. This is the real proof.
3. Confirm correctness by pushing an `imgtest-*` tag to run `quarvo-image-test` on both arches.

(To prove it *without* touching `quarvo-main`, you can instead re-run the **same** tag's workflow
[`gh run rerun <id>`] — a re-run shares the ref, so it restores that ref's own cache and runs warm.
That validates the mechanism but not the cross-release path, which needs the `quarvo-main` cache.)

**Local `docker build` still works unchanged** (compiles cold, no disk cache):
`docker build -f quarvo/Dockerfile -t ghcr.io/edutivo/quarvo-workerd:<tag> .`

**Scale-up if the disk cache outgrows 10 GB or you want cross-runner sharing:** stand up a Bazel
**remote cache** (`bazel-remote`/`buildbarn` backed by a bucket, or a SaaS free tier) and add
`--remote_cache=<url>` (+ a secret) to the `bazel build` lines — no size cap, shared across
arches/runs/PRs. This is what upstream does (their cache is private to them). No remote cache or
extra secrets are configured today; the `--disk_cache` approach needs none.

See `quarvo/INTEGRATION.md` for how the quarvo runtime consumes the published artifact.
