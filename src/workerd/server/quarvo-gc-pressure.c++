#include "quarvo-gc-pressure.h"

#include <cstdlib>
#include <strings.h>  // strcasecmp

#include <fcntl.h>
#include <unistd.h>

#include <kj/debug.h>
#include <kj/exception.h>

namespace workerd::server {

kj::Maybe<QuarvoGcPressureConfig> parseQuarvoGcPressureEnv(kj::Maybe<kj::StringPtr> enabled,
    kj::Maybe<kj::StringPtr> thresholdPct,
    kj::Maybe<kj::StringPtr> thresholdMb,
    kj::Maybe<kj::StringPtr> minIntervalMs) {
  bool on = false;
  KJ_IF_SOME(e, enabled) {
    for (auto candidate: {"on", "1", "true", "yes"}) {
      if (strcasecmp(e.cStr(), candidate) == 0) on = true;
    }
  }
  if (!on) return kj::none;

  QuarvoGcPressureConfig config;
  KJ_IF_SOME(p, thresholdPct) {
    KJ_IF_SOME(v, p.tryParseAs<uint32_t>()) {
      if (v >= 1 && v <= 100) {
        config.thresholdPct = v;
      } else {
        KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_THRESHOLD_PCT out of range [1,100]; using default", p);
      }
    } else {
      KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_THRESHOLD_PCT not a number; using default", p);
    }
  }
  KJ_IF_SOME(m, thresholdMb) {
    KJ_IF_SOME(v, m.tryParseAs<uint64_t>()) {
      if (v > (1ull << 30)) {
        // > 1 PiB after the <<20 below — nonsensical, and larger values would overflow.
        KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_THRESHOLD_MB implausibly large; ignoring", m);
      } else if (v > 0) {
        config.thresholdBytes = v << 20;
      } else {
        KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_THRESHOLD_MB must be > 0; ignoring", m);
      }
    } else {
      KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_THRESHOLD_MB not a number; ignoring", m);
    }
  }
  KJ_IF_SOME(i, minIntervalMs) {
    KJ_IF_SOME(v, i.tryParseAs<uint64_t>()) {
      if (v > 86'400'000) {  // > 24h is a typo, not a tuning choice
        KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_MIN_INTERVAL_MS exceeds 24h; using default", i);
      } else {
        config.minIntervalMs = v;
      }
    } else {
      KJ_LOG(WARNING, "QUARVO_GC_PRESSURE_MIN_INTERVAL_MS not a number; using default", i);
    }
  }
  return config;
}

kj::Maybe<uint64_t> parseCgroupValue(kj::StringPtr content) {
  // Trim a single trailing newline. NOTE: do NOT build a kj::StringPtr over a sub-range —
  // StringPtr requires NUL termination at [size] (debug-asserts otherwise); copy instead.
  size_t len = content.size();
  if (len > 0 && content[len - 1] == '\n') len--;
  kj::String trimmed = kj::str(kj::ArrayPtr<const char>(content.begin(), len));
  if (trimmed == "max" || trimmed.size() == 0) return kj::none;
  return trimmed.tryParseAs<uint64_t>();
}

kj::Maybe<kj::String> parseCgroupV2Path(kj::StringPtr procSelfCgroup) {
  // Work with ArrayPtr<const char> line slices (StringPtr sub-ranges would violate its
  // NUL-termination invariant).
  kj::ArrayPtr<const char> rest = procSelfCgroup.asArray();
  while (rest.size() > 0) {
    kj::ArrayPtr<const char> line = rest;
    for (size_t i = 0; i < rest.size(); i++) {
      if (rest[i] == '\n') {
        line = rest.first(i);
        break;
      }
    }
    rest = rest.size() > line.size() ? rest.slice(line.size() + 1, rest.size())
                                     : kj::ArrayPtr<const char>();
    if (line.size() >= 3 && line[0] == '0' && line[1] == ':' && line[2] == ':') {
      return kj::str(line.slice(3, line.size()));
    }
  }
  return kj::none;
}

kj::Maybe<uint64_t> effectiveThresholdBytes(
    const QuarvoGcPressureConfig& config, kj::Maybe<uint64_t> memoryMax) {
  kj::Maybe<uint64_t> result = config.thresholdBytes;
  KJ_IF_SOME(max, memoryMax) {
    // Multiply before dividing: max/100 truncates first and can undercount by a few dozen KiB
    // on typical cgroup limits (e.g. 256 MiB @ 50% must be exactly 128 MiB, not 128 MiB - 28B).
    // uint64 overflow is not a concern: memory.max would need to exceed ~184 million TiB.
    uint64_t pctBytes = max * config.thresholdPct / 100;
    KJ_IF_SOME(existing, result) {
      result = kj::min(existing, pctBytes);
    } else {
      result = pctBytes;
    }
  }
  return result;
}

// ======================================================================================
// QuarvoGcPressureReclaimer

namespace {

kj::Maybe<kj::String> tryReadSmallFile(kj::StringPtr path) {
  // Loop-read to EOF: /proc/self/cgroup can exceed a single small buffer on hybrid v1+v2
  // hosts, and truncating it could cut off the "0::" line we need.
  int fd = open(path.cStr(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return kj::none;
  KJ_DEFER(close(fd));
  kj::Vector<char> data;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) return kj::none;
    if (n == 0) break;
    data.addAll(kj::ArrayPtr<const char>(buf, n));
  }
  return kj::str(data.asPtr());
}

kj::Maybe<kj::StringPtr> getEnvMaybe(kj::StringPtr name) {
  const char* value = getenv(name.cStr());
  if (value == nullptr) return kj::none;
  return kj::StringPtr(value);
}

constexpr auto TICK = 1 * kj::SECONDS;

}  // namespace

kj::Maybe<kj::Own<QuarvoGcPressureReclaimer>> QuarvoGcPressureReclaimer::tryCreateFromEnv() {
  return parseQuarvoGcPressureEnv(getEnvMaybe("QUARVO_GC_PRESSURE"),
      getEnvMaybe("QUARVO_GC_PRESSURE_THRESHOLD_PCT"),
      getEnvMaybe("QUARVO_GC_PRESSURE_THRESHOLD_MB"),
      getEnvMaybe("QUARVO_GC_PRESSURE_MIN_INTERVAL_MS"))
      .map([](QuarvoGcPressureConfig config) {
    return kj::heap<QuarvoGcPressureReclaimer>(config);
  });
}

QuarvoGcPressureReclaimer::QuarvoGcPressureReclaimer(QuarvoGcPressureConfig configParam)
    : config(configParam),
      cgroupDir([]() -> kj::Maybe<kj::String> {
        KJ_IF_SOME(content, tryReadSmallFile("/proc/self/cgroup")) {
          KJ_IF_SOME(path, parseCgroupV2Path(content)) {
            return kj::str("/sys/fs/cgroup", path == "/" ? ""_kj : path.asPtr());
          }
        }
        return kj::none;
      }()),
      thread([this]() { threadMain(); }) {}

QuarvoGcPressureReclaimer::~QuarvoGcPressureReclaimer() noexcept(false) {
  // Body runs before member destructors; `thread`'s destructor (first, it is the last member)
  // then joins. A GC in progress delays shutdown by at most one pause.
  *stopRequested.lockExclusive() = true;
}

void QuarvoGcPressureReclaimer::registerIsolate(
    kj::Own<const Worker::Isolate::WeakIsolateRef> ref) {
  // Inert (no cgroup v2): the reclaim thread has exited and nothing would ever prune the
  // registry, so don't grow it.
  if (cgroupDir == kj::none) return;
  registry.lockExclusive()->add(kj::mv(ref));
}

void QuarvoGcPressureReclaimer::threadMain() {
  // Startup logging lives here (not in the ctor body) so the ctor cannot throw after the thread
  // has started, and so all logging happens on the reclaim thread by construction.
  if (cgroupDir == kj::none) {
    KJ_LOG(WARNING,
        "QUARVO_GC_PRESSURE is on but no cgroup v2 hierarchy was found "
        "(/proc/self/cgroup has no 0:: entry); the feature is inert");
    return;
  }
  KJ_IF_SOME(dir, cgroupDir) {
    KJ_LOG(INFO, "quarvo GC-pressure reclaimer enabled", dir, config.thresholdPct,
        config.thresholdBytes.orDefault(0) >> 20, config.minIntervalMs);
  }
  while (true) {
    // Interruptible sleep: returns early (true) when the destructor sets the flag.
    bool stop = stopRequested.when([](const bool& s) { return s; },
        [](const bool& s) { return s; }, TICK);
    if (stop) return;
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() { tick(); })) {
      KJ_LOG(ERROR, "quarvo GC-pressure tick threw; continuing", exception);
    }
  }
}

