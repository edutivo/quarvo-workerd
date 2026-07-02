// quarvo e2e: tail worker isolate (the "logtail" analog). Every dispatcher.js invocation
// produces a tail event here (via the `tails = ["logtail"]` binding in config.capnp), and this
// worker materializes per-event garbage in ITS OWN isolate, like the production logtail worker.
export default {
  tail(events) {
    const acc = [];
    for (const e of events) acc.push(JSON.stringify(e));
    return acc.length;
  },
};
