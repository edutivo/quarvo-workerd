# quarvo-workerd

A thin, rebaseable fork of [cloudflare/workerd](https://github.com/cloudflare/workerd) that adds
**real per-isolate resource-limit enforcement** for workers loaded through the **Worker Loader
binding** (`env.LOADER.get(name, getCode)`). Stock OSS workerd accepts a `limits` object on a
`WorkerCode` but ignores it; this fork makes it bite — starting with `memoryMB`.

The fork tracks upstream release tags and keeps the diff minimal: with **no** limit declared, a
loaded isolate is byte-for-byte equivalent to stock workerd at the same tag.

## Capabilities at a glance

| Capability | Status | Details |
|---|---|---|
| `memoryMB` — per-isolate memory cap | ✅ Shipped | [FEATURES.md](FEATURES.md#memorymb--per-isolate-memory-cap) |
| `cpuMs` — per-request CPU cap | 🚧 Planned (Phase B) | [FEATURES.md](FEATURES.md#cpums--per-request-cpu-time-cap) |
| `subRequests` — subrequest count cap | ⛔ Not planned (quarvo allow-list) | [FEATURES.md](FEATURES.md#subrequests--subrequest-count-cap) |
| GC under memory pressure (`QUARVO_GC_PRESSURE`) | ✅ Shipped (opt-in) | [FEATURES.md](FEATURES.md#gc-under-memory-pressure-quarvo_gc_pressure) |

## Which doc do I read?

| If you are… | …read | Covers |
|---|---|---|
| A quarvo developer **using** the fork's features | **[FEATURES.md](FEATURES.md)** | What differs from OSS workerd, how to use each capability, semantics, limitations, status |
| **Wiring** the artifact into the quarvo runtime | **[INTEGRATION.md](INTEGRATION.md)** | The `WorkerCode.limits` contract, distribution/pinning (OCI / npm), the runner swap, verifying it |
| **Maintaining / rebasing** the fork, or building & releasing it | **[MAINTENANCE.md](MAINTENANCE.md)** | What the fork changes, the seams to re-verify on rebase, build/test, CI caching |
| Seeing **what changed** between fork updates | **[CHANGELOG.md](CHANGELOG.md)** | Timeline of quarvo-specific changes vs upstream, per release |

## Quick start (consumer)

Declare the limit on the `WorkerCode` you return from the Worker Loader callback:

```js
// inside env.LOADER.get(name, () => ({ ... }))
return {
  compatibilityDate: quant.spec.compatibilityDate,
  mainModule: 'main.js',
  modules: { 'main.js': source },
  limits: { memoryMB: 128 },   // enforced by this fork; omit ⇒ stock behavior
};
```

See [FEATURES.md](FEATURES.md) for what each key does and what is/isn't enforced, and
[INTEGRATION.md](INTEGRATION.md) for pinning the artifact into your build.
