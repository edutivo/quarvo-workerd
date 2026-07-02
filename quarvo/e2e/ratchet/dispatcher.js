// quarvo e2e: dispatcher isolate. Allocates jsg::Wrappable garbage (URL/Request/Response) per
// request — the shape of the per-request cppgc ratchet (upstream workerd#6824) — and also
// invokes a dynamically-loaded "quant" isolate (via the Worker Loader binding) that does the
// same, so all three production isolate classes (dispatcher, tail worker, worker-loader quant)
// accumulate garbage on every request. Mirrors the worker-loader call shapes used by
// src/workerd/api/tests/worker-loader-memory-test.js and worker-loader-test.js:
// `env.loader.get(name, () => ({compatibilityDate, mainModule, modules, globalOutbound}))` then
// `.getEntrypoint().fetch(url)`.
// Workload sizing (calibrated empirically against the shipped 1.20260623.1-quarvo.4 image):
// 800 iterations x (URL + Request(1 KiB body, padded header) + Response(1 KiB body)) in BOTH
// the dispatcher and the quant, so each HTTP request churns ~4800 jsg wrappables plus ~5 MiB of
// body/header strings held by them. All of it is dropped at end-of-request; only a unified
// (V8+cppgc) GC gets the cppgc share back — that's the ratchet under test. Measured retention
// with the feature off: ~400 KB/request (313 MiB over 800 requests) — ~3.9x the 80 MiB
// OFF_MIN_GROWTH assertion in run.sh. Retention scales with wrappable count far more than with
// payload bytes, so if recalibration is ever needed, adjust the loop count first.
const QUANT_CODE = `export default {
  async fetch(req) {
    const junk = [];
    for (let i = 0; i < 800; i++) {
      junk.push(new URL("https://quant.example/p/" + i));
      junk.push(new Request("https://quant.example/r/" + i, {
        method: "POST",
        headers: { "x-pad": "p".repeat(1024) },
        body: "x".repeat(1024),
      }));
      junk.push(new Response("q".repeat(1024)));
    }
    return new Response("quant-ok " + junk.length);
  }
};`;

export default {
  async fetch(req, env) {
    const junk = [];
    for (let i = 0; i < 800; i++) {
      junk.push(new URL("https://dispatcher.example/a/" + i));
      junk.push(new Request("https://dispatcher.example/b/" + i, {
        method: "POST",
        headers: { "x-n": String(i), "x-pad": "h".repeat(1024) },
        body: "y".repeat(1024),
      }));
      junk.push(new Response("z".repeat(1024)));
    }

    const quant = env.loader.get("quant-1", () => ({
      compatibilityDate: "2026-06-01",
      mainModule: "quant.js",
      modules: { "quant.js": QUANT_CODE },
      globalOutbound: null,
    }));
    const res = await quant.getEntrypoint().fetch("https://quant.internal/");

    // No explicit action needed to produce a tail event: the `tails = ["logtail"]` binding in
    // config.capnp makes every completed request on this worker automatically dispatch a tail
    // event into the logtail isolate (see tail-worker-test.wd-test for the same pattern).
    return new Response("ok " + res.status + " " + junk.length);
  },
};
