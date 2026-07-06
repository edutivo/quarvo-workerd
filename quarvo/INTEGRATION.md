# How quarvo consumes quarvo-workerd

This doc is the **consumption contract**: the JS-visible `limits` API, how to pin/swap the published
artifact, and how to verify the swap. For **what each limit does and what is/isn't enforced**, see
[FEATURES.md](FEATURES.md); for fork internals and the rebase seams, see [MAINTENANCE.md](MAINTENANCE.md).

## Contract (JS-visible API)

quarvo's dispatcher builds a `WorkerCode` in its Worker Loader callback and sets `limits` from each
function's declared limits:

```js
// inside env.LOADER.get(name, () => ({ ... }))
return {
  compatibilityDate: quant.spec.compatibilityDate,
  mainModule: 'main.js',
  modules: { 'main.js': source },
  // ...
  limits: {
    cpuMs: quant.spec.limits.cpuMs,       // accepted; not yet enforced — see FEATURES.md
    memoryMB: quant.spec.limits.memoryMB, // enforced by this fork — see FEATURES.md
  },
};
```

- **Keys are exactly `cpuMs`, `subRequests`, `memoryMB`** (camelCase, matching the `JSG_STRUCT` in
  `src/workerd/io/io-channels.h`). Do not rename them.
- `memoryMB` is a **per-isolate** cap (it applies to the whole loaded worker / isolate, keyed by
  `(workerLoader binding id, name)`), not per-entrypoint. Pass it on `WorkerCode.limits`.
- **Omitting a limit ⇒ stock behavior.** If `memoryMB` is unset, the isolate is created with no cap,
  identical to stock workerd. quarvo should pass a limit only when the quant declares one.

## Enforcement behavior

What each limit does, what happens on a breach, and the known limitations of the memory cap (read
these before relying on it as a sandbox) now live in **[FEATURES.md](FEATURES.md)** — the single
source of truth for behavior. In short: a `memoryMB` breach terminates the offending **request**,
the process survives, and the warm isolate self-heals and stays capped; `cpuMs`/`subRequests` are
**not** enforced by this fork. See [FEATURES.md](FEATURES.md) for the full semantics and gotchas.

## Runtime env vars (GC under memory pressure)

Unlike `WorkerCode.limits` (per-isolate, set by the dispatcher at load time from each function's
declared limits), the GC-pressure reclaimer is configured **process-wide**, via the runner
deployment's environment: `QUARVO_GC_PRESSURE`, `QUARVO_GC_PRESSURE_THRESHOLD_PCT`,
`QUARVO_GC_PRESSURE_THRESHOLD_MB`, `QUARVO_GC_PRESSURE_MIN_INTERVAL_MS`. Full semantics, defaults,
and the Kubernetes Downward API pattern (for feeding the pod's memory *request* in, since it isn't
visible in-container) are in **[FEATURES.md](FEATURES.md#gc-under-memory-pressure-quarvo_gc_pressure)**.

Recommended runner setting: `QUARVO_GC_PRESSURE=on` with `QUARVO_GC_PRESSURE_THRESHOLD_MB` fed from
`resources.requests.memory` (Downward API, divisor `1Mi`). Leaving all of these **unset** is
byte-identical to stock workerd — no thread, no registry, no behavior change.

## Runtime env vars (runtime metering)

`QUARVO_RUNTIME_METERING=on` enables the per-loader-key `stub.getStats()` meters (`cpuMs`,
`stolenMs` + split counters, `startDelayMs`, `epoch`). Accepted values: `on/1/true/yes` /
`off/0/false/no` (case-insensitive); unset/empty = off. **Any other value refuses to boot** —
deliberately, so a typo'd deploy cannot run silently on the dispatcher's fallback estimator
while dashboards believe they have runtime ground truth. Full semantics and accuracy notes:
[FEATURES.md](FEATURES.md#runtime-metering-quarvo_runtime_metering--per-worker-stolenms--cpums).

Consumer contract:

- `dispatcher.js` must feature-detect (`typeof stub.getStats === "function"`) and fall back to
  its statistical estimator when absent — the same bundle then runs unmodified on stock workerd.
- **Never expose `getStats()` values to quant code** (via env, props, responses, or timing side
  channels you control). Parent-only visibility is what keeps the Spectre clock-freeze
  meaningful one layer down; re-exporting the counters to children re-opens cross-isolate
  timing measurement.
- Counters reset when an isolate is respawned under the same loader key; detect via the
  `epoch` field, not value-decrease heuristics.

At boot the runtime prints a one-line config banner (`quarvo-workerd: <version> metering=… …`)
to stderr regardless of flag state — treat it as the authoritative "what is actually enabled"
record for fleet audits (format contract in
[FEATURES.md](FEATURES.md#boot-config-banner)).

## Distribution / pinning (drop-in swap)

The first release matches quarvo's pinned upstream tag (`v1.20260623.1`) so **only enforcement
changes** are introduced. quarvo's `runner/Dockerfile` swaps its stock install for the fork. Two
supported shapes (pick one — see the open decisions in the project notes):

**A. OCI image (this is the chosen distribution).** `quarvo/Dockerfile` builds the patched workerd
and produces an image with the (statically-linked) binary at `/usr/local/bin/workerd`. The
`.github/workflows/quarvo-release.yml` workflow builds it and pushes to
`ghcr.io/edutivo/quarvo-workerd:<upstreamTag>-quarvo.N`; the image is mirrored to
`edutivo.azurecr.io/edutivo/quarvo-workerd` (ACR), which is what production pulls from. quarvo's
`runner/Dockerfile` then swaps its install, pinning the release's multi-arch **index digest** (the
release workflow emits it in the job summary/outputs) for an exact, immutable build:

```dockerfile
# was: RUN npm i -g workerd@1.20260623.1
COPY --from=edutivo.azurecr.io/edutivo/quarvo-workerd@sha256:c0d944e8ddbee08032e0e2a9e77ec8c5223a9ced490c3a12e3f9cd19da6070c5 \
     /usr/local/bin/workerd /usr/local/bin/workerd
```

The digest pin is exact; the tag (`:1.20260623.1-quarvo.4`) remains the readable alternative if you
prefer legibility over immutability:

```dockerfile
COPY --from=edutivo.azurecr.io/edutivo/quarvo-workerd:1.20260623.1-quarvo.4 \
     /usr/local/bin/workerd /usr/local/bin/workerd
```

The image is also directly runnable (`ENTRYPOINT ["/usr/local/bin/workerd"]`) if quarvo prefers to
base its runner on it instead of copying the binary.

**B. Scoped npm package** (mirrors the upstream `workerd` meta-package + platform binary packages):

```dockerfile
# was: RUN npm i -g workerd@1.20260623.1
RUN npm i -g @edutivo/workerd@1.20260623.1-quarvo.1
```

Either way, the published version string encodes the upstream tag plus a `-quarvo.N` suffix so
quarvo can pin an exact, reproducible build.

## Verifying the swap took effect

A quick smoke test: load a worker with `limits:{ memoryMB: 64 }` that allocates ~1 GiB and confirm
the request fails while the runtime stays up (see `src/workerd/api/tests/worker-loader-memory-test.js`
for the canonical assertions).
