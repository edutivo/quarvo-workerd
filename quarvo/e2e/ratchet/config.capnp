# quarvo e2e: reproduces the per-request cppgc memory ratchet (upstream workerd#6824) with all
# three production isolate classes: a static dispatcher, a static tail worker (logtail analog),
# and a dynamic worker-loader isolate (quant analog). Driven by quarvo/e2e/ratchet/run.sh.
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "dispatcher",
      worker = (
        modules = [(name = "dispatcher.js", esModule = embed "dispatcher.js")],
        compatibilityDate = "2026-06-01",
        compatibilityFlags = ["experimental"],
        bindings = [(name = "loader", workerLoader = ())],
        tails = ["logtail"],
      )),
    ( name = "logtail",
      worker = (
        modules = [(name = "logtail.js", esModule = embed "logtail.js")],
        compatibilityDate = "2026-06-01",
      )),
  ],
  sockets = [(name = "http", address = "*:8080", http = (), service = "dispatcher")],
);
