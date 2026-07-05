#include "quarvo-banner.h"

#include <kj/string.h>

#include <cstdio>

namespace workerd::server {

kj::String composeQuarvoBanner(kj::StringPtr version,
    bool meteringOn,
    kj::StringPtr gcFragment,
    kj::ArrayPtr<const kj::String> v8Flags) {
  kj::String flagList =
      v8Flags.size() == 0 ? kj::str("(none)") : kj::strArray(v8Flags, ",");
  return kj::str("quarvo-workerd: ", version, " metering=", meteringOn ? "on" : "off", " ",
      gcFragment, " memorymb_enforcement=available v8flags=", flagList);
}

void printQuarvoBanner(
    bool meteringOn, kj::StringPtr gcFragment, kj::ArrayPtr<const kj::String> v8Flags) {
  auto line = composeQuarvoBanner(QUARVO_FORK_VERSION, meteringOn, gcFragment, v8Flags);
  fprintf(stderr, "%s\n", line.cStr());
  fflush(stderr);
}

}  // namespace workerd::server
