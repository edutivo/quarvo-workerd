// The echo quant: one timer wakeup per request so every request exercises the metered
// resume path (timer lateness + slice brackets), not just a single synchronous slice.
export default {
  async fetch(req) {
    await new Promise((resolve) => setTimeout(resolve, 1));
    return new Response('ok');
  },
};
