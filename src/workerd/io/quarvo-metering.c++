#include "quarvo-metering.h"

#include <kj/debug.h>

#include <cstdlib>

#if defined(__linux__) || defined(__APPLE__)
#include <strings.h>  // strcasecmp
#include <time.h>
#endif

namespace workerd::quarvo {

namespace {
bool gEnabled = false;
thread_local bool tlIsLoopThread = false;

bool equalsIgnoreCase(kj::StringPtr a, const char* b) {
#if defined(__linux__) || defined(__APPLE__)
  return strcasecmp(a.cStr(), b) == 0;
#else
  return _stricmp(a.cStr(), b) == 0;
#endif
}
}  // namespace

bool parseQuarvoMeteringEnv(kj::Maybe<kj::StringPtr> value) {
  KJ_IF_SOME(v, value) {
    if (v.size() == 0) return false;
    for (const char* on: {"on", "1", "true", "yes"}) {
      if (equalsIgnoreCase(v, on)) return true;
    }
    for (const char* off: {"off", "0", "false", "no"}) {
      if (equalsIgnoreCase(v, off)) return false;
    }
    // Deliberately fatal (spec §6): a typo'd flag must not silently leave the consumer running
    // on its fallback estimator while believing it has runtime ground truth.
    KJ_FAIL_REQUIRE("QUARVO_RUNTIME_METERING has unrecognized value; expected one of "
                    "on/1/true/yes/off/0/false/no (case-insensitive). Refusing to boot.",
        v);
  }
  return false;
}

void initMeteringFromEnv() {
  kj::Maybe<kj::StringPtr> value;
  if (const char* raw = getenv("QUARVO_RUNTIME_METERING")) {
    value = kj::StringPtr(raw);
  }
  gEnabled = parseQuarvoMeteringEnv(value);
  tlIsLoopThread = true;
  if (gEnabled) {
    KJ_LOG(INFO, "quarvo runtime metering enabled (QUARVO_RUNTIME_METERING)");
  }
}

bool meteringEnabled() {
  return gEnabled;
}

bool onLoopThread() {
  return tlIsLoopThread;
}

uint64_t nowMonoNs() {
#if defined(__linux__) || defined(__APPLE__)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
#else
  return 0;
#endif
}

uint64_t threadCpuNs() {
#if defined(__linux__) || defined(__APPLE__)
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
#else
  return 0;
#endif
}

void chainOnSliceBegin(ChainState& c, uint64_t nowNs) {
  if (c.chainId == 0 || nowNs >= c.lastSliceEndNs + CHAIN_GAP_EPSILON_NS) {
    ++c.chainId;
    c.chainStartNs = nowNs;
    c.chainHoldNs = 0;
  }
}

uint64_t resumeChargeNs(const ChainState& c, const MeterChainMark& m) {
  if (c.chainId == 0) return 0;
  uint64_t foreign = (m.chainId == c.chainId) ? c.chainHoldNs - m.holdSnapshotNs : c.chainHoldNs;
  return foreign / 2;  // spec §4.3: unbiased-in-expectation half-charge (code constant, Q5)
}

void chainOnSliceEnd(ChainState& c, MeterChainMark& m, uint64_t sliceBeginNs, uint64_t nowNs) {
  c.chainHoldNs += nowNs - sliceBeginNs;
  c.lastSliceEndNs = nowNs;
  m.chainId = c.chainId;
  m.holdSnapshotNs = c.chainHoldNs;
}

ChainState& loopChainState() {
  static thread_local ChainState state;
  return state;
}

void applyEntryCharges(
    const ChainState& c, const WorkerMeter& m, RequestMeterState& req, uint64_t nowNs) {
  if (!req.firstRunSeen) {
    // Spec §2.2.4: pre-first-run time is startDelay, never stolenMs (and lockWait for this
    // entry is subsumed by it — callers must not also add lockWait for a first entry).
    req.firstRunSeen = true;
    m.addStartDelay(nowNs - req.createdMonoNs);
  } else if (req.suppressNextResumeCharge) {
    // Timer path already reported exact lateness via quarvoReportTimerLag (spec §4.2).
    req.suppressNextResumeCharge = false;
  } else {
    m.addResumeEst(resumeChargeNs(c, m.chainMark));
  }
}

}  // namespace workerd::quarvo
