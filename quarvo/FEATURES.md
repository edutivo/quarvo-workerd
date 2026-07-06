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
| `memoryMB` per-isolate cap | accepted, ignored | **enforced** (cap re-arms after breach) | ✅ Shipped |
| `cpuMs` per-request CPU cap | accepted, ignored | accepted, **not yet** enforced | 🚧 Planned (Phase B) |
| `subRequests` cap | accepted, ignored | accepted, not enforced | ⛔ Not planned (egress is quarvo's allow-list) |
| External / `ArrayBuffer` memory accounting | not counted | not counted by `memoryMB` | 🚧 Planned hardening |
| Per-isolate OOM containment (discard supervisor) | none | none — self-heal only | 🚧 Planned hardening |
| Process-wide GC under memory pressure | none | **enforced (opt-in)** — `QUARVO_GC_PRESSURE=on` + threshold env vars | ✅ Shipped |
| Runtime metering (`stolenMs` / `cpuMs`) | none | **per-loader-key `stub.getStats()` (opt-in)** — `QUARVO_RUNTIME_METERING=on` | ✅ Shipped |
| Boot config banner | none | one `quarvo-workerd: …` stderr line at startup (always) | ✅ Shipped |

## How to read each entry

Every capability below uses the same template so future features slot in identically:

- **What it is** — one line.
- **OSS workerd vs quarvo** — what stock does, what this fork does.
- **How to use** — the exact API surface / config keys.
- **Semantics on violation** — what happens when the limit is hit. (For capabilities where nothing
  "violates" — e.g. GC under memory pressure — this heading is just **Semantics**: what the
  mechanism does when it triggers.)
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
  re-arms the cap at the next request entry — the enforcer restores V8's heap limit explicitly
  (rather than waiting for a full GC), so the per-request ceiling is **sustained** for subsequent
  requests instead of drifting up after a breach. This is a **soft per-isolate ceiling**: all
  isolates share one
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
  assertions (MEM-1/2/3, REG-1, REARM-1 — the last asserts the cap re-arms after an eviction). Quick
  smoke: load a worker with `limits:{ memoryMB: 64 }` that allocates ~1 GiB and confirm the request
  fails while the runtime stays up.

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

## GC under memory pressure (`QUARVO_GC_PRESSURE`)

**Status: ✅ Shipped**

- **What it is** — workerd's RSS ratchets up per request and never comes back (a fixed ~610 MiB
  plateau, or an OOM-kill on tightly-limited pods): `jsg::Wrappable` garbage (Request, `new
  URL()`, Response, per-invocation tail-worker events) lives on the cppgc/Oilpan heap, and only a
  **major unified** GC sweeps it — but stock workerd never runs one under sustained load, because
  it hardcodes `--noincremental-marking`, which disables V8's `MemoryReducer` (the automatic idle
  major GC) at any request rate (upstream [workerd#6824](https://github.com/cloudflare/workerd/issues/6824),
  open, unfixed). quarvo-workerd adds an embedder-side reclaimer: a background thread watches the
  process's cgroup v2 memory usage and, past a threshold, takes each **idle** isolate's lock and
  runs `MemoryPressureNotification(kCritical)` — a full V8+cppgc collection that returns freed
  pages to the OS. Every isolate registered by `makeWorkerImpl` is covered: static workers
  (dispatcher, tail workers like `logtail`) and dynamic worker-loader isolates (quants) alike.

- **OSS workerd vs quarvo** — stock workerd has no such mechanism; RSS ratchets until the process
  hits its internal heap limit or the pod's OOM killer acts. This fork adds an opt-in reclaimer
  that keeps RSS sawtoothing around a configurable threshold, with no request-path cost: GCs run
  on a dedicated background thread and only touch isolates with no request in flight or queued.

- **How to use** — set the env vars on the workerd process (these are process-wide, not part of
  `WorkerCode.limits` — see [INTEGRATION.md](INTEGRATION.md)):

  | Env var | Default | Meaning |
  |---|---|---|
  | `QUARVO_GC_PRESSURE` | `off` | Master switch (`on`/`1`/`true`/`yes`, case-insensitive; anything else is off). Off is byte-identical to stock workerd — no thread, no registry. |
  | `QUARVO_GC_PRESSURE_THRESHOLD_PCT` | `60` | Trigger when cgroup `memory.current` reaches this percent of `memory.max` (the container's memory **limit**). Range 1–100. Inert if `memory.max` is unlimited. |
  | `QUARVO_GC_PRESSURE_THRESHOLD_MB` | unset | Absolute trigger in MiB, compared against `memory.current`. When both thresholds resolve, the **lower** byte value wins. Bounds-checked (≤ 2^30 MiB). This is also the **only** working knob when the cgroup has no `memory.max`. |
  | `QUARVO_GC_PRESSURE_MIN_INTERVAL_MS` | `30000` | Floor between reclaim rounds, even if usage stays above threshold (thrash guard). Must be ≤ 24 h. |

  The cgroup path is resolved from `/proc/self/cgroup` (the `0::` line), so this works both inside
  containers and on bare cgroup-v2 hosts.

  Kubernetes only exposes the **limit** in-pod; the **request** is a scheduler-side concept and
  cannot be auto-detected. Feed it in via the Downward API so RSS sawtooths around the guaranteed
  memory instead of the limit:

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

  On large or unlimited pods, set `_THRESHOLD_MB` directly (e.g. `256`) to cap idle memory use
  outright.

- **Semantics** — every second, the reclaimer reads `memory.current`/`memory.max` for the
  process's own cgroup. Once usage is at or above the effective threshold **and** at least
  `MIN_INTERVAL_MS` has elapsed since the last round, it runs a round: walk the registered
  isolates, skip any with a request in flight or queued (`getCurrentLoad() > 0`), run a locked,
  synchronous, critical GC on each idle one (serialized, never in parallel), re-reading usage
  between isolates and stopping early once it drops back below threshold. Each round emits one
  `KJ_LOG(INFO)` line — usage before/after and threshold in MiB, plus reclaimed/skipped/live
  isolate counts — **visible only when workerd is run with `--verbose`**. On shutdown, the
  reclaimer thread is joined in `~Server`; a round already in flight stops after at most one GC
  pause.

- **Limitations & gotchas** — read these before relying on this as a memory ceiling:
  - **cgroup v2 only.** On any other setup (cgroup v1, no cgroup) the feature logs one warning
    and stays inert for the life of the process.
  - **Host cgroup namespace caveat.** With a *private* cgroup namespace (the container/K8s
    default) the resolved path is the container's own root and always exists. With a *host*
    cgroup namespace, the path read from `/proc/self/cgroup` may not exist under the container's
    `/sys/fs/cgroup` mount; the reclaimer warns after 10 consecutive read failures rather than
    failing silently.
  - **Always-busy isolates are never reclaimed by this mechanism.** An isolate with a request
    permanently in flight or queued is skipped every round (its own allocation-driven GCs still
    run as normal).
  - **A request arriving mid-GC waits out the pause.** Narrow window in practice: only idle
    isolates are targeted, so a request has to race the reclaimer to land exactly during the GC.
  - **`memory.current` includes page cache** — the threshold is evaluated "as the OOM killer sees
    it", not as bare process RSS.
  - **Does not shrink the warm-isolate working set** (~13 MiB/isolate of always-live state). That
    needs idle-isolate eviction, a separate, out-of-scope feature.

- **Verify / test** — unit tests for the env-var parsing and threshold logic
  (`quarvo-gc-pressure-test@`); a churn `wd_test` variant of the memory-limit suite run with the
  reclaimer forced on (`worker-loader-memory-gcpressure-test@`), proving `memoryMB` semantics
  survive constant background unified GCs; and a container end-to-end test
  (`quarvo/e2e/ratchet/run.sh`) that runs the published image: `off` runs uncapped and reproduces
  the ratchet (800 requests, ≥80 MiB growth — a memory cap would just OOM-kill the reproduction);
  `on` stays bounded in a 512 MiB-capped container (2500 requests, peak ≤260 MiB). Wired as the
  dispatch-only `memory-ratchet` job in `quarvo-image-test.yml` (requires an image ≥ `quarvo.4`).

---

## Runtime metering (`QUARVO_RUNTIME_METERING`) — per-worker `stolenMs` + `cpuMs`

**Status: ✅ Shipped** (spec: `docs/superpowers/specs/2026-07-05-quarvo-runtime-metering-design.md`, untracked)

- **What it is** — two monotonic meters per Worker-Loader-loaded worker (per loader key, per
  isolate instance), readable synchronously by the **loader-holding parent** via
  `stub.getStats()`: thread-CPU consumed in the worker's JS slices (`cpuMs`) and
  runnable-but-not-running time stolen by other isolates sharing the JS thread (`stolenMs`).
  Built for QoS-aware admission control (pressure = observed / budget).

- **OSS workerd vs quarvo** — stock workerd has no per-isolate CPU or scheduling-delay
  accounting at all (its `IsolateObserver::LockTiming` interface exists but has no
  implementation); this fork implements it behind an env-var gate. Off ⇒ behavior byte-identical
  to stock (plus the boot banner line, below).

- **How to use** — set `QUARVO_RUNTIME_METERING=on` on the runtime, then in the parent worker:

  ```js
  const stub = env.LOADER.get(quantId, getCode);
  if (typeof stub.getStats === "function") {   // feature detection; absent on stock workerd
    const s = stub.getStats();                 // synchronous, lock-free, sub-µs
    // s = { cpuMs, stolenMs, timerLagMs, lockWaitMs, resumeDelayEstMs, startDelayMs, epoch }
  }
  ```

  All values are fractional milliseconds since isolate creation. `stolenMs` =
  `timerLagMs + lockWaitMs + resumeDelayEstMs`. `startDelayMs` (request delivery → first JS
  instruction, summed per request) is deliberately **not** part of `stolenMs`. `epoch` starts
  at 1 and increments each time an isolate is (re)created under the same loader key — counters
  (other than `epoch`) restart at 0 with each new epoch. The env value must be one of
  `on/1/true/yes/off/0/false/no` (case-insensitive); **anything else refuses to boot** (a typo
  must not silently leave a consumer on its fallback estimator believing it has ground truth).

- **Semantics** — accuracy differs by how each wakeup's "became runnable" instant is knowable:
  - `timerLagMs` — **exact**: timer fire minus scheduled deadline (monotonic clock, never the
    Spectre-frozen JS clock).
  - `lockWaitMs` — **exact**: time an entry waited for the isolate lock; ~0 on the single JS
    thread except when another thread holds the lock (inspector, GC-pressure reclaimer).
  - `resumeDelayEstMs` — **an estimate** (busy-window estimator): non-timer resumes of
    in-flight requests are charged **half the foreign JS-hold time** in the contiguous busy
    chain since the worker's own previous slice. Per-event error is bounded by the enclosing
    foreign busy window; in expectation it lands between ~0.5× and ~1× of true steal for
    Poisson I/O arrivals. Unsuitable for billing; designed for pressure ratios.
  - `cpuMs` — thread-CPU (`CLOCK_THREAD_CPUTIME_ID`) across the worker's JS slices, including
    its own allocation-triggered GC.

- **Limitations & gotchas**
  - `resumeDelayEstMs` is estimated — weight estimator-dominated steal with wider guard bands.
  - Steal caused by non-JS event-loop work (kj-native stream pumping, TLS) is under-attributed
    (busy chains are built from JS slices only).
  - Host CFS throttling appears in `stolenMs` (the thread itself was frozen) — disambiguate via
    cgroup throttle counters. `cpuMs` is immune.
  - Intra-quant queueing is invisible: R1 waiting on R2 of the same worker is not "stolen"
    (per-worker granularity; deliberate — self-overload is a lag/cpu-rate signal, not a steal
    signal).
  - Off-loop-thread lock holds (GC-pressure reclaimer GC, inspector) are **excluded** from
    `cpuMs`; their effect on the worker shows up as `lockWaitMs` instead. Windows builds report
    `cpuMs` = 0 (no thread-CPU clock; the fork ships Linux images).
  - The stub (including `getStats`) is bound to the IoContext that created it — re-`get()` the
    stub per request (cheap; named loads are cached).
  - **Never forward stats to quant code** — parent-only visibility is what keeps the Spectre
    clock-freeze meaningful one layer down (see INTEGRATION.md).

- **Verify / test** — Bazel: `//src/workerd/io:quarvo-metering-test` (env parse fatal-on-typo,
  busy-chain/charge math), `//src/workerd/api/tests:worker-loader-metering-test` (feature
  detection, self-calibrating cpuMs, stolen differential + split-counter identity, estimator
  class via RPC streams, read-mid-traffic, epoch-across-respawn) and
  `worker-loader-metering-off-test` (same JS, stock env ⇒ API absent). Container e2e:
  `quarvo/e2e/meter-overhead/run.sh <image> off|on` measures ON-vs-OFF throughput (the honest
  overhead number is measured on a quiet host and recorded here after release; the CI
  `meter-overhead` job in `quarvo-image-test.yml` asserts a loose ≤10% tripwire because shared
  runners are noisy) and asserts the boot banner contract.

---

## Boot config banner

**Status: ✅ Shipped** (requested by quarvo for fleet config audits; the one deliberate
exception to "default off = byte-identical to stock")

- **What it is** — one line on **stderr** at startup (both `workerd serve` and `workerd test`),
  printed **unconditionally** — including when every quarvo feature is off, because "nothing
  enabled" is exactly the misconfiguration that must be visible during rollouts.

- **Format (ops contract)** — stable prefix `quarvo-workerd: ` followed by the compiled-in fork
  version and key=value tokens; only the prefix and the documented keys are guaranteed, not
  their order beyond what is shown:

  ```
  quarvo-workerd: 1.20260623.1-quarvo.5 metering=on gc_pressure=on gc_threshold_mb=96 gc_threshold_pct=100 gc_min_interval_ms=500 memorymb_enforcement=available v8flags=--enforce-global-heap-limit,--maximum-global-heap-limit-factor=2
  ```

  - `metering=on|off` — resolved `QUARVO_RUNTIME_METERING` state (post-parse).
  - `gc_pressure=on|off|inert` — `inert` means enabled but no cgroup v2 hierarchy is visible
    (the reclaimer cannot act). When on/inert, the *effective* resolved numbers follow:
    `gc_threshold_mb` (after the min(MB, PCT×memory.max) resolution; `unresolved` when neither
    side can be computed yet), `gc_threshold_pct`, `gc_min_interval_ms`.
  - `memorymb_enforcement=available` — compile-time capability statement (per-worker caps are
    runtime data).
  - `v8flags=…` — comma-joined from the loaded config's `v8Flags`; `(none)` when absent.

- **Verify / test** — `//src/workerd/server:quarvo-banner-test` (composition + version-scheme
  pin); `quarvo/e2e/meter-overhead/run.sh` greps the container log for the prefix and the
  `metering=on|off` token. Fleet audit: `kubectl logs <pod> | grep 'quarvo-workerd: '`.

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