kj::Maybe<uint64_t> QuarvoGcPressureReclaimer::readCgroupFile(kj::StringPtr fileName) const {
  auto& dir = KJ_ASSERT_NONNULL(cgroupDir);
  KJ_IF_SOME(content, tryReadSmallFile(kj::str(dir, "/", fileName))) {
    return parseCgroupValue(content);
  }
  return kj::none;
}

kj::String QuarvoGcPressureReclaimer::bannerFragment() const {
  // Boot-time only; reads memory.max once, same as a tick would. `unresolved` means the
  // threshold cannot be computed yet (e.g. memory.max is "max" and no absolute MB configured)
  // — the reclaimer will warn separately when it cannot trigger.
  if (cgroupDir == kj::none) {
    return kj::str("gc_pressure=inert gc_threshold_pct=", config.thresholdPct,
        " gc_min_interval_ms=", config.minIntervalMs);
  }
  kj::String mb = kj::str("unresolved");
  KJ_IF_SOME(t, effectiveThresholdBytes(config, readCgroupFile("memory.max"))) {
    mb = kj::str(t / (1024 * 1024));
  }
  return kj::str("gc_pressure=on gc_threshold_mb=", mb, " gc_threshold_pct=",
      config.thresholdPct, " gc_min_interval_ms=", config.minIntervalMs);
}

