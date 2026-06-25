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
