# Changelog — quarvo-workerd

All notable **quarvo-specific** changes to this fork of
[cloudflare/workerd](https://github.com/cloudflare/workerd), newest first.

This is a **fork changelog**: it records only what quarvo **adds or changes relative to upstream
workerd** — it is *not* upstream's release notes (for those, see
[workerd releases](https://github.com/cloudflare/workerd/releases)). Each quarvo release pins an
upstream base (`workerd v<X>`) and applies the deltas below on top of it. For the *current* behavior
and semantics of each capability (rather than its history), see [FEATURES.md](FEATURES.md).

**Versioning:** `<upstream-workerd-version>-quarvo.<N>` — e.g. `1.20260623.1-quarvo.3` is upstream
workerd `v1.20260623.1` plus quarvo patch series `3`.
**Images (multi-arch: amd64 + arm64):** `ghcr.io/edutivo/quarvo-workerd:<version>` (GHCR) and, when
mirrored, `edutivo.azurecr.io/edutivo/quarvo-workerd:<version>` (ACR).

Categories: **Added** (new capability) · **Changed** (behavior change) · **Fixed** (bug fix) ·
**Internal** (build / CI / docs — no runtime effect for users).

## [Unreleased]

### Added
- **Opt-in GC under memory pressure** (`QUARVO_GC_PRESSURE` + `_THRESHOLD_PCT` / `_THRESHOLD_MB` /
  `_MIN_INTERVAL_MS`): a background thread watches the process's cgroup v2 memory usage and runs a
  full unified (V8+cppgc) GC on idle isolates once usage crosses the threshold, so long-running
  runners hold a bounded sawtooth instead of ratcheting up to a ~610 MiB plateau or OOM-killing
  tightly-limited pods (upstream [workerd#6824](https://github.com/cloudflare/workerd/issues/6824),
  unfixed). Off by default — with the env vars unset, behavior is byte-identical to stock workerd.
  Covers every isolate the runtime creates: static workers (dispatcher, tail workers like
  `logtail`) and dynamic worker-loader isolates (quants) alike. See
  [FEATURES.md](FEATURES.md#gc-under-memory-pressure-quarvo_gc_pressure). ([#9])

### Internal
- Added this `CHANGELOG.md` and `quarvo/AGENTS.md` (the latter requires keeping this changelog
  current on every quarvo change / release); linked the changelog from `quarvo/README.md` and the
  root `AGENTS.md`.
- CI now runs the quarvo bazel test targets on the warm disk cache for PRs to
  quarvo-main and quarvo-main pushes; the cache restore gained a `restore-keys` fallback so
  cache-key rotations no longer force a cold rebuild; release runs capture the multi-arch
  **index digest** in the job summary/outputs for `@sha256:` pinning. ([#8])

## [1.20260623.1-quarvo.3] — 2026-06-25

**Upstream base:** workerd `v1.20260623.1` · **Image:** `…/quarvo-workerd:1.20260623.1-quarvo.3`

### Fixed
- **`memoryMB` cap now re-arms after an over-cap eviction (sustained per-request ceiling).** Before
  this, once a warm isolate terminated a request for exceeding its memory cap, V8's heap limit could
  stay raised — V8 only auto-restores it during a full GC with live heap below 50% of the cap, which
  may never happen on an idle isolate — so later requests on that isolate ran against a higher
  ceiling instead of `memoryMB`. The enforcer now restores the cap explicitly at the next request
  entry (isolate-lock-held, no forced GC). The per-fire headroom grant was also changed from
  multiplicative to additive so the ceiling can't ratchet upward across back-to-back over-cap events.
  ([#6])

### Internal
- Reorganized `quarvo/` docs into a layered set — `README` (overview + doc map) + `FEATURES`
  (per-capability reference) over `INTEGRATION` / `MAINTENANCE`. ([#4])
- `quarvo-release` CI now serializes per ref to avoid duplicate cache-warm builds. ([#5])

## [1.20260623.1-quarvo.2] — 2026-06-25

**Upstream base:** workerd `v1.20260623.1` · **Image:** `…/quarvo-workerd:1.20260623.1-quarvo.2`

### Internal
- Persisted the Bazel disk cache across CI runs — warm rebuilds drop from ~3 h to minutes. No
  runtime or behavior change versus `quarvo.1`.

## [1.20260623.1-quarvo.1] — 2026-06-25

**Upstream base:** workerd `v1.20260623.1` · **Image:** `…/quarvo-workerd:1.20260623.1-quarvo.1`

First quarvo release.

### Added
- **Per-isolate `memoryMB` enforcement for dynamically-loaded Workers** (the Worker Loader binding,
  `env.LOADER.get(name, getCode)`). A loaded worker that exceeds its declared `limits.memoryMB` has
  the offending **request** terminated with a clean `OVERLOADED: Worker exceeded memory limit.`
  error; the workerd **process survives** and the warm isolate stays usable. With **no** limit
  declared, a loaded isolate is byte-for-byte equivalent to stock workerd at the same tag. See
  [FEATURES.md](FEATURES.md#memorymb--per-isolate-memory-cap).
- **Multi-arch OCI image** (`linux/amd64` + `linux/arm64`), built natively per arch and published to
  GHCR.
- **CI**: a release pipeline plus a memory-enforcement test that runs against the shipped image on
  both architectures.
- **Fork docs**: `MAINTENANCE.md` and `INTEGRATION.md` (plus the cold-build fixes the upstream base
  needed — clang-19 from Debian trixie, an explicit bazelisk install, a TypeScript / `@types/node`
  pin, and a `noCheck` stopgap for upstream's in-progress type migration).

### Known limitations
- `memoryMB` caps the V8 **old generation** only; `ArrayBuffer` / external memory is **not** counted
  yet (planned hardening — see [FEATURES.md](FEATURES.md)).
- A single allocation larger than the headroom the enforcer grants can still fatally OOM the process
  before the request is torn down; use conservative caps for untrusted code.

[#9]: https://github.com/edutivo/quarvo-workerd/pull/9
[#8]: https://github.com/edutivo/quarvo-workerd/pull/8
[#6]: https://github.com/edutivo/quarvo-workerd/pull/6
[#5]: https://github.com/edutivo/quarvo-workerd/pull/5
[#4]: https://github.com/edutivo/quarvo-workerd/pull/4
