// The dispatcher (quarvo parent-worker analog): forwards requests to a loader-loaded quant.
// GET /stats returns stub.getStats() as JSON (or {"metering":false} on stock builds) so the
// bench can sanity-check that metering is actually active and counting.
export default {
  async fetch(req, env) {
    const stub = env.loader.get('quant', () => ({
      compatibilityDate: '2026-06-01',
      mainModule: 'q.js',
      modules: { 'q.js': env.quantSrc },
    }));
    const url = new URL(req.url);
    if (url.pathname === '/stats') {
      const body =
        typeof stub.getStats === 'function'
          ? JSON.stringify(stub.getStats())
          : JSON.stringify({ metering: false });
      return new Response(body, {
        headers: { 'content-type': 'application/json' },
      });
    }
    return stub.getEntrypoint().fetch(req);
  },
};
