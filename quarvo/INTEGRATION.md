# How quarvo consumes quarvo-workerd

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
    cpuMs: quant.spec.limits.cpuMs,       // Phase B (CPU watchdog) — see below
    memoryMB: quant.spec.limits.memoryMB, // Phase A (enforced by this fork)
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

When a loaded worker exceeds its `memoryMB` cap, **the offending request is terminated with an
error and the workerd process survives**; the warm isolate self-heals and remains capped for
subsequent requests. This is a soft per-isolate ceiling (all isolates share one process-wide ~4 GiB
pointer-compression cage), so it bounds an individual function's heap and fails its over-limit
requests cleanly — it does not hard-partition address space between functions.

`cpuMs` is **not yet enforced** by this fork (Phase B). quarvo's dispatcher already guarantees a
wall-clock abort, so the gap is CPU-bound (not I/O-bound) runaways only. `subRequests` is likewise
unenforced here (quarvo controls egress via its allow-list).

### Known limitations of the memory cap (read before relying on it as a sandbox)

The cap uses V8's near-heap-limit callback + `TerminateExecution`, which bounds **incrementally**
growing heaps cleanly. Two residual gaps remain in Phase A:

- **Single oversized allocation → possible fatal OOM.** A single JS allocation whose size alone
  exceeds the headroom the enforcer grants (and what V8 accommodates across its GC/retry rounds)
  can still reach `V8::FatalProcessOutOfMemory` → `abort()` **before** termination unwinds — taking
  the whole process (and all co-resident isolates) down. The enforcer grants generous headroom to
  make this window small, but it is not closed. Mitigate by setting **conservative caps** and not
  exposing the loader to fully-untrusted code without additional process-level sandboxing.
- **ArrayBuffer / external memory is not counted.** `memoryMB` caps the V8 **old generation**.
  `ArrayBuffer`/`SharedArrayBuffer` backing stores are tracked as *external* memory, not old-gen, so
  a worker can allocate large ArrayBuffers beyond `memoryMB`. (workerd separately caps a single
  Blob/buffering operation at 128 MB, but not aggregate ArrayBuffer use.)

Closing both fully requires deeper V8 integration (a custom fatal-error/OOM handler that tears down
just the offending isolate, and external-memory accounting) — tracked as follow-up hardening, not in
the Phase A scope.

## Distribution / pinning (drop-in swap)

The first release matches quarvo's pinned upstream tag (`v1.20260623.1`) so **only enforcement
changes** are introduced. quarvo's `runner/Dockerfile` swaps its stock install for the fork. Two
supported shapes (pick one — see the open decisions in the project notes):

**A. OCI image (this is the chosen distribution).** `quarvo/Dockerfile` builds the patched workerd
and produces an image with the (statically-linked) binary at `/usr/local/bin/workerd`. The
`.github/workflows/quarvo-release.yml` workflow builds it and pushes to
`ghcr.io/edutivo/quarvo-workerd:<upstreamTag>-quarvo.N` (e.g. `1.20260623.1-quarvo.1`). quarvo's
`runner/Dockerfile` then swaps its install:

```dockerfile
# was: RUN npm i -g workerd@1.20260623.1
COPY --from=ghcr.io/edutivo/quarvo-workerd:1.20260623.1-quarvo.1 \
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
