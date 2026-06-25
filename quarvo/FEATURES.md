# quarvo-workerd features & differences from OSS workerd

This is the catalog of **what quarvo-workerd does differently from stock cloudflare/workerd, and how
to use each capability.** The fork makes the `limits` object on a Worker-Loader `WorkerCode`
actually enforced; stock workerd accepts the same object and silently ignores it.

- For **how to consume/pin** the published artifact, see [INTEGRATION.md](INTEGRATION.md).
- For **fork internals, the rebase seams, and build/release**, see [MAINTENANCE.md](MAINTENANCE.md).

All limits live on `WorkerCode.limits` as camelCase keys (matching the `JSG_STRUCT` in
`src/workerd/io/io-channels.h`): `cpuMs`, `subRequests`, `memoryMB`. **Omitting a key ⇒ stock
behavior** — that dimension is created with no cap, byte-for-byte identical to stock workerd. quarvo
should pass a limit only when the quant declares one.

## Capability matrix

| Capability | OSS workerd | quarvo-workerd | Status |
|---|---|---|---|
| `memoryMB` per-isolate cap | accepted, ignored | **enforced** (self-heal on breach) | ✅ Shipped |
| `cpuMs` per-request CPU cap | accepted, ignored | accepted, **not yet** enforced | 🚧 Planned (Phase B) |
| `subRequests` cap | accepted, ignored | accepted, not enforced | ⛔ Not planned (egress is quarvo's allow-list) |
| External / `ArrayBuffer` memory accounting | not counted | not counted by `memoryMB` | 🚧 Planned hardening |
| Per-isolate OOM containment (discard supervisor) | none | none — self-heal only | 🚧 Planned hardening |

## How to read each entry

Every capability below uses the same template so future features slot in identically:

- **What it is** — one line.
- **OSS workerd vs quarvo** — what stock does, what this fork does.
- **How to use** — the exact API surface / config keys.
- **Semantics on violation** — what happens when the limit is hit.
- **Limitations & gotchas** — what it does *not* protect against.
- **Verify / test** — where to confirm the behavior.

---

## `memoryMB` — per-isolate memory cap

**Status: ✅ Shipped**

- **What it is** — a per-isolate ceiling on a loaded worker's V8 **old generation**, keyed by
  `(workerLoader binding id, name)`. It applies to the whole loaded worker/isolate, **not**
  per-entrypoint.

- **OSS workerd vs quarvo** — stock workerd accepts `limits.memoryMB` and never applies it (it
  ships an inert `NullIsolateLimitEnforcer` and sets no V8 `ResourceConstraints`). This fork's
  `QuarvoIsolateLimitEnforcer` caps the old generation via `ResourceConstraints` and registers V8's
  near-heap-limit callback.

- **How to use** — pass it on the `WorkerCode.limits` you return from the Worker Loader callback:

  ```js
  // inside env.LOADER.get(name, () => ({ ... }))
  return {
    compatibilityDate: quant.spec.compatibilityDate,
    mainModule: 'main.js',
    modules: { 'main.js': source },
    limits: { memoryMB: quant.spec.limits.memoryMB },  // e.g. 128
  };
  ```

- **Semantics on violation** — the offending **request** is terminated with a clean
  `"Worker has exceeded memory limit."` error and **the workerd process survives**. The warm isolate
  self-heals (V8 restores the cap after the request's allocations are collected) and remains capped
  for subsequent requests. This is a **soft per-isolate ceiling**: all isolates share one
  process-wide ~4 GiB pointer-compression cage, so the cap bounds an individual function's heap and
  fails its over-limit requests cleanly — it does **not** hard-partition address space between
  functions. (There is no idle-eviction in self-hosted workerd, which is why the design self-heals
  the warm isolate rather than discarding it.)

- **Limitations & gotchas** — read these before relying on the cap as a sandbox:
  - **Single oversized allocation → possible fatal OOM.** A single JS allocation whose size alone
    exceeds the headroom the enforcer grants (and what V8 accommodates across its GC/retry rounds)
    can still reach `V8::FatalProcessOutOfMemory` → `abort()` **before** termination unwinds —
    taking the whole process (and all co-resident isolates) down. The enforcer grants generous
    headroom to shrink this window, but it is not closed. **Mitigate** with conservative caps and by
    not exposing the loader to fully-untrusted code without additional process-level sandboxing.
  - **`ArrayBuffer` / external memory is not counted.** `memoryMB` caps the V8 old generation only.
    `ArrayBuffer`/`SharedArrayBuffer` backing stores are tracked as *external* memory, so a worker
    can allocate large ArrayBuffers beyond `memoryMB`. (workerd separately caps a single
    Blob/buffering operation at 128 MB, but not aggregate ArrayBuffer use.)

- **Verify / test** — `src/workerd/api/tests/worker-loader-memory-test.js` holds the canonical
  assertions (MEM-1/2/3, REG-1). Quick smoke: load a worker with `limits:{ memoryMB: 64 }` that
  allocates ~1 GiB and confirm the request fails while the runtime stays up.

---

## `cpuMs` — per-request CPU time cap

**Status: 🚧 Planned (Phase B)**

- **What it is** — a per-request CPU-time budget for a loaded worker.

- **OSS workerd vs quarvo** — both accept `limits.cpuMs`; **neither enforces it yet.** Enforcement
  (a CPU watchdog) is the planned Phase B of this fork.

- **How to use** — you may declare `limits: { cpuMs: N }` today, but it is currently inert in both
  stock workerd and this fork. Declaring it now is forward-compatible (it will start biting once
  Phase B lands) and harmless until then.

- **Semantics on violation** — none yet (unenforced).

- **Limitations & gotchas** — quarvo's dispatcher already guarantees a **wall-clock abort**, so the
  remaining exposure is **CPU-bound (not I/O-bound) runaways only** — work that burns CPU without
  yielding long enough for the wall-clock abort to be the effective bound. Treat `cpuMs` as
  defense-in-depth that is not yet active.

- **Verify / test** — n/a until Phase B ships an enforcement test.

---

## `subRequests` — subrequest count cap

**Status: ⛔ Not planned (handled elsewhere)**

- **What it is** — a cap on the number of outbound subrequests (egress fetches) a loaded worker may
  make per request.

- **OSS workerd vs quarvo** — both accept `limits.subRequests`; this fork does not enforce it.

- **How to use** — n/a here. quarvo controls egress via its **allow-list** at the dispatcher layer,
  which is the intended enforcement point, so this fork does not duplicate it.

- **Semantics on violation** — none in this fork (enforced by quarvo, not workerd).

- **Limitations & gotchas** — do not rely on workerd to bound egress count; rely on quarvo's
  allow-list.

- **Verify / test** — n/a (out of scope for this fork).

---

## Planned hardening

Tracked follow-ups that would tighten the `memoryMB` cap toward a true sandbox. None are in the
current (Phase A) scope; closing them requires deeper V8 integration.

- **External-memory / `ArrayBuffer` accounting** — count backing-store bytes against the cap, not
  just the V8 old generation.
- **Single-oversized-allocation OOM containment** — a custom fatal-error / OOM handler that tears
  down **just the offending isolate** instead of letting `V8::FatalProcessOutOfMemory` `abort()` the
  whole process.
- **Per-isolate discard supervisor** — evict/replace an isolate that trips its limit, rather than
  self-healing it in place.

---

## Adding a new feature to this page

When the fork starts enforcing a new dimension:

1. Add a row to the **Capability matrix** (OSS behavior, quarvo behavior, status emoji).
2. Add a section using the template under **How to read each entry**.
3. If the new feature changes the consumption contract (new `limits` key, new config), update
   [INTEGRATION.md](INTEGRATION.md); if it touches a rebase seam, update
   [MAINTENANCE.md](MAINTENANCE.md).
4. If it appears in the quick-glance table, update [README.md](README.md).
