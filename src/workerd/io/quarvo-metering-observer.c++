#include "quarvo-metering-observer.h"

namespace workerd::quarvo {

void QuarvoLockTiming::locked() {
  // Off-loop lock holds (inspector thread, GC-pressure reclaimer) neither consume the shared
  // JS thread nor belong in cpuMs (spec §2.2.2). Their effect on victims IS captured: the
  // victim's next on-loop entry sees created->locked wait, recorded below as lockWait.
  if (!onLoopThread()) return;
  uint64_t now = nowMonoNs();
  auto& chain = loopChainState();
  chainOnSliceBegin(chain, now);
  KJ_IF_SOME(m, meter) {
    bool firstEntry = false;
    KJ_IF_SOME(ro, reqRef) {
      KJ_IF_SOME(st, ro->quarvoRequestState()) {
        firstEntry = !st.firstRunSeen;
        applyEntryCharges(chain, *m, st, now);
      }
    }
    // Lock wait (created -> locked): ~0 on the single thread except when another thread holds
    // this isolate's lock. First entries fold it into startDelay instead (spec §2.2.4).
    if (!firstEntry) {
      m->addLockWait(now - createdNs);
    }
  }
  sliceBeginNs = now;
  cpuBeginNs = threadCpuNs();
  inSlice = true;
}

void QuarvoLockTiming::stop() {
  if (!inSlice) return;
  inSlice = false;
  uint64_t now = nowMonoNs();
  auto& chain = loopChainState();
  KJ_IF_SOME(m, meter) {
    m->addCpu(threadCpuNs() - cpuBeginNs);
    chainOnSliceEnd(chain, m->chainMark, sliceBeginNs, now);
  } else {
    MeterChainMark unused;
    chainOnSliceEnd(chain, unused, sliceBeginNs, now);
  }
}

kj::Maybe<kj::Own<IsolateObserver::LockTiming>> QuarvoIsolateObserver::tryCreateLockTiming(
    kj::OneOf<SpanParent, kj::Maybe<RequestObserver&>> parentOrRequest) const {
  kj::Maybe<kj::Own<RequestObserver>> reqRef;
  KJ_SWITCH_ONEOF(parentOrRequest) {
    KJ_CASE_ONEOF(span, SpanParent) {}
    KJ_CASE_ONEOF(req, kj::Maybe<RequestObserver&>) {
      KJ_IF_SOME(r, req) {
        reqRef = kj::addRef(r);
      }
    }
  }
  kj::Maybe<kj::Own<const WorkerMeter>> meterRef;
  KJ_IF_SOME(m, meter) {
    meterRef = kj::atomicAddRef(*m);
  }
  return kj::Own<LockTiming>(kj::heap<QuarvoLockTiming>(kj::mv(meterRef), kj::mv(reqRef)));
}

}  // namespace workerd::quarvo
