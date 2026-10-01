// NOTE(quarvo): cgroup-pressure-driven unified GC. Under memory pressure, a background thread
// runs MemoryPressureNotification(kCritical) on idle isolates while holding the isolate lock,
// sweeping cppgc garbage and returning pages to the OS. Opt-in via QUARVO_GC_PRESSURE; when off
// this module is never instantiated and behavior is identical to stock workerd.
// Design: docs/superpowers/specs/2026-07-02-quarvo-gc-pressure-design.md
#pragma once

#include <workerd/io/worker.h>

#include <kj/common.h>
#include <kj/mutex.h>
#include <kj/string.h>
#include <kj/thread.h>
#include <kj/time.h>
#include <kj/vector.h>

namespace workerd::server {

struct QuarvoGcPressureConfig {
  // Percent of cgroup memory.max at which to trigger (inert if memory.max is unlimited).
  uint32_t thresholdPct = 60;
  // Absolute trigger in bytes (from QUARVO_GC_PRESSURE_THRESHOLD_MB). When both this and the
  // pct threshold resolve, the LOWER byte value wins.
  kj::Maybe<uint64_t> thresholdBytes;
  // Minimum time between reclaim rounds.
  uint64_t minIntervalMs = 30000;
};

// Pure parsing/decision helpers, split out for unit testing (quarvo-gc-pressure-test.c++).

// Parses the four env values (pass results of getenv(); kj::none = unset). Returns kj::none
// unless `enabled` is one of on/1/true/yes (case-insensitive). Invalid numeric values fall back
// to defaults with a warning (fail-safe).
kj::Maybe<QuarvoGcPressureConfig> parseQuarvoGcPressureEnv(kj::Maybe<kj::StringPtr> enabled,
    kj::Maybe<kj::StringPtr> thresholdPct,
    kj::Maybe<kj::StringPtr> thresholdMb,
    kj::Maybe<kj::StringPtr> minIntervalMs);

// Parses the content of a cgroup v2 numeric file ("123456\n" or "max\n"). "max", empty, or
// malformed => kj::none.
kj::Maybe<uint64_t> parseCgroupValue(kj::StringPtr content);

// Extracts the cgroup v2 relative path from /proc/self/cgroup content (the "0::<path>" line).
kj::Maybe<kj::String> parseCgroupV2Path(kj::StringPtr procSelfCgroup);

// Effective trigger threshold in bytes: min(pct of memoryMax, thresholdBytes), where either
// side may be absent. kj::none => the feature cannot trigger (misconfiguration).
kj::Maybe<uint64_t> effectiveThresholdBytes(
    const QuarvoGcPressureConfig& config, kj::Maybe<uint64_t> memoryMax);

// Registry of live isolates + the background reclaim thread. One instance per Server, created
// only when QUARVO_GC_PRESSURE is enabled.
class QuarvoGcPressureReclaimer final {
 public:
  // Reads the QUARVO_GC_PRESSURE* env vars; returns kj::none when the feature is off.
  static kj::Maybe<kj::Own<QuarvoGcPressureReclaimer>> tryCreateFromEnv();

  explicit QuarvoGcPressureReclaimer(QuarvoGcPressureConfig config);
  ~QuarvoGcPressureReclaimer() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(QuarvoGcPressureReclaimer);

  // Thread-safe; called from Server::makeWorkerImpl for every isolate (static and dynamic).
  // Dead refs are pruned lazily by the reclaim thread — no deregistration call exists, which
  // eliminates teardown-ordering use-after-free by construction.
  //
  // The FIRST call also starts the reclaim thread (never the constructor). The caller must hold
  // a live isolate, which proves V8 platform init has run — see the thread-start note in
  // threadSlot below for why that ordering is load-bearing.
  void registerIsolate(kj::Own<const Worker::Isolate::WeakIsolateRef> ref);

  // True once the reclaim thread has been started (false while idle-before-first-isolate, and
  // forever when inert). Exposed for the "no thread before V8 init" regression test.
  bool threadStarted() const;

  // Self-description for the boot config banner (quarvo-banner.h): "gc_pressure=on|inert" plus
  // the EFFECTIVE resolved numbers (threshold after min(MB, PCT×memory.max), cgroup-derived
  // when PCT applies). "inert" = enabled but no cgroup v2 visible (the reclaimer cannot act).
  kj::String bannerFragment() const;

 private:
  const QuarvoGcPressureConfig config;
  kj::Maybe<kj::String> cgroupDir;  // resolved once in the ctor; none => feature inert
  kj::MutexGuarded<kj::Vector<kj::Own<const Worker::Isolate::WeakIsolateRef>>> registry;
  kj::MutexGuarded<bool> stopRequested{false};
  kj::Maybe<kj::TimePoint> lastRound;
  bool warnedNoThreshold = false;
  // Consecutive memory.current read failures (reclaim-thread-only); warns once at 10.
  uint currentReadFailures = 0;
  struct ThreadSlot {
    bool attempted = false;
    kj::Maybe<kj::Thread> thread;
  };
  // Started lazily by the first registerIsolate(), NOT in the constructor. The Server (and so
  // this object) is constructed BEFORE the V8 platform exists, and V8's default platform
  // allocates the x86 memory-protection key (pkey_alloc) that guards its code-pointer /
  // JS-dispatch tables during platform init. A thread inherits PKRU from its creator at clone
  // time, so a reclaim thread created earlier keeps the kernel default PKRU (access to every
  // non-zero pkey DISABLED) and SIGSEGVs (SEGV_PKUERR) the first time its forced GC touches
  // those tables. Threads created after V8 init inherit the key's access. Invisible on CPUs/VMs
  // without PKU (arm64, many cloud VMs), which is why it only crashed on some hosts.
  //
  // MUST be the last member: kj::Thread's destructor joins (runs first, before other members
  // die), so every field threadMain() touches must outlive it.
  kj::MutexGuarded<ThreadSlot> threadSlot;

  // Idempotent. Logs the "inert" warning (no cgroup v2) instead of starting a thread.
  void startThreadOnce();
  void threadMain();
  void tick();
  void runRound(uint64_t usageBytes, uint64_t thresholdBytes);
  kj::Maybe<uint64_t> readCgroupFile(kj::StringPtr fileName) const;
};

}  // namespace workerd::server
