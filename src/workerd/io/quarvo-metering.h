// NOTE(quarvo): opt-in runtime metering for Worker-Loader children. Two meters per loader-key
// isolate — stolen time (run-delay: timer lag + cross-thread lock wait + busy-window resume
// estimate) and thread-CPU under the isolate lock — readable synchronously by the loader-holding
// parent via WorkerStub.getStats(). Opt-in via QUARVO_RUNTIME_METERING; a typo in the value is
// FATAL at boot (spec §6). When off, nothing here is instantiated and behavior is identical to
// stock workerd. Design: docs/superpowers/specs/2026-07-05-quarvo-runtime-metering-design.md
#pragma once

#include <kj/common.h>
#include <kj/refcount.h>
#include <kj/string.h>

#include <atomic>

namespace workerd::quarvo {

// ---------- process-wide gate ----------

// Pure parse of the QUARVO_RUNTIME_METERING value (kj::none = unset). on/1/true/yes => true;
// unset/""/off/0/false/no => false; anything else THROWS (boot-fatal, spec §6).
bool parseQuarvoMeteringEnv(kj::Maybe<kj::StringPtr> value);

// Reads the env var, applies parseQuarvoMeteringEnv (letting a bad value propagate as a fatal
// startup exception), and marks the calling thread as the event-loop thread. Called once from
// the Server constructor, which runs on the loop thread.
void initMeteringFromEnv();

// Cheap global check; false until initMeteringFromEnv() enables it.
bool meteringEnabled();

// True on the thread that called initMeteringFromEnv(). All steal/CPU accounting is loop-thread
// only (spec §2.2.2: reclaimer/inspector lock holds don't occupy the shared JS thread).
bool onLoopThread();

// ---------- clocks ----------

// CLOCK_MONOTONIC, ns. Valid to compare/subtract within the process.
uint64_t nowMonoNs();

// CLOCK_THREAD_CPUTIME_ID, ns. Returns 0 on platforms without it (Windows) — cpuMs then stays
// 0, documented in FEATURES.md; the fork ships Linux images.
uint64_t threadCpuNs();

// ---------- busy-chain estimator (spec §4.3) — pure, loop-thread-only state ----------

// A "busy chain" is a maximal run of JS slices with < epsilon gaps between them. Steal for
// non-timer resumes is estimated as half the foreign hold time in the current chain since the
// worker's own previous slice in it.
constexpr uint64_t CHAIN_GAP_EPSILON_NS = 200'000;  // 200 us, spec §4.3 (code constant, Q5)

struct ChainState {
  uint64_t chainId = 0;  // 0 = no chain yet; incremented on each reset
  uint64_t chainStartNs = 0;
  uint64_t lastSliceEndNs = 0;
  uint64_t chainHoldNs = 0;  // total hold ns of ALL slices in the current chain
};

// Per-meter bookmark: where this worker last stood in the chain.
struct MeterChainMark {
  uint64_t chainId = 0;
  uint64_t holdSnapshotNs = 0;  // ChainState::chainHoldNs right after this worker's last slice
};

// Slice begin: reset the chain if the idle gap reached epsilon.
void chainOnSliceBegin(ChainState& c, uint64_t nowNs);

// Half of the foreign hold accumulated in the current chain since this worker's last slice in
// it (whole chain if it hasn't run in this chain). Zero if there is no live chain context.
uint64_t resumeChargeNs(const ChainState& c, const MeterChainMark& m);

// Slice end: account this slice into the chain and bookmark the worker.
void chainOnSliceEnd(ChainState& c, MeterChainMark& m, uint64_t sliceBeginNs, uint64_t nowNs);

// The loop thread's chain state (plain thread_local; only ever touched on the loop thread).
ChainState& loopChainState();

// ---------- per-worker meter ----------

struct MeterSnapshot {
  uint64_t cpuNs;
  uint64_t timerLagNs;
  uint64_t lockWaitNs;
  uint64_t resumeDelayEstNs;
  uint64_t startDelayNs;
  uint32_t epoch;
};

// One per metered (Worker-Loader child) isolate instance. Writers: the loop thread (all
// counters). Readers: the loop thread via getStats(); atomics keep torn reads impossible if a
// future caller reads cross-thread. chainMark is loop-thread-only plain data.
class WorkerMeter final: public kj::AtomicRefcounted {
 public:
  explicit WorkerMeter(uint32_t epoch): epoch(epoch) {}

  void addCpu(uint64_t ns) const {
    cpuNs.fetch_add(ns, std::memory_order_relaxed);
  }
  void addTimerLag(uint64_t ns) const {
    timerLagNs.fetch_add(ns, std::memory_order_relaxed);
  }
  void addLockWait(uint64_t ns) const {
    lockWaitNs.fetch_add(ns, std::memory_order_relaxed);
  }
  void addResumeEst(uint64_t ns) const {
    resumeDelayEstNs.fetch_add(ns, std::memory_order_relaxed);
  }
  void addStartDelay(uint64_t ns) const {
    startDelayNs.fetch_add(ns, std::memory_order_relaxed);
  }

  MeterSnapshot snapshot() const {
    return {
      .cpuNs = cpuNs.load(std::memory_order_relaxed),
      .timerLagNs = timerLagNs.load(std::memory_order_relaxed),
      .lockWaitNs = lockWaitNs.load(std::memory_order_relaxed),
      .resumeDelayEstNs = resumeDelayEstNs.load(std::memory_order_relaxed),
      .startDelayNs = startDelayNs.load(std::memory_order_relaxed),
      .epoch = epoch,
    };
  }

  // Loop-thread only. Mutable for the same reason the counters are const-qualified atomics:
  // meters are shared as kj::Own<const WorkerMeter> through const observer methods.
  mutable MeterChainMark chainMark;

 private:
  mutable std::atomic<uint64_t> cpuNs{0};
  mutable std::atomic<uint64_t> timerLagNs{0};
  mutable std::atomic<uint64_t> lockWaitNs{0};
  mutable std::atomic<uint64_t> resumeDelayEstNs{0};
  mutable std::atomic<uint64_t> startDelayNs{0};
  const uint32_t epoch;
};

// ---------- per-request carrier state ----------

// Hangs off RequestObserver (via the quarvoRequestState() virtual added in observer.h).
// Written/read only on the loop thread.
struct RequestMeterState {
  uint64_t createdMonoNs = 0;  // stamped at observer construction ~= delivery
  bool firstRunSeen = false;   // first JS entry => startDelay, not a steal charge
  bool suppressNextResumeCharge = false;  // set by the timer path: lag already counted exactly
};

// Chain must already have had chainOnSliceBegin(nowNs) applied. Applies exactly one of:
// startDelay (first entry), nothing (suppressed timer entry), or the resume estimate.
// Split out from QuarvoLockTiming::locked() for unit testing.
void applyEntryCharges(
    const ChainState& c, const WorkerMeter& m, RequestMeterState& req, uint64_t nowNs);

}  // namespace workerd::quarvo
