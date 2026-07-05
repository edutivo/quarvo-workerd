# quarvo e2e: runtime-metering overhead bench. A static dispatcher (quarvo's parent-worker
# shape) forwards every request to a worker-loader child (quant analog) and exposes /stats
# reading stub.getStats(). Driven by quarvo/e2e/meter-overhead/run.sh, which compares req/s
# with QUARVO_RUNTIME_METERING off vs on and asserts the boot banner contract.
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "dispatcher",
      worker = (
        modules = [(name = "dispatcher.js", esModule = embed "dispatcher.js")],
        compatibilityDate = "2026-06-01",
        compatibilityFlags = ["experimental"],
        bindings = [
          (name = "loader", workerLoader = ()),
          (name = "quantSrc", text = embed "quant.js"),
        ],
      )),
  ],
  sockets = [(name = "http", address = "*:8080", http = (), service = "dispatcher")],
);
