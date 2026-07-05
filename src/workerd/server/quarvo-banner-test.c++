#include "quarvo-banner.h"

#include <kj/test.h>

namespace workerd::server {
namespace {

KJ_TEST("composeQuarvoBanner: all features off") {
  auto line = composeQuarvoBanner("1.20260623.1-quarvo.5"_kj, false, "gc_pressure=off"_kj,
      kj::ArrayPtr<const kj::String>());
  KJ_EXPECT(line ==
          "quarvo-workerd: 1.20260623.1-quarvo.5 metering=off gc_pressure=off "
          "memorymb_enforcement=available v8flags=(none)"_kj,
      line);
}

KJ_TEST("composeQuarvoBanner: everything on") {
  auto flags = kj::heapArray<kj::String>(2);
  flags[0] = kj::str("--enforce-global-heap-limit");
  flags[1] = kj::str("--maximum-global-heap-limit-factor=2");
  auto line = composeQuarvoBanner("1.20260623.1-quarvo.5"_kj, true,
      "gc_pressure=on gc_threshold_mb=96 gc_threshold_pct=100 gc_min_interval_ms=500"_kj,
      flags.asPtr());
  KJ_EXPECT(line ==
          "quarvo-workerd: 1.20260623.1-quarvo.5 metering=on gc_pressure=on gc_threshold_mb=96 "
          "gc_threshold_pct=100 gc_min_interval_ms=500 memorymb_enforcement=available "
          "v8flags=--enforce-global-heap-limit,--maximum-global-heap-limit-factor=2"_kj,
      line);
}

KJ_TEST("QUARVO_FORK_VERSION matches the release naming scheme") {
  // Guards against forgetting the release-checklist bump leaving a placeholder.
  KJ_EXPECT(QUARVO_FORK_VERSION.contains("-quarvo."));
}

}  // namespace
}  // namespace workerd::server
