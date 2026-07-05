// NOTE(quarvo): the first concrete IsolateObserver::LockTiming in the tree. Installed by
// Server::makeWorkerImpl for ALL isolates when QUARVO_RUNTIME_METERING is on (static isolates
// carry no meter but still feed the busy chain — the dispatcher's own slices steal from quants
// too). See quarvo-metering.h for the accounting model and the design spec reference.
#pragma once

#include <workerd/io/observer.h>
#include <workerd/io/quarvo-metering.h>

namespace workerd::quarvo {

class QuarvoLockTiming final: public IsolateObserver::LockTiming {
 public:
  QuarvoLockTiming(
      kj::Maybe<kj::Own<const WorkerMeter>> meter, kj::Maybe<kj::Own<RequestObserver>> reqRef)
      : meter(kj::mv(meter)),
        reqRef(kj::mv(reqRef)),
        createdNs(nowMonoNs()) {}

  // Slice begin: chain maintenance, then exactly one of startDelay / suppressed / resume
  // estimate for request-bearing entries, plus lockWait for non-first entries.
  void locked() override;

  // Slice end: CPU delta into the meter; slice hold into the busy chain.
  void stop() override;

 private:
  kj::Maybe<kj::Own<const WorkerMeter>> meter;  // none => static isolate: chain-only
  kj::Maybe<kj::Own<RequestObserver>> reqRef;
  uint64_t createdNs;
  uint64_t sliceBeginNs = 0;
  uint64_t cpuBeginNs = 0;
  bool inSlice = false;
};

class QuarvoIsolateObserver final: public IsolateObserver {
 public:
  // meter is none for static (non-Worker-Loader) isolates: chain tracking only.
  explicit QuarvoIsolateObserver(kj::Maybe<kj::Own<const WorkerMeter>> meter)
      : meter(kj::mv(meter)) {}

  kj::Maybe<kj::Own<LockTiming>> tryCreateLockTiming(
      kj::OneOf<SpanParent, kj::Maybe<RequestObserver&>> parentOrRequest) const override;

  void quarvoReportTimerLag(uint64_t lagNs) const override {
    KJ_IF_SOME(m, meter) {
      m->addTimerLag(lagNs);
    }
  }

 private:
  kj::Maybe<kj::Own<const WorkerMeter>> meter;
};

}  // namespace workerd::quarvo