void QuarvoGcPressureReclaimer::tick() {
  uint64_t usage;
  KJ_IF_SOME(u, readCgroupFile("memory.current")) {
    usage = u;
    currentReadFailures = 0;
  } else {
    // Transient read failure (or a persistently wrong path); try again next tick, but warn once
    // if it keeps failing so a misresolved cgroup path is diagnosable rather than silent.
    if (++currentReadFailures == 10) {
      auto& dir = KJ_ASSERT_NONNULL(cgroupDir);
      KJ_LOG(WARNING,
          "QUARVO_GC_PRESSURE: cannot read <dir>/memory.current; if this container uses the host "
          "cgroup namespace the resolved path may not exist in the container mount", dir);
    }
    return;
  }

  auto maybeThreshold = effectiveThresholdBytes(config, readCgroupFile("memory.max"));
  uint64_t threshold;
  KJ_IF_SOME(t, maybeThreshold) {
    threshold = t;
  } else {
    if (!warnedNoThreshold) {
      warnedNoThreshold = true;
      KJ_LOG(WARNING,
          "QUARVO_GC_PRESSURE: cgroup memory.max is unlimited and no "
          "QUARVO_GC_PRESSURE_THRESHOLD_MB is set; the feature cannot trigger");
    }
    return;
  }
  warnedNoThreshold = false;  // re-warn if it becomes unresolvable again later

  if (usage < threshold) return;
  KJ_IF_SOME(last, lastRound) {
    auto elapsed = kj::systemPreciseMonotonicClock().now() - last;
    if (elapsed < config.minIntervalMs * kj::MILLISECONDS) return;
  }

  runRound(usage, threshold);
  lastRound = kj::systemPreciseMonotonicClock().now();
}

void QuarvoGcPressureReclaimer::runRound(uint64_t usageBytes, uint64_t thresholdBytes) {
  // Snapshot strong refs under the mutex (pruning dead entries), then GC OUTSIDE the mutex so
  // registration from other threads never blocks behind a GC pause.
  kj::Vector<kj::Own<const Worker::Isolate>> live;
  {
    auto locked = registry.lockExclusive();
    kj::Vector<kj::Own<const Worker::Isolate::WeakIsolateRef>> stillAlive;
    for (auto& weak: *locked) {
      KJ_IF_SOME(strong, weak->tryAddStrongRef()) {
        live.add(kj::mv(strong));
        stillAlive.add(kj::mv(weak));
      }
    }
    *locked = kj::mv(stillAlive);
  }

  uint reclaimed = 0, skippedBusy = 0;
  uint64_t after = usageBytes;
  for (auto& isolate: live) {
    if (*stopRequested.lockShared()) break;  // shutting down: at most one in-flight GC pause
    if (isolate->getCurrentLoad() > 0) {
      // A request holds or awaits this isolate's lock; skip it (next round will retry). A busy
      // isolate is executing JS, where V8's own allocation-driven GCs still run. (racy by
      // design: an isolate can become busy after this check; the GC then briefly queues behind
      // that request's lock — accepted, see design spec)
      skippedBusy++;
      continue;
    }
    isolate->memoryPressureReclaim();
    reclaimed++;
    KJ_IF_SOME(u, readCgroupFile("memory.current")) {
      after = u;
      if (u < thresholdBytes) break;  // pressure relieved; stop early
    }
  }

  KJ_LOG(INFO, "quarvo GC-pressure reclaim round", usageBytes >> 20, after >> 20,
      thresholdBytes >> 20, reclaimed, skippedBusy, live.size());
}

}  // namespace workerd::server
