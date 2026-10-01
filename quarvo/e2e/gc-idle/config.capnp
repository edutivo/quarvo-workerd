using Workerd = import "/workerd/workerd.capnp";

# Minimal idle config for the GC-pressure reclaimer crash regression (see run.sh): one static
# worker, no quants, no tails, so the reclaimer finds an IDLE isolate on its first tick.
const config :Workerd.Config = (
  services = [(name = "main", worker = (
    modules = [(name = "worker.js", esModule = embed "worker.js")],
    compatibilityDate = "2026-06-23",
  ))],
  sockets = [(name = "http", address = "127.0.0.1:18080", http = (), service = "main")],
);
