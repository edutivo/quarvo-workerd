// quarvo e2e: dispatcher isolate. Allocates jsg::Wrappable garbage (URL/Request/Response) per
// request — the shape of the per-request cppgc ratchet (upstream workerd#6824) — and also
// invokes a dynamically-loaded "quant" isolate (via the Worker Loader binding) that does the
// same, so all three production isolate classes (dispatcher, tail worker, worker-loader quant)
// accumulate garbage on every request. Mirrors the worker-loader call shapes used by
// src/workerd/api/tests/worker-loader-memory-test.js and worker-loader-test.js:
// `env.loader.get(name, () => ({compatibilityDate, mainModule, modules, globalOutbound}))` then
// `.getEntrypoint().fetch(url)`.
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
    const res = await quant.getEntrypoint().fetch("https://quant.internal/");

    // No explicit action needed to produce a tail event: the `tails = ["logtail"]` binding in
    // config.capnp makes every completed request on this worker automatically dispatch a tail
    // event into the logtail isolate (see tail-worker-test.wd-test for the same pattern).
    return new Response("ok " + res.status + " " + junk.length);
  },
};
