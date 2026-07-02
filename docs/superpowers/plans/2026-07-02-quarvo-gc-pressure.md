# quarvo GC-pressure (QUARVO_GC_PRESSURE) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Under cgroup memory pressure, a background thread runs a full unified (V8+cppgc) GC on idle isolates so cppgc garbage is swept and pages returned to the OS — opt-in via `QUARVO_GC_PRESSURE`, byte-identical when off.

**Architecture:** A new fork-owned `QuarvoGcPressureReclaimer` (registry of `Worker::Isolate::WeakIsolateRef` + dedicated `kj::Thread`) reads the process's cgroup v2 usage each second and, past a threshold, calls a new `Worker::Isolate::memoryPressureReclaim()` which takes the synchronous full isolate lock (the inspector's pattern) and fires `MemoryPressureNotification(kCritical)`. Spec: `docs/superpowers/specs/2026-07-02-quarvo-gc-pressure-design.md`.

**Tech Stack:** C++ (kj, jsg, V8 15.0.245.5), Bazel, wd_test, GitHub Actions, docker buildx, GHCR/ACR.

---

## CRITICAL EXECUTION CONSTRAINTS (read first)

1. **This host CANNOT build workerd** (~5 GB disk; cold build is ~3 h on a 40 GB/16-core box). NEVER run `bazel build`/`bazel test` locally. All compile/test validation happens in GitHub Actions on the persisted Bazel disk cache. Steps below marked **[CI]** are validated there, not locally.
2. **Two-PR sequencing** (cache-key mechanics): `quarvo/Dockerfile` is part of the Actions cache key (`hashFiles('.bazelversion','.bazelrc','MODULE.bazel','quarvo/Dockerfile')`). Editing it forces ONE cold ~3 h rebuild. So:
   - **PR-1 (CI infra):** Dockerfile `test` stage (target list injected via build-arg so later changes DON'T touch the Dockerfile again) + workflow changes. Its post-merge run is cold (~3 h/arch) and warms the new cache key.
   - **PR-2 (feature):** all C++/tests/docs + workflow build-arg listing the new test targets. Its `pull_request` run restores the warm cache from quarvo-main scope → builds+tests in minutes, PRE-merge.
3. **quarvo conventions:** commit subjects `quarvo: <what>`; land on `quarvo-main` via **squash PRs only**; every user-facing delta gets a `quarvo/CHANGELOG.md` `[Unreleased]` bullet; do NOT put `[skip ci]` on these merges (we need the CI runs).
4. Work on branch `quarvo-gc-pressure` (already exists, holds the spec commit).

---

## Phase 1 — PR-1: CI/test infrastructure

### Task 1: Dockerfile `test` stage (build-arg-driven target list)

**Files:**
- Modify: `quarvo/Dockerfile` (append after the `export` stage block, i.e. after line 73)

- [ ] **Step 1.1: Append the test stage to `quarvo/Dockerfile`**

Append after the `export` stage (after `COPY --from=builder /bazel-disk-cache /bazel-disk-cache`), before the `runtime` stage comment block:

```dockerfile
# CI test target: run the quarvo regression test targets against the same build tree + disk cache.
# The target list is a BUILD-ARG (not baked into this file) so extending it later does NOT change
# this file's hash — which is part of the CI cache key (editing this file forces a ~3h cold build).
# worker-loader-test@ is excluded by default: it is tagged requires-network and flaky in builders.
FROM builder AS quarvo-test
ARG QUARVO_TEST_TARGETS="//src/workerd/api/tests:worker-loader-memory-test@ //src/workerd/api/tests:worker-loader-limits-test@"
RUN bazel test --disk_cache=/bazel-disk-cache --config=release_linux \
      --test_output=errors ${QUARVO_TEST_TARGETS} \
    && touch /quarvo-tests-ok
```

Note: stage is named `quarvo-test` (not `test`) to avoid colliding with buildx conventions. It is `FROM builder`, so within the same CI job the already-built layers are reused and only the (incremental) test build+run happens.

- [ ] **Step 1.2: Commit**

```bash
git add quarvo/Dockerfile
git commit -m "quarvo: add quarvo-test Dockerfile stage (bazel test on the warm disk cache)"
```

### Task 2: quarvo-release.yml — PR trigger, test step, INDEX digest capture

**Files:**
- Modify: `.github/workflows/quarvo-release.yml`

- [ ] **Step 2.1: Add `pull_request` trigger**

In the `on:` block, alongside the existing triggers (push tags / push quarvo-main / workflow_dispatch), add:

```yaml
  pull_request:
    branches: [quarvo-main]
```

PR runs restore the quarvo-main-scoped cache (GitHub allows base-branch cache reads), never save (`save` is already gated on `github.ref == 'refs/heads/quarvo-main'`), and never publish (publish is gated on tag/dispatch). Verify those three gates still hold after editing.

- [ ] **Step 2.2: Add the test step to the `build` job**

Immediately after the existing "build" step (the `docker buildx build --target export ...` step) in the build job, add:

```yaml
      - name: Run quarvo test targets (warm cache)
        if: github.event_name == 'pull_request' || github.ref == 'refs/heads/quarvo-main'
        run: |
          docker buildx build --target quarvo-test -f quarvo/Dockerfile \
            --build-context bazelcache=$HOME/bazel-disk-cache \
            ${QUARVO_TEST_TARGETS:+--build-arg QUARVO_TEST_TARGETS="$QUARVO_TEST_TARGETS"} .
```

And add at the top of the workflow (workflow-level `env:` block, next to `IMAGE`):

```yaml
  # Extra/override test targets for the quarvo-test stage; empty = Dockerfile default list.
  QUARVO_TEST_TARGETS: ""
```

(PR-2 will set this to the full list including the new targets.)

- [ ] **Step 2.2b: Add restore-keys so the Dockerfile-edit key rotation stays warm**

The cache key hashes `quarvo/Dockerfile`, so Task 1 rotates it. To avoid a ~3 h cold rebuild,
add a prefix fallback to the RESTORE step (restore only — the save step and its exact-key
condition stay unchanged):

```yaml
          restore-keys: |
            bazel-disk-${{ matrix.arch }}-
```

(The existing key line `bazel-disk-${{ matrix.arch }}-${{ hashFiles(...) }}` stays as-is; on an
exact miss the newest cache with the prefix seeds the build — V8/ICU/abseil all hit and only
genuinely new actions compile. This deliberately relaxes the repo's earlier "no restore-keys"
stance; the >100 MB trim in the Dockerfile plus GitHub's 10 GB LRU bound the snowball risk.
Document this in MAINTENANCE.md — see Task 10.3.)

- [ ] **Step 2.3: Capture the multi-arch INDEX digest in the `merge` job**

After the existing "Inspect (confirm amd64 + arm64)" step, add:

```yaml
      - name: Capture multi-arch index digest
        id: digest
        run: |
          d=$(docker buildx imagetools inspect \
                "${IMAGE}:${{ steps.meta.outputs.version }}" \
                --format '{{json .Manifest}}' | jq -r '.digest')
          test -n "$d" && [ "$d" != "null" ]
          echo "index_digest=${d}" >> "$GITHUB_OUTPUT"
          {
            echo "### quarvo-workerd ${{ steps.meta.outputs.version }}"
            echo "- tag: \`${IMAGE}:${{ steps.meta.outputs.version }}\`"
            echo "- multi-arch index digest: \`${d}\`"
            echo "- pin: \`${IMAGE}@${d}\`"
          } >> "$GITHUB_STEP_SUMMARY"
```

- [ ] **Step 2.4: Sanity-check the workflow YAML locally**

Run: `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/quarvo-release.yml'))" && echo OK`
Expected: `OK`

- [ ] **Step 2.5: CHANGELOG entry + commit**

Add under `## [Unreleased]` in `quarvo/CHANGELOG.md` (create the section if missing), category **Internal**:

```markdown
- **Internal:** CI now runs the quarvo bazel test targets on the warm disk cache for PRs to
  quarvo-main and quarvo-main pushes, and release runs capture the multi-arch **index digest**
  in the job summary/outputs for `@sha256:` pinning. (#N)
```

```bash
git add .github/workflows/quarvo-release.yml quarvo/CHANGELOG.md
git commit -m "quarvo: CI test step on warm cache + capture multi-arch index digest"
```

### Task 3: Open PR-1, merge, wait for the cold cache-warm run **[CI]**

- [ ] **Step 3.1: Push and open PR-1 with ONLY the two commits above**

PR-1 must contain ONLY Tasks 1–2 (plus optionally the spec/plan docs — docs are harmless). Create it from a dedicated branch so the feature branch can continue:

```bash
git checkout -b quarvo-ci-test-infra quarvo-gc-pressure
git push -u origin quarvo-ci-test-infra
gh pr create --base quarvo-main --title "quarvo: CI test stage on warm cache + index-digest capture" \
  --body "Infra for the GC-pressure feature (spec: docs/superpowers/specs/2026-07-02-quarvo-gc-pressure-design.md).
- quarvo-test Dockerfile stage (bazel test, target list via build-arg so future changes don't rotate the cache key)
- pull_request trigger for quarvo-main (build+test pre-merge on warm cache; no save, no publish)
- merge job captures the multi-arch INDEX digest (output + step summary)
NOTE: rotates the Bazel cache key (Dockerfile edit) — the post-merge run is one cold ~3h build that warms the new key."
```

- [ ] **Step 3.2: Squash-merge PR-1 (no `[skip ci]`), then watch the quarvo-main run**

```bash
gh pr merge --squash --subject "quarvo: CI test stage on warm cache + index-digest capture (#N)"
gh run list --workflow=quarvo-release.yml --branch=quarvo-main --limit 1
gh run watch <run-id>   # warm via restore-keys fallback; extra test-dep compilation ~10-30 min
```

Expected: run green in well under an hour (restore-keys seeds the old warm cache; only the new
test-target deps compile); test step passes (existing memory/limits wd_tests); cache saved under
the NEW key so later runs are exact-hit warm. If the test step fails on pre-existing tests, STOP
and investigate before Phase 2 (do not proceed on a red base).

---

## Phase 2 — PR-2: the feature

### Task 4: Pure config/cgroup parsing + unit tests (`quarvo-gc-pressure.{h,c++}`, TDD)

**Files:**
- Create: `src/workerd/server/quarvo-gc-pressure.h`
- Create: `src/workerd/server/quarvo-gc-pressure.c++`
- Create: `src/workerd/server/quarvo-gc-pressure-test.c++`
- Modify: `src/workerd/server/BUILD.bazel` (server target is at line ~219)

- [ ] **Step 4.1: Write the header (types + pure functions + class declaration)**

`src/workerd/server/quarvo-gc-pressure.h`:

```c++
// NOTE(quarvo): cgroup-pressure-driven unified GC. Under memory pressure, a background thread
// runs MemoryPressureNotification(kCritical) on idle isolates while holding the isolate lock,
// sweeping cppgc garbage and returning pages to the OS. Opt-in via QUARVO_GC_PRESSURE; when off
// this module is never instantiated and behavior is identical to stock workerd.
// Design: docs/superpowers/specs/2026-07-02-quarvo-gc-pressure-design.md
#pragma once

#include <workerd/io/worker.h>

#include <kj/common.h>
#include <kj/mutex.h>
#include <kj/string.h>
#include <kj/thread.h>
#include <kj/time.h>
#include <kj/vector.h>

namespace workerd::server {

struct QuarvoGcPressureConfig {
  // Percent of cgroup memory.max at which to trigger (inert if memory.max is unlimited).
  uint32_t thresholdPct = 60;
  // Absolute trigger in bytes (from QUARVO_GC_PRESSURE_THRESHOLD_MB). When both this and the
  // pct threshold resolve, the LOWER byte value wins.
  kj::Maybe<uint64_t> thresholdBytes;
  // Minimum time between reclaim rounds.
  uint64_t minIntervalMs = 30000;
};

// Pure parsing/decision helpers, split out for unit testing (quarvo-gc-pressure-test.c++).

// Parses the four env values (pass results of getenv(); kj::none = unset). Returns kj::none
// unless `enabled` is one of on/1/true/yes (case-insensitive). Invalid numeric values fall back
// to defaults with a warning (fail-safe).
kj::Maybe<QuarvoGcPressureConfig> parseQuarvoGcPressureEnv(kj::Maybe<kj::StringPtr> enabled,
    kj::Maybe<kj::StringPtr> thresholdPct,
    kj::Maybe<kj::StringPtr> thresholdMb,
    kj::Maybe<kj::StringPtr> minIntervalMs);

// Parses the content of a cgroup v2 numeric file ("123456\n" or "max\n"). "max", empty, or
// malformed => kj::none.
kj::Maybe<uint64_t> parseCgroupValue(kj::StringPtr content);

// Extracts the cgroup v2 relative path from /proc/self/cgroup content (the "0::<path>" line).
kj::Maybe<kj::String> parseCgroupV2Path(kj::StringPtr procSelfCgroup);

// Effective trigger threshold in bytes: min(pct of memoryMax, thresholdBytes), where either
// side may be absent. kj::none => the feature cannot trigger (misconfiguration).
kj::Maybe<uint64_t> effectiveThresholdBytes(
    const QuarvoGcPressureConfig& config, kj::Maybe<uint64_t> memoryMax);

// Registry of live isolates + the background reclaim thread. One instance per Server, created
// only when QUARVO_GC_PRESSURE is enabled.
class QuarvoGcPressureReclaimer final {
 public:
  // Reads the QUARVO_GC_PRESSURE* env vars; returns kj::none when the feature is off.
  static kj::Maybe<kj::Own<QuarvoGcPressureReclaimer>> tryCreateFromEnv();

  explicit QuarvoGcPressureReclaimer(QuarvoGcPressureConfig config);
  ~QuarvoGcPressureReclaimer() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(QuarvoGcPressureReclaimer);

  // Thread-safe; called from Server::makeWorkerImpl for every isolate (static and dynamic).
  // Dead refs are pruned lazily by the reclaim thread — no deregistration call exists, which
  // eliminates teardown-ordering use-after-free by construction.
  void registerIsolate(kj::Own<const Worker::Isolate::WeakIsolateRef> ref);

 private:
  const QuarvoGcPressureConfig config;
  kj::Maybe<kj::String> cgroupDir;  // resolved once in the ctor; none => feature inert
  kj::MutexGuarded<kj::Vector<kj::Own<const Worker::Isolate::WeakIsolateRef>>> registry;
  kj::MutexGuarded<bool> stopRequested{false};
  kj::Maybe<kj::TimePoint> lastRound;
  bool warnedNoThreshold = false;
  // MUST be the last member: the thread starts in the constructor (all state above must be
  // initialized) and kj::Thread's destructor joins (runs first, before other members die).
  kj::Thread thread;

  void threadMain();
  void tick();
  void runRound(uint64_t usageBytes, uint64_t thresholdBytes);
  kj::Maybe<uint64_t> readCgroupFile(kj::StringPtr fileName) const;
};

}  // namespace workerd::server
```

- [ ] **Step 4.2: Write the failing unit tests**

`src/workerd/server/quarvo-gc-pressure-test.c++`:

```c++
#include "quarvo-gc-pressure.h"

#include <kj/test.h>

namespace workerd::server {
namespace {

KJ_TEST("quarvo gc-pressure: env parsing — disabled unless explicitly enabled") {
  KJ_EXPECT(parseQuarvoGcPressureEnv(kj::none, kj::none, kj::none, kj::none) == kj::none);
  KJ_EXPECT(parseQuarvoGcPressureEnv("off"_kj, kj::none, kj::none, kj::none) == kj::none);
  KJ_EXPECT(parseQuarvoGcPressureEnv("0"_kj, kj::none, kj::none, kj::none) == kj::none);
  KJ_EXPECT(parseQuarvoGcPressureEnv("banana"_kj, kj::none, kj::none, kj::none) == kj::none);
  for (auto v: {"on"_kj, "1"_kj, "true"_kj, "yes"_kj, "ON"_kj, "TRUE"_kj}) {
    KJ_EXPECT(parseQuarvoGcPressureEnv(v, kj::none, kj::none, kj::none) != kj::none, v);
  }
}

KJ_TEST("quarvo gc-pressure: env parsing — values and defaults") {
  auto c = KJ_ASSERT_NONNULL(parseQuarvoGcPressureEnv("on"_kj, kj::none, kj::none, kj::none));
  KJ_EXPECT(c.thresholdPct == 60);
  KJ_EXPECT(c.thresholdBytes == kj::none);
  KJ_EXPECT(c.minIntervalMs == 30000);

  auto c2 = KJ_ASSERT_NONNULL(parseQuarvoGcPressureEnv("on"_kj, "45"_kj, "192"_kj, "5000"_kj));
  KJ_EXPECT(c2.thresholdPct == 45);
  KJ_EXPECT(KJ_ASSERT_NONNULL(c2.thresholdBytes) == 192ull << 20);
  KJ_EXPECT(c2.minIntervalMs == 5000);

  // Invalid values fall back to defaults (fail-safe), not to disabled.
  auto c3 = KJ_ASSERT_NONNULL(parseQuarvoGcPressureEnv("on"_kj, "0"_kj, "zap"_kj, "-3"_kj));
  KJ_EXPECT(c3.thresholdPct == 60);
  KJ_EXPECT(c3.thresholdBytes == kj::none);
  KJ_EXPECT(c3.minIntervalMs == 30000);
  auto c4 = KJ_ASSERT_NONNULL(parseQuarvoGcPressureEnv("on"_kj, "101"_kj, kj::none, kj::none));
  KJ_EXPECT(c4.thresholdPct == 60);
}

KJ_TEST("quarvo gc-pressure: cgroup value parsing") {
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupValue("268435456\n"_kj)) == 268435456ull);
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupValue("0"_kj)) == 0);
  KJ_EXPECT(parseCgroupValue("max\n"_kj) == kj::none);
  KJ_EXPECT(parseCgroupValue(""_kj) == kj::none);
  KJ_EXPECT(parseCgroupValue("bogus\n"_kj) == kj::none);
}

KJ_TEST("quarvo gc-pressure: /proc/self/cgroup v2 path extraction") {
  // Pure cgroup v2 (container with private namespace).
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupV2Path("0::/\n"_kj)) == "/");
  // Bare host leaf cgroup.
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupV2Path(
                "0::/user.slice/user-1000.slice/session-1.scope\n"_kj)) ==
      "/user.slice/user-1000.slice/session-1.scope");
  // Hybrid v1+v2: only the "0::" line counts.
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupV2Path(
                "12:pids:/init.scope\n1:name=systemd:/init.scope\n0::/foo\n"_kj)) == "/foo");
  // No v2 entry at all.
  KJ_EXPECT(parseCgroupV2Path("12:pids:/init.scope\n"_kj) == kj::none);
}

KJ_TEST("quarvo gc-pressure: effective threshold — lower wins") {
  QuarvoGcPressureConfig c{.thresholdPct = 50, .thresholdBytes = kj::none, .minIntervalMs = 1};

  // pct of a 256 MiB limit = 128 MiB.
  KJ_EXPECT(KJ_ASSERT_NONNULL(effectiveThresholdBytes(c, 256ull << 20)) == 128ull << 20);
  // Unlimited cgroup, no MB set => cannot trigger.
  KJ_EXPECT(effectiveThresholdBytes(c, kj::none) == kj::none);

  // MB set lower than pct => MB wins.
  c.thresholdBytes = 64ull << 20;
  KJ_EXPECT(KJ_ASSERT_NONNULL(effectiveThresholdBytes(c, 256ull << 20)) == 64ull << 20);
  // MB set higher than pct => pct wins.
  c.thresholdBytes = 200ull << 20;
  KJ_EXPECT(KJ_ASSERT_NONNULL(effectiveThresholdBytes(c, 256ull << 20)) == 128ull << 20);
  // Unlimited cgroup, MB set => MB.
  KJ_EXPECT(KJ_ASSERT_NONNULL(effectiveThresholdBytes(c, kj::none)) == 200ull << 20);
}

}  // namespace
}  // namespace workerd::server
```

- [ ] **Step 4.3: Implement the pure functions**

`src/workerd/server/quarvo-gc-pressure.c++` (first half; the thread half comes in Task 5):

```c++
#include "quarvo-gc-pressure.h"

#include <cstdlib>
#include <strings.h>  // strcasecmp

#include <fcntl.h>
#include <unistd.h>

#include <kj/debug.h>

namespace workerd::server {

kj::Maybe<QuarvoGcPressureConfig> parseQuarvoGcPressureEnv(kj::Maybe<kj::StringPtr> enabled,
    kj::Maybe<kj::StringPtr> thresholdPct,
    kj::Maybe<kj::StringPtr> thresholdMb,
    kj::Maybe<kj::StringPtr> minIntervalMs) {
  bool on = false;
  KJ_IF_SOME(e, enabled) {
    for (auto candidate: {"on", "1", "true", "yes"}) {
      if (strcasecmp(e.cStr(), candidate) == 0) on = true;
    }
  }
  if (!on) return kj::none;

  QuarvoGcPressureConfig config;
  KJ_IF_SOME(p, thresholdPct) {
    KJ_IF_SOME(v, p.tryParseAs<uint32_t>()) {
      if (v >= 1 && v <= 100) {
        config.thresholdPct = v;
      } else {
        KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_THRESHOLD_PCT out of range [1,100]; using default", p);
      }
    } else {
      KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_THRESHOLD_PCT not a number; using default", p);
    }
  }
  KJ_IF_SOME(m, thresholdMb) {
    KJ_IF_SOME(v, m.tryParseAs<uint64_t>()) {
      if (v > 0) {
        config.thresholdBytes = v << 20;
      } else {
        KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_THRESHOLD_MB must be > 0; ignoring", m);
      }
    } else {
      KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_THRESHOLD_MB not a number; ignoring", m);
    }
  }
  KJ_IF_SOME(i, minIntervalMs) {
    KJ_IF_SOME(v, i.tryParseAs<uint64_t>()) {
      config.minIntervalMs = v;
    } else {
      KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_MIN_INTERVAL_MS not a number; using default", i);
    }
  }
  return config;
}

kj::Maybe<uint64_t> parseCgroupValue(kj::StringPtr content) {
  // Trim a single trailing newline. NOTE: do NOT build a kj::StringPtr over a sub-range —
  // StringPtr requires NUL termination at [size] (debug-asserts otherwise); copy instead.
  size_t len = content.size();
  if (len > 0 && content[len - 1] == '\n') len--;
  kj::String trimmed = kj::str(kj::ArrayPtr<const char>(content.begin(), len));
  if (trimmed == "max" || trimmed.size() == 0) return kj::none;
  return trimmed.tryParseAs<uint64_t>();
}

kj::Maybe<kj::String> parseCgroupV2Path(kj::StringPtr procSelfCgroup) {
  // Work with ArrayPtr<const char> line slices (StringPtr sub-ranges would violate its
  // NUL-termination invariant).
  kj::ArrayPtr<const char> rest = procSelfCgroup.asArray();
  while (rest.size() > 0) {
    kj::ArrayPtr<const char> line = rest;
    for (size_t i = 0; i < rest.size(); i++) {
      if (rest[i] == '\n') {
        line = rest.first(i);
        break;
      }
    }
    rest = rest.size() > line.size() ? rest.slice(line.size() + 1, rest.size())
                                     : kj::ArrayPtr<const char>();
    if (line.size() >= 3 && line[0] == '0' && line[1] == ':' && line[2] == ':') {
      return kj::str(line.slice(3, line.size()));
    }
  }
  return kj::none;
}

kj::Maybe<uint64_t> effectiveThresholdBytes(
    const QuarvoGcPressureConfig& config, kj::Maybe<uint64_t> memoryMax) {
  kj::Maybe<uint64_t> result = config.thresholdBytes;
  KJ_IF_SOME(max, memoryMax) {
    uint64_t pctBytes = max / 100 * config.thresholdPct;
    KJ_IF_SOME(existing, result) {
      result = kj::min(existing, pctBytes);
    } else {
      result = pctBytes;
    }
  }
  return result;
}

}  // namespace workerd::server
```

NOTE for the implementer: `kj::StringPtr::tryParseAs`, `findFirst`, `startsWith`, `slice` all exist in the vendored kj; `strcasecmp` needs `<strings.h>`. If `tryParseAs<uint64_t>` rejects `"-3"` (it does — unsigned), the fallback-to-default branch covers it, which is what the test asserts.

- [ ] **Step 4.4: Add the BUILD targets**

In `src/workerd/server/BUILD.bazel`: add these rules next to the `server` library (before it), and add `":quarvo-gc-pressure"` to the `server` target's `deps` list. Match the `load(...)` idioms already at the top of the file (it already loads `wd_cc_library`; add `kj_test` to an existing `load("//:build/kj_test.bzl", "kj_test")` if not present — check `src/workerd/util/BUILD.bazel` line 1-10 for the exact load path used in this repo and copy it).

```python
wd_cc_library(
    name = "quarvo-gc-pressure",
    srcs = ["quarvo-gc-pressure.c++"],
    hdrs = ["quarvo-gc-pressure.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//src/workerd/io",
    ],
)

kj_test(
    src = "quarvo-gc-pressure-test.c++",
    deps = [":quarvo-gc-pressure"],
)
```

- [ ] **Step 4.5: Commit**

```bash
git add src/workerd/server/quarvo-gc-pressure.h src/workerd/server/quarvo-gc-pressure.c++ \
        src/workerd/server/quarvo-gc-pressure-test.c++ src/workerd/server/BUILD.bazel
git commit -m "quarvo: gc-pressure config/cgroup parsing + unit tests"
```

(Tests run in CI — the kj_test target gets added to the CI target list in Task 8.)

### Task 5: Reclaimer thread, cgroup reader, reclaim round

**Files:**
- Modify: `src/workerd/server/quarvo-gc-pressure.c++` (append)

- [ ] **Step 5.1: Append the class implementation**

```c++
// ======================================================================================
// QuarvoGcPressureReclaimer

namespace {

kj::Maybe<kj::String> tryReadSmallFile(kj::StringPtr path) {
  // Loop-read to EOF: /proc/self/cgroup can exceed a single small buffer on hybrid v1+v2
  // hosts, and truncating it could cut off the "0::" line we need.
  int fd = open(path.cStr(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return kj::none;
  KJ_DEFER(close(fd));
  kj::Vector<char> data;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) return kj::none;
    if (n == 0) break;
    data.addAll(kj::ArrayPtr<const char>(buf, n));
  }
  return kj::str(data.asPtr());
}

kj::Maybe<kj::StringPtr> getEnvMaybe(kj::StringPtr name) {
  const char* value = getenv(name.cStr());
  if (value == nullptr) return kj::none;
  return kj::StringPtr(value);
}

constexpr auto TICK = 1 * kj::SECONDS;

}  // namespace

kj::Maybe<kj::Own<QuarvoGcPressureReclaimer>> QuarvoGcPressureReclaimer::tryCreateFromEnv() {
  return parseQuarvoGcPressureEnv(getEnvMaybe("QUARVO_GC_PRESSURE"),
      getEnvMaybe("QUARVO_GC_PRESSURE_THRESHOLD_PCT"),
      getEnvMaybe("QUARVO_GC_PRESSURE_THRESHOLD_MB"),
      getEnvMaybe("QUARVO_GC_PRESSURE_MIN_INTERVAL_MS"))
      .map([](QuarvoGcPressureConfig config) {
    return kj::heap<QuarvoGcPressureReclaimer>(config);
  });
}

QuarvoGcPressureReclaimer::QuarvoGcPressureReclaimer(QuarvoGcPressureConfig configParam)
    : config(configParam),
      cgroupDir([]() -> kj::Maybe<kj::String> {
        KJ_IF_SOME(content, tryReadSmallFile("/proc/self/cgroup")) {
          KJ_IF_SOME(path, parseCgroupV2Path(content)) {
            return kj::str("/sys/fs/cgroup", path == "/" ? ""_kj : path.asPtr());
          }
        }
        return kj::none;
      }()),
      thread([this]() { threadMain(); }) {
  KJ_IF_SOME(dir, cgroupDir) {
    KJ_LOG(INFO, "quarvo GC-pressure reclaimer enabled", dir, config.thresholdPct,
        config.thresholdBytes.orDefault(0) >> 20, config.minIntervalMs);
  } else {
    KJ_LOG(WARNING,
        "QUARVO_GC_PRESSURE is on but no cgroup v2 hierarchy was found "
        "(/proc/self/cgroup has no 0:: entry); the feature is inert");
  }
}

QuarvoGcPressureReclaimer::~QuarvoGcPressureReclaimer() noexcept(false) {
  // Body runs before member destructors; `thread`'s destructor (first, it is the last member)
  // then joins. A GC in progress delays shutdown by at most one pause.
  *stopRequested.lockExclusive() = true;
}

void QuarvoGcPressureReclaimer::registerIsolate(
    kj::Own<const Worker::Isolate::WeakIsolateRef> ref) {
  registry.lockExclusive()->add(kj::mv(ref));
}

void QuarvoGcPressureReclaimer::threadMain() {
  if (cgroupDir == kj::none) return;  // warned in ctor
  while (true) {
    // Interruptible sleep: returns early (true) when the destructor sets the flag.
    bool stop = stopRequested.when([](const bool& s) { return s; },
        [](const bool& s) { return s; }, TICK);
    if (stop) return;
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() { tick(); })) {
      KJ_LOG(ERROR, "quarvo GC-pressure tick threw; continuing", exception);
    }
  }
}

kj::Maybe<uint64_t> QuarvoGcPressureReclaimer::readCgroupFile(kj::StringPtr fileName) const {
  auto& dir = KJ_ASSERT_NONNULL(cgroupDir);
  KJ_IF_SOME(content, tryReadSmallFile(kj::str(dir, "/", fileName))) {
    return parseCgroupValue(content);
  }
  return kj::none;
}

void QuarvoGcPressureReclaimer::tick() {
  uint64_t usage;
  KJ_IF_SOME(u, readCgroupFile("memory.current")) {
    usage = u;
  } else {
    return;  // transient read failure (or root cgroup on odd hosts); try again next tick
  }

  auto maybeThreshold = effectiveThresholdBytes(config, readCgroupFile("memory.max"));
  uint64_t threshold;
  KJ_IF_SOME(t, maybeThreshold) {
    threshold = t;
  } else {
    if (!warnedNoThreshold) {
      warnedNoThreshold = true;
      KJ_LOG(WARNING,
          "QUARVO_GC_PRESSURE: cgroup memory.max is unlimited and no "
          "QUARVO_GC_PRESSURE_THRESHOLD_MB is set; the feature cannot trigger");
    }
    return;
  }
  warnedNoThreshold = false;  // re-warn if it becomes unresolvable again later

  if (usage < threshold) return;
  KJ_IF_SOME(last, lastRound) {
    auto elapsed = kj::systemPreciseMonotonicClock().now() - last;
    if (elapsed < config.minIntervalMs * kj::MILLISECONDS) return;
  }

  runRound(usage, threshold);
  lastRound = kj::systemPreciseMonotonicClock().now();
}

void QuarvoGcPressureReclaimer::runRound(uint64_t usageBytes, uint64_t thresholdBytes) {
  // Snapshot strong refs under the mutex (pruning dead entries), then GC OUTSIDE the mutex so
  // registration from other threads never blocks behind a GC pause.
  kj::Vector<kj::Own<const Worker::Isolate>> live;
  {
    auto locked = registry.lockExclusive();
    kj::Vector<kj::Own<const Worker::Isolate::WeakIsolateRef>> stillAlive;
    for (auto& weak: *locked) {
      KJ_IF_SOME(strong, weak->tryAddStrongRef()) {
        live.add(kj::mv(strong));
        stillAlive.add(kj::mv(weak));
      }
    }
    *locked = kj::mv(stillAlive);
  }

  uint reclaimed = 0, skippedBusy = 0;
  uint64_t after = usageBytes;
  for (auto& isolate: live) {
    if (isolate->getCurrentLoad() > 0) {
      // A request holds or awaits this isolate's lock; skip it (next round will retry). A busy
      // isolate is executing JS, where V8's own allocation-driven GCs still run.
      skippedBusy++;
      continue;
    }
    isolate->memoryPressureReclaim();
    reclaimed++;
    KJ_IF_SOME(u, readCgroupFile("memory.current")) {
      after = u;
      if (u < thresholdBytes) break;  // pressure relieved; stop early
    }
  }

  KJ_LOG(INFO, "quarvo GC-pressure reclaim round", usageBytes >> 20, after >> 20, thresholdBytes >> 20,
      reclaimed, skippedBusy, live.size());
}

}  // namespace workerd::server
```

(The final `}  // namespace workerd::server` replaces the one from Step 4.3 — i.e. insert this block before it.)

- [ ] **Step 5.2: Commit**

```bash
git add src/workerd/server/quarvo-gc-pressure.c++ src/workerd/server/quarvo-gc-pressure.h
git commit -m "quarvo: gc-pressure reclaimer thread (cgroup polling + reclaim rounds)"
```

### Task 6: `Worker::Isolate::memoryPressureReclaim()` (worker.{h,c++})

**Files:**
- Modify: `src/workerd/io/worker.h` (Isolate public section — right after `uint getLockSuccessCount() const;`, ~line 440)
- Modify: `src/workerd/io/worker.c++` (implement near `getCurrentLoad()`/`getLockSuccessCount()`, ~line 4417)

- [ ] **Step 6.1: Declare in worker.h**

After `uint getLockSuccessCount() const;` add:

```c++
  // NOTE(quarvo): Synchronously takes this isolate's lock and runs a critical memory-pressure
  // GC — a full unified (V8 + cppgc) collection that sweeps cppgc garbage and returns freed
  // pages to the OS. Safe to call from any thread; the collection runs on the calling thread
  // while it holds the lock (mirrors the inspector's foreign-thread locking pattern, e.g.
  // attachInspector/TakeHeapSnapshot). Callers should prefer idle isolates (getCurrentLoad()
  // == 0) since the pause blocks a request that arrives while it runs. Used by
  // server/quarvo-gc-pressure.{h,c++}.
  void memoryPressureReclaim() const;
```

- [ ] **Step 6.2: Implement in worker.c++**

Next to the `getCurrentLoad()` / `getLockSuccessCount()` definitions:

```c++
void Worker::Isolate::memoryPressureReclaim() const {
  // NOTE(quarvo): Impl::Lock takes the real v8::Locker; with the lock held,
  // MemoryPressureNotification(kCritical) runs the full unified GC synchronously on THIS
  // thread (see V8 api.cc Isolate::MemoryPressureNotification: a foreign thread holding the
  // Locker counts as the isolate thread). Without the lock, V8 would defer the GC to the next
  // request's JS entry — the opposite of what a background reclaimer wants.
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    Impl::Lock recordedLock(*this, Worker::Lock::TakeSynchronously(kj::none), stackScope);
    recordedLock.lock->v8Isolate->MemoryPressureNotification(
        v8::MemoryPressureLevel::kCritical);
  });
}
```

Verified context for the implementer: `Impl::Lock(const Worker::Isolate&, Worker::LockType, jsg::V8StackScope&)` (worker.c++:592-594); `Worker::Lock::TakeSynchronously(kj::Maybe<RequestObserver&>)` (worker.h:707); `recordedLock.lock` is the `kj::Own<jsg::Lock>` (see `attachInspector`, worker.c++:3520-3523, which uses this exact recipe from a non-request thread). The `KJ_DASSERT(impl.currentLock == kj::none)` in the Lock ctor runs after the v8::Locker is acquired, so cross-thread use is safe — only a genuinely recursive lock on the same thread would trip it, which this code never does.

- [ ] **Step 6.3: Commit**

```bash
git add src/workerd/io/worker.h src/workerd/io/worker.c++
git commit -m "quarvo: Worker::Isolate::memoryPressureReclaim() — locked kCritical unified GC"
```

### Task 7: Server integration (member + registration)

**Files:**
- Modify: `src/workerd/server/server.h` (~line 171, next to `inspectorIsolateRegistrar`)
- Modify: `src/workerd/server/server.c++` (includes ~top; ctor at line 180; makeWorkerImpl at ~5305-5312)

- [ ] **Step 7.1: server.h — forward declaration + member**

In the `workerd::server` namespace, before `class Server`, add:

```c++
class QuarvoGcPressureReclaimer;
```

Next to `kj::Maybe<kj::Own<InspectorServiceIsolateRegistrar>> inspectorIsolateRegistrar;` add:

```c++
  // NOTE(quarvo): background GC-pressure reclaimer (QUARVO_GC_PRESSURE). kj::none when the
  // feature is off (the default) — in that case no thread or registry exists and behavior is
  // byte-identical to stock workerd.
  kj::Maybe<kj::Own<QuarvoGcPressureReclaimer>> quarvoGcPressureReclaimer;
```

(`kj::Own` of a forward-declared type is fine here: the destructor is instantiated in server.c++ where the type is complete.)

- [ ] **Step 7.2: server.c++ — include, construct, register**

Add `#include "quarvo-gc-pressure.h"` next to the other local includes at the top.

In the `Server::Server` constructor BODY (server.c++:180-196 — the body is currently the empty `{}`), assign the member (body assignment avoids init-list/declaration-order coupling with an incomplete type):

```c++
    : /* ...existing initializer list unchanged... */ {
  quarvoGcPressureReclaimer = QuarvoGcPressureReclaimer::tryCreateFromEnv();
}
```

In `makeWorkerImpl`, immediately after the inspector registrar block (server.c++:5310-5312):

```c++
  // NOTE(quarvo): register every isolate (static and dynamic — dispatcher, tail workers, and
  // worker-loader isolates alike) with the GC-pressure reclaimer. Weak refs self-invalidate on
  // isolate teardown; dead entries are pruned lazily by the reclaim thread.
  KJ_IF_SOME(reclaimer, quarvoGcPressureReclaimer) {
    reclaimer->registerIsolate(isolate->getWeakRef());
  }
```

- [ ] **Step 7.3: Commit**

```bash
git add src/workerd/server/server.h src/workerd/server/server.c++
git commit -m "quarvo: wire QuarvoGcPressureReclaimer into Server (env-gated, weak-ref registry)"
```

### Task 8: Feature-on churn wd_test + CI target list

**Files:**
- Create: `src/workerd/api/tests/worker-loader-memory-gcpressure-test.wd-test`
- Modify: `src/workerd/api/tests/BUILD.bazel` (after the `worker-loader-memory-test` wd_test, ~line 984)
- Modify: `.github/workflows/quarvo-release.yml` (the `QUARVO_TEST_TARGETS` env)

- [ ] **Step 8.1: Create the wd-test config (reuses the existing memory-test JS)**

`src/workerd/api/tests/worker-loader-memory-gcpressure-test.wd-test`:

```capnp
# quarvo: the worker-loader memory-limit suite (MEM-1/2/3, REG-1, REARM-1) run with the
# GC-pressure reclaimer forced permanently ON (THRESHOLD_MB=1 => every interval is a reclaim
# round). Proves memoryMB enforcement and the runtime survive constant background unified GCs.
# On hosts where no cgroup v2 hierarchy is visible the reclaimer is inert and this degrades to
# a duplicate of worker-loader-memory-test — still a valid (weaker) pass; the container e2e
# (quarvo/e2e/ratchet) is the authoritative feature-on validation.
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [
    ( name = "worker-loader-memory-gcpressure-test",
      worker = (
        modules = [
          (name = "worker", esModule = embed "worker-loader-memory-test.js")
        ],
        compatibilityFlags = ["nodejs_compat", "experimental"],
        bindings = [
          (name = "loader", workerLoader = ()),
        ],
      )
    ),
  ],
);
```

- [ ] **Step 8.2: Add the wd_test target with env**

In `src/workerd/api/tests/BUILD.bazel`, after the `worker-loader-memory-test` rule:

```python
wd_test(
    size = "large",
    src = "worker-loader-memory-gcpressure-test.wd-test",
    args = ["--experimental"],
    data = ["worker-loader-memory-test.js"],
    env = {
        "QUARVO_GC_PRESSURE": "on",
        "QUARVO_GC_PRESSURE_THRESHOLD_MB": "1",
        "QUARVO_GC_PRESSURE_MIN_INTERVAL_MS": "250",
    },
)
```

- [ ] **Step 8.3: Point CI at the full target list**

In `.github/workflows/quarvo-release.yml`, set the workflow-level env (added in Task 2):

```yaml
  QUARVO_TEST_TARGETS: >-
    //src/workerd/server:quarvo-gc-pressure-test
    //src/workerd/api/tests:worker-loader-memory-test@
    //src/workerd/api/tests:worker-loader-memory-gcpressure-test@
    //src/workerd/api/tests:worker-loader-limits-test@
```

- [ ] **Step 8.4: Commit**

```bash
git add src/workerd/api/tests/worker-loader-memory-gcpressure-test.wd-test \
        src/workerd/api/tests/BUILD.bazel .github/workflows/quarvo-release.yml
git commit -m "quarvo: feature-on churn wd_test + CI test-target list"
```

### Task 9: Container e2e — ratchet reproduction + bounded sawtooth

**Files:**
- Create: `quarvo/e2e/ratchet/config.capnp`
- Create: `quarvo/e2e/ratchet/dispatcher.js`
- Create: `quarvo/e2e/ratchet/logtail.js`
- Create: `quarvo/e2e/ratchet/run.sh`
- Modify: `.github/workflows/quarvo-image-test.yml` (new `memory-ratchet` job)

- [ ] **Step 9.1: Workload config — dispatcher + tail worker + worker-loader quant**

`quarvo/e2e/ratchet/config.capnp`:

```capnp
# quarvo e2e: reproduces the per-request cppgc memory ratchet (upstream workerd#6824) with all
# three production isolate classes: a static dispatcher, a static tail worker (logtail analog),
# and a dynamic worker-loader isolate (quant analog). Driven by quarvo/e2e/ratchet/run.sh.
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "dispatcher",
      worker = (
        modules = [(name = "dispatcher.js", esModule = embed "dispatcher.js")],
        compatibilityDate = "2026-06-01",
        compatibilityFlags = ["experimental"],
        bindings = [(name = "loader", workerLoader = ())],
        tails = ["logtail"],
      )),
    ( name = "logtail",
      worker = (
        modules = [(name = "logtail.js", esModule = embed "logtail.js")],
        compatibilityDate = "2026-06-01",
      )),
  ],
  sockets = [(name = "http", address = "*:8080", http = (), service = "dispatcher")],
);
```

- [ ] **Step 9.2: Worker code**

`quarvo/e2e/ratchet/dispatcher.js` — allocates jsg::Wrappable garbage (URL/Request/Response) per request and invokes a dynamically-loaded quant that does the same. IMPORTANT for the implementer: mirror the loader-callback shape used by `src/workerd/api/tests/worker-loader-memory-test.js` (`env.loader.get(name, () => ({compatibilityDate, mainModule, modules, globalOutbound: null}))` then `.getEntrypoint()`); read that file first and copy its exact call shapes.

```js
const QUANT_CODE = `export default {
  async fetch(req) {
    const junk = [];
    for (let i = 0; i < 200; i++) {
      junk.push(new URL("https://quant.example/p/" + i));
      junk.push(new Request("https://quant.example/r/" + i, { method: "POST", body: "x".repeat(64) }));
      junk.push(new Response("q".repeat(128)));
    }
    return new Response("quant-ok " + junk.length);
  }
};`;

export default {
  async fetch(req, env) {
    const junk = [];
    for (let i = 0; i < 200; i++) {
      junk.push(new URL("https://dispatcher.example/a/" + i));
      junk.push(new Request("https://dispatcher.example/b/" + i, { headers: { "x-n": String(i) } }));
      junk.push(new Response("y".repeat(128)));
    }
    const quant = env.loader.get("quant-1", () => ({
      compatibilityDate: "2026-06-01",
      mainModule: "quant.js",
      modules: { "quant.js": QUANT_CODE },
      globalOutbound: null,
    }));
    const ep = quant.getEntrypoint();
    const res = await ep.fetch("https://quant.internal/");
    return new Response("ok " + res.status + " " + junk.length);
  }
};
```

`quarvo/e2e/ratchet/logtail.js` — every dispatcher invocation also produces a tail event into this isolate:

```js
export default {
  tail(events) {
    // Materialize per-event garbage in THIS isolate, like the production logtail worker.
    const acc = [];
    for (const e of events) acc.push(JSON.stringify(e));
    return acc.length;
  }
};
```

- [ ] **Step 9.3: The driver/assertion script**

`quarvo/e2e/ratchet/run.sh` (also runnable locally: `bash quarvo/e2e/ratchet/run.sh <image> on|off`):

```bash
#!/usr/bin/env bash
# Drives the ratchet workload against a quarvo-workerd image in a memory-capped container and
# asserts: MODE=off -> usage ratchets up (bug reproduced); MODE=on -> usage stays bounded around
# the threshold (feature works). Samples the container's own cgroup memory.current.
set -euo pipefail

IMG="${1:?usage: run.sh <image> <on|off>}"
MODE="${2:?usage: run.sh <image> <on|off>}"

# OFF mode runs UNCAPPED and shorter: with a memory cap the ratchet would OOM-kill the
# container mid-run (that's the production bug!). Growth measurement doesn't need a cap.
# ON mode runs capped at 512 MiB to prove bounded operation under the real constraint.
if [ "$MODE" = "on" ]; then
  REQUESTS="${REQUESTS:-2500}"
else
  REQUESTS="${REQUESTS:-800}"
fi
SAMPLE_EVERY=100
MEM_LIMIT="${MEM_LIMIT:-512m}"
THRESHOLD_MB="${THRESHOLD_MB:-150}"
MIN_INTERVAL_MS="${MIN_INTERVAL_MS:-2000}"
# Assertion bounds (MiB). off: growth from warm baseline must exceed OFF_MIN_GROWTH (proves the
# ratchet). on: peak must stay under ON_MAX_PEAK (threshold + one interval of accumulation +
# slack; well under the 512 MiB cap).
OFF_MIN_GROWTH="${OFF_MIN_GROWTH:-80}"
ON_MAX_PEAK="${ON_MAX_PEAK:-260}"

DIR="$(cd "$(dirname "$0")" && pwd)"
NAME="quarvo-ratchet-$MODE-$$"

RUN_ARGS=()
if [ "$MODE" = "on" ]; then
  RUN_ARGS+=(--memory="$MEM_LIMIT"
             -e QUARVO_GC_PRESSURE=on
             -e QUARVO_GC_PRESSURE_THRESHOLD_MB="$THRESHOLD_MB"
             -e QUARVO_GC_PRESSURE_MIN_INTERVAL_MS="$MIN_INTERVAL_MS")
fi

docker run -d --name "$NAME" -p 127.0.0.1:0:8080 \
  -v "$DIR:/app:ro" "${RUN_ARGS[@]}" \
  "$IMG" serve --experimental /app/config.capnp
trap 'docker logs "$NAME" | tail -50; docker rm -f "$NAME" >/dev/null' EXIT

PORT="$(docker inspect -f '{{(index (index .NetworkSettings.Ports "8080/tcp") 0).HostPort}}' "$NAME")"

# Wait for readiness (up to 30s).
for i in $(seq 1 60); do
  if curl -fsS -o /dev/null "http://127.0.0.1:$PORT/"; then break; fi
  [ "$i" = 60 ] && { echo "FATAL: server never became ready"; exit 1; }
  sleep 0.5
done

sample_mib() { docker exec "$NAME" cat /sys/fs/cgroup/memory.current | awk '{printf "%d", $1/1048576}'; }

BASELINE="$(sample_mib)"
PEAK=0
echo "req,mem_mib"
for i in $(seq 1 "$REQUESTS"); do
  curl -fsS -o /dev/null "http://127.0.0.1:$PORT/" || { echo "FATAL: request $i failed"; exit 1; }
  if [ $((i % SAMPLE_EVERY)) -eq 0 ]; then
    M="$(sample_mib)"
    echo "$i,$M"
    [ "$M" -gt "$PEAK" ] && PEAK="$M"
  fi
done
FINAL="$(sample_mib)"
GROWTH=$((FINAL - BASELINE))
echo "mode=$MODE baseline=${BASELINE}MiB final=${FINAL}MiB peak=${PEAK}MiB growth=${GROWTH}MiB"

if [ "$MODE" = "off" ]; then
  [ "$GROWTH" -ge "$OFF_MIN_GROWTH" ] || {
    echo "FAIL: expected the ratchet to grow >= ${OFF_MIN_GROWTH}MiB with the feature off"; exit 1; }
  echo "PASS: ratchet reproduced (feature off)"
else
  [ "$PEAK" -le "$ON_MAX_PEAK" ] || {
    echo "FAIL: peak ${PEAK}MiB exceeded ${ON_MAX_PEAK}MiB with the feature on"; exit 1; }
  echo "PASS: usage bounded (feature on)"
fi
```

Run `chmod +x quarvo/e2e/ratchet/run.sh`.

Calibration note for the implementer: the per-request garbage (≈600 wrappables across 3 isolates) is sized so the 800-request off-run accumulates well over 80 MiB (upstream #6824 measured ~530 MiB over 800 requests with a similar workload). If the off-run's observed growth in CI is below 80 MiB, raise the per-request loop count in the JS rather than lowering `OFF_MIN_GROWTH`; if the on-run flakes above 260 MiB, first check the reclaim-round INFO logs in `docker logs` (workerd runs without `--verbose` here, so add `--verbose` to the serve args while debugging) before widening the bound.

- [ ] **Step 9.4: Add the e2e job to quarvo-image-test.yml**

Append to `.github/workflows/quarvo-image-test.yml` `jobs:`:

```yaml
  memory-ratchet:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Log in to GHCR
        uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}

      - name: Ratchet e2e — off reproduces, on stays bounded
        run: |
          set -euxo pipefail
          TAG="${{ github.event.inputs.image_tag || '1.20260623.1-quarvo.1' }}"
          IMG="ghcr.io/${GITHUB_REPOSITORY_OWNER,,}/quarvo-workerd:${TAG}"
          docker pull "$IMG"
          bash quarvo/e2e/ratchet/run.sh "$IMG" off
          bash quarvo/e2e/ratchet/run.sh "$IMG" on
```

NOTE: this job only makes sense for images ≥ quarvo.4 (older images pass `off` but fail `on`'s premise silently — the env vars are simply ignored and `on` will fail its bound). That is fine: it is dispatch-driven with an explicit tag.

- [ ] **Step 9.5: Commit**

```bash
git add quarvo/e2e/ .github/workflows/quarvo-image-test.yml
git commit -m "quarvo: ratchet e2e (memory-capped container; off reproduces, on bounded)"
```

### Task 10: Documentation (FEATURES / INTEGRATION / MAINTENANCE / README / CHANGELOG)

**Files:**
- Modify: `quarvo/FEATURES.md` (capability matrix ~lines 10-21 + new section; follow the template at lines 153-163)
- Modify: `quarvo/INTEGRATION.md` (new section after the distribution/pinning section)
- Modify: `quarvo/MAINTENANCE.md` (seams section + CI section)
- Modify: `quarvo/README.md` (capability table)
- Modify: `quarvo/CHANGELOG.md` (`[Unreleased]`)

- [ ] **Step 10.1: FEATURES.md — matrix row + section**

Add a capability-matrix row:

```markdown
| Process-wide GC under memory pressure | ✅ Enforced (opt-in) | `QUARVO_GC_PRESSURE=on` + threshold env vars |
```

Add a section following the existing per-capability template (What it is / OSS workerd vs quarvo / How to use / Semantics / Limitations & gotchas / Verify / test):

```markdown
## GC under memory pressure (`QUARVO_GC_PRESSURE`)

**What it is.** workerd's RSS ratchets up per request and never comes back (fixed ~610 MiB
plateau, or an OOM-kill on tight pods): jsg wrappables (Request, URL, Response, tail events)
are cppgc/Oilpan garbage that only a *major unified* GC sweeps, and stock workerd never runs
one under sustained load — it hardcodes `--noincremental-marking`, which disables V8's
automatic idle major GC (upstream workerd#6824, unfixed). quarvo-workerd adds an embedder-side
reclaimer: a background thread watches the container's cgroup v2 memory usage and, past a
threshold, takes each *idle* isolate's lock and runs `MemoryPressureNotification(kCritical)` —
a full V8+cppgc collection that returns freed pages to the OS. All isolates are covered:
dispatcher, tail workers (logtail), and worker-loader isolates (quants).

**OSS workerd:** no mechanism; RSS ratchets until the internal heap limit or the pod OOMs.
**quarvo:** opt-in sawtooth around your threshold, with zero request-path cost (GCs run on a
background thread, only against isolates with no request in flight or queued).

**How to use.**

| Env var | Default | Meaning |
|---|---|---|
| `QUARVO_GC_PRESSURE` | `off` | Master switch (`on`/`1`/`true`/`yes`). Off = byte-identical to stock. |
| `QUARVO_GC_PRESSURE_THRESHOLD_PCT` | `60` | Trigger at this % of cgroup `memory.max` (the K8s **limit**). Inert if `memory.max` is unlimited. |
| `QUARVO_GC_PRESSURE_THRESHOLD_MB` | unset | Absolute trigger (MiB) against `memory.current`. When both resolve, the **lower** byte value wins. |
| `QUARVO_GC_PRESSURE_MIN_INTERVAL_MS` | `30000` | Floor between reclaim rounds (thrash guard). |

Kubernetes: the container can only see the **limit**; the **request** is not exposed in-pod.
Feed it via the Downward API so RSS sawtooths around your guaranteed memory:

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

On large/unlimited pods, set `_MB` (e.g. `256`) to cap idle memory use outright.

**Semantics.** Every second the reclaimer reads the process's cgroup v2 `memory.current` /
`memory.max` (path resolved via `/proc/self/cgroup`, so it works in containers and on bare
hosts). Above the effective threshold — and at most once per `MIN_INTERVAL` — it walks the
live isolates, skips any with a request in flight or queued (`getCurrentLoad() > 0`), runs the
locked critical GC on each idle one, and stops early once usage drops below the threshold.
Each round emits one INFO log line (before/after MiB, reclaimed/skipped counts) — visible with
`--verbose`.

**Limitations & gotchas.** cgroup v2 only (elsewhere: one warning, then inert). An isolate
that is *always* mid-request is never reclaimed by this mechanism (its own allocation-driven
GCs still run). A request arriving during an isolate's GC waits out the pause (tens of ms;
idle-gated so this is rare). `memory.current` includes page cache — the threshold semantics
are "as the OOM killer sees it". This feature does NOT shrink the live warm-isolate working
set (~13 MiB/isolate) — that needs idle-isolate eviction, a separate feature.

**Verify / test.** `worker-loader-memory-gcpressure-test` (the memoryMB suite under constant
reclaim churn) and `quarvo/e2e/ratchet/run.sh` (memory-capped container: `off` reproduces the
ratchet, `on` stays bounded). See the memory-ratchet job in `quarvo-image-test.yml`.
```

- [ ] **Step 10.2: INTEGRATION.md — consumption contract**

Add a section (after distribution/pinning):

```markdown
## Runtime env vars (GC under memory pressure)

Unlike `WorkerCode.limits` (per-isolate, set by the dispatcher at load time), the GC-pressure
reclaimer is **process-wide** and configured by the runner deployment's environment:
`QUARVO_GC_PRESSURE`, `QUARVO_GC_PRESSURE_THRESHOLD_PCT`, `QUARVO_GC_PRESSURE_THRESHOLD_MB`,
`QUARVO_GC_PRESSURE_MIN_INTERVAL_MS` — semantics and the Kubernetes Downward-API pattern are
in FEATURES.md ("GC under memory pressure"). Recommended runner setting: `on` +
`_THRESHOLD_MB` fed from `resources.requests.memory` (divisor `1Mi`). Off (unset) is
byte-identical to stock workerd.
```

Also update the `COPY --from=` example to pin by digest once the release digest exists (Task 12).

- [ ] **Step 10.3: MAINTENANCE.md — seams + CI notes**

In the seams section add:

```markdown
- **GC-pressure reclaimer (fork feature):** `src/workerd/server/quarvo-gc-pressure.{h,c++}`
  (fork-owned files; config parsing, cgroup v2 reader, registry of
  `Worker::Isolate::WeakIsolateRef`, reclaim thread). Touch points in upstream files:
  `Worker::Isolate::memoryPressureReclaim()` (io/worker.{h,c++} — synchronous full lock +
  `MemoryPressureNotification(kCritical)`, mirroring the inspector's foreign-thread lock
  pattern) and Server (server.h member + ctor init + registration in `makeWorkerImpl`, next to
  the inspector registrar). Rebase watch: `Impl::Lock`'s constructor signature and
  `AtomicWeakRef`/`getWeakRef()`.
```

In the CI section add:

```markdown
- The `quarvo-test` Dockerfile stage runs the quarvo bazel test targets on the warm disk
  cache; the target list is injected via the `QUARVO_TEST_TARGETS` build-arg from
  quarvo-release.yml (so list changes don't rotate the cache key). PRs to quarvo-main build
  and test on the base branch's warm cache without saving or publishing. Release runs emit the
  multi-arch **index digest** as a `merge`-job output and in the step summary.
- The cache RESTORE step now has a `restore-keys` prefix fallback (`bazel-disk-<arch>-`): when
  one of the hashed files (e.g. `quarvo/Dockerfile`) changes and rotates the exact key, the
  build seeds from the newest previous cache instead of going cold (~3 h). This relaxes the
  original "no restore-keys" stance; snowballing is bounded by the >100 MB trim in the
  Dockerfile and GitHub's 10 GB LRU eviction. The save path is unchanged (exact key,
  quarvo-main only).
```

- [ ] **Step 10.4: README.md capability row + CHANGELOG entry**

README capability table row:

```markdown
| GC under memory pressure (`QUARVO_GC_PRESSURE`) | RSS sawtooths at your threshold instead of ratcheting to ~610 MiB / OOM | FEATURES.md |
```

CHANGELOG under `## [Unreleased]`:

```markdown
- **Added:** opt-in GC under memory pressure (`QUARVO_GC_PRESSURE` + `_THRESHOLD_PCT` /
  `_THRESHOLD_MB` / `_MIN_INTERVAL_MS`): a background thread watches cgroup v2 memory usage and
  runs a full unified (V8+cppgc) GC on idle isolates past the threshold, so long-running runners
  hold a bounded sawtooth instead of ratcheting to ~610 MiB or OOM-killing tight pods (upstream
  workerd#6824). Off by default — unset env = byte-identical to stock. Covers dispatcher, tail
  workers, and worker-loader isolates. (#N)
```

- [ ] **Step 10.5: Commit**

```bash
git add quarvo/FEATURES.md quarvo/INTEGRATION.md quarvo/MAINTENANCE.md quarvo/README.md quarvo/CHANGELOG.md
git commit -m "quarvo: document QUARVO_GC_PRESSURE (FEATURES/INTEGRATION/MAINTENANCE/README/CHANGELOG)"
```

### Task 11: Open PR-2; pre-merge CI validation **[CI]**

- [ ] **Step 11.1: Push and open PR-2**

```bash
git push -u origin quarvo-gc-pressure
gh pr create --base quarvo-main --title "quarvo: GC under memory pressure (QUARVO_GC_PRESSURE)" \
  --body "Implements docs/superpowers/specs/2026-07-02-quarvo-gc-pressure-design.md.
Background reclaimer thread + weak-ref isolate registry; locked MemoryPressureNotification(kCritical)
on idle isolates past a cgroup-v2 threshold. Off by default (byte-identical). Includes unit tests,
a feature-on churn wd_test, a container ratchet e2e, and docs."
```

- [ ] **Step 11.2: Watch the pull_request run (warm cache — expect minutes, not hours)**

```bash
gh run watch $(gh run list --workflow=quarvo-release.yml --event=pull_request --limit 1 --json databaseId -q '.[0].databaseId')
```

Expected: build green; `quarvo-test` stage green (unit test + memory/limits/gcpressure wd_tests). Iterate on failures by pushing fixes to the PR branch (each push re-runs warm). DO NOT merge red.

- [ ] **Step 11.3: Squash-merge PR-2 (no `[skip ci]`); watch the quarvo-main cache-warm run**

```bash
gh pr merge --squash --subject "quarvo: GC under memory pressure (QUARVO_GC_PRESSURE) (#N)"
gh run watch <quarvo-main run id>
```

Expected: green (warm build + tests, cache re-saved).

---

## Phase 3 — Release, e2e, mirror, handback

### Task 12: Tag `v1.20260623.1-quarvo.4`, capture the index digest **[CI]**

- [ ] **Step 12.1: Tag the squash-merge commit and push**

```bash
git fetch origin quarvo-main
git tag v1.20260623.1-quarvo.4 origin/quarvo-main
git push origin v1.20260623.1-quarvo.4
gh run watch <tag run id>   # warm: restore quarvo-main cache, build, push per-arch digests, merge job
```

- [ ] **Step 12.2: Record the multi-arch INDEX digest**

From the merge job's step summary (Task 2.3), or directly:

```bash
docker buildx imagetools inspect ghcr.io/edutivo/quarvo-workerd:1.20260623.1-quarvo.4 \
  --format '{{json .Manifest}}' | jq -r '.digest'
```

Expected: `sha256:<64 hex chars>` — SAVE THIS; it is the handback digest. (If no local docker
auth for the private GHCR package: read it from the workflow step summary instead.)

- [ ] **Step 12.3: Cut the CHANGELOG release**

Per the AGENTS.md rule: rename `## [Unreleased]` → `## [1.20260623.1-quarvo.4] — <today>`, add
`**Upstream base:** workerd v1.20260623.1` and the `**Image:**` lines (GHCR + ACR tags + the
index digest), open a fresh empty `## [Unreleased]`. Also update INTEGRATION.md's
`COPY --from=` example to the new tag/digest. Land via a small squash PR (this one may use
`[skip ci]` — docs only).

### Task 13: Run the image e2e (both jobs) against the new tag **[CI]**

- [ ] **Step 13.1: Dispatch quarvo-image-test with the new tag**

```bash
gh workflow run quarvo-image-test.yml -f image_tag=1.20260623.1-quarvo.4
gh run watch <run id>
```

Expected green: `test-image` (amd64+arm64 memory-enforcement suite — MEM/REG assertions on the
shipped binary) AND `memory-ratchet` (off reproduces the ratchet; on stays bounded). This is
acceptance criterion 3 of the spec. If `memory-ratchet` fails its bounds, pull the run logs
(`docker logs` tail is printed on exit) and check the reclaim-round INFO lines before touching
the bounds — see the calibration note in Task 9.3.

### Task 14: Mirror to ACR and verify the digest survived

- [ ] **Step 14.1: Mirror (operator credentials required)**

Requires `az login` (igor@edutivo.com.br is Owner on the `edutivo` registry) and the GHCR
classic PAT file per `quarvo/ops/README.md`. If credentials are unavailable to the agent, hand
the exact command to the operator:

```bash
bash quarvo/ops/ghcr-to-acr.sh --pat-file <path-to-ghcr-pat> 1.20260623.1-quarvo.4
```

(`az acr import` copies server-side and preserves the multi-arch index digest.)

- [ ] **Step 14.2: Verify ACR digest == GHCR index digest**

```bash
az acr repository show-manifests --name edutivo --repository edutivo/quarvo-workerd \
  --query "[?tags[?@=='1.20260623.1-quarvo.4']].digest" -o tsv
```

Expected: identical to the digest recorded in Step 12.2. If it differs, STOP — the consumer
pin would be wrong; re-import and re-check.

### Task 15: Handback summary

- [ ] **Step 15.1: Produce the consumer-repo handback**

Deliver to the user (for the quarvo repo's `runner/Dockerfile` re-pin per the M9 pattern):

```
Image tag:    1.20260623.1-quarvo.4
Index digest: sha256:<from Step 12.2>
Pin:          COPY --from=edutivo.azurecr.io/edutivo/quarvo-workerd@sha256:<digest> /usr/local/bin/workerd /usr/local/bin/workerd

Runner deployment env (to enable the fix):
  QUARVO_GC_PRESSURE=on
  QUARVO_GC_PRESSURE_THRESHOLD_MB=<pod requests.memory in MiB — use the Downward API>
  # optional: QUARVO_GC_PRESSURE_THRESHOLD_PCT (default 60, % of the LIMIT),
  #           QUARVO_GC_PRESSURE_MIN_INTERVAL_MS (default 30000)
Unset = feature off = byte-identical to quarvo.3 behavior.
```

Include: validation evidence (links to the green quarvo-image-test run: ratchet reproduced off,
bounded on; memoryMB suite green on the shipped binary).
