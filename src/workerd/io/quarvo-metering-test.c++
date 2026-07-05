#include "quarvo-metering.h"

#include <kj/test.h>

namespace workerd::quarvo {
namespace {

KJ_TEST("parseQuarvoMeteringEnv: accepted values") {
  KJ_EXPECT(parseQuarvoMeteringEnv(kj::none) == false);
  KJ_EXPECT(parseQuarvoMeteringEnv("off"_kj) == false);
  KJ_EXPECT(parseQuarvoMeteringEnv("0"_kj) == false);
  KJ_EXPECT(parseQuarvoMeteringEnv("false"_kj) == false);
  KJ_EXPECT(parseQuarvoMeteringEnv("no"_kj) == false);
  KJ_EXPECT(parseQuarvoMeteringEnv(""_kj) == false);
  KJ_EXPECT(parseQuarvoMeteringEnv("on"_kj) == true);
  KJ_EXPECT(parseQuarvoMeteringEnv("ON"_kj) == true);
  KJ_EXPECT(parseQuarvoMeteringEnv("1"_kj) == true);
  KJ_EXPECT(parseQuarvoMeteringEnv("true"_kj) == true);
  KJ_EXPECT(parseQuarvoMeteringEnv("yes"_kj) == true);
}

KJ_TEST("parseQuarvoMeteringEnv: typo is fatal (throws)") {
  // Spec §6: unrecognized value must fail loudly at boot, unlike the fail-safe
  // QUARVO_GC_PRESSURE master switch. A typo'd flag would leave the consumer silently on its
  // statistical estimator while believing it has ground truth.
  KJ_EXPECT_THROW_MESSAGE("QUARVO_RUNTIME_METERING", parseQuarvoMeteringEnv("onn"_kj));
  KJ_EXPECT_THROW_MESSAGE("QUARVO_RUNTIME_METERING", parseQuarvoMeteringEnv("enable"_kj));
}

KJ_TEST("busy chain: gap >= epsilon starts a new chain") {
  ChainState c;
  chainOnSliceBegin(c, 1'000'000);
  KJ_EXPECT(c.chainId == 1);
  MeterChainMark dummy;
  chainOnSliceEnd(c, dummy, 1'000'000, 2'000'000);  // 1ms slice
  // Next slice begins within epsilon (200us) => same chain.
  chainOnSliceBegin(c, 2'000'000 + CHAIN_GAP_EPSILON_NS - 1);
  KJ_EXPECT(c.chainId == 1);
  chainOnSliceEnd(c, dummy, 2'000'000 + CHAIN_GAP_EPSILON_NS - 1, 3'000'000);
  // Gap of exactly epsilon => new chain.
  chainOnSliceBegin(c, 3'000'000 + CHAIN_GAP_EPSILON_NS);
  KJ_EXPECT(c.chainId == 2);
  KJ_EXPECT(c.chainHoldNs == 0);
}

KJ_TEST("resume charge: half of foreign hold since own last slice, per spec §4.3") {
  ChainState c;
  WorkerMeter meter(1);
  MeterChainMark foreignMark;

  // Foreign isolate runs a 100ms slice starting the chain.
  chainOnSliceBegin(c, 0);
  chainOnSliceEnd(c, foreignMark, 0, 100'000'000);

  // Metered worker W resumes 10us later (same chain): never ran in this chain
  // => foreign hold = full 100ms => charge = 50ms.
  chainOnSliceBegin(c, 100'010'000);
  KJ_EXPECT(resumeChargeNs(c, meter.chainMark) == 50'000'000);
  chainOnSliceEnd(c, meter.chainMark, 100'010'000, 100'020'000);  // W runs 10us

  // Foreign runs 40ms more in the same chain.
  chainOnSliceBegin(c, 100'030'000);
  chainOnSliceEnd(c, foreignMark, 100'030'000, 140'030'000);

  // W resumes again: foreign hold since W's own last slice = 40ms => charge 20ms.
  chainOnSliceBegin(c, 140'040'000);
  KJ_EXPECT(resumeChargeNs(c, meter.chainMark) == 20'000'000);

  // After the chain resets (idle gap), charge is zero.
  chainOnSliceEnd(c, meter.chainMark, 140'040'000, 140'050'000);
  chainOnSliceBegin(c, 200'000'000);  // >> epsilon after last end
  KJ_EXPECT(resumeChargeNs(c, meter.chainMark) == 0);
}

KJ_TEST("WorkerMeter: counters accumulate and snapshot") {
  WorkerMeter m(3);
  m.addCpu(1'000'000);
  m.addCpu(500'000);
  m.addTimerLag(2'000'000);
  m.addLockWait(10'000);
  m.addResumeEst(3'000'000);
  m.addStartDelay(4'000'000);
  auto s = m.snapshot();
  KJ_EXPECT(s.cpuNs == 1'500'000);
  KJ_EXPECT(s.timerLagNs == 2'000'000);
  KJ_EXPECT(s.lockWaitNs == 10'000);
  KJ_EXPECT(s.resumeDelayEstNs == 3'000'000);
  KJ_EXPECT(s.startDelayNs == 4'000'000);
  KJ_EXPECT(s.epoch == 3);
}

KJ_TEST("applyEntryCharges: first entry -> startDelay; suppressed -> neither; "
        "else -> resume estimate") {
  ChainState c;
  WorkerMeter m(1);
  RequestMeterState req;
  req.createdMonoNs = 1'000'000;

  // Foreign 10ms slice creates chain context.
  MeterChainMark foreignMark;
  chainOnSliceBegin(c, 0);
  chainOnSliceEnd(c, foreignMark, 0, 10'000'000);

  // First entry at t=12ms: startDelay = 11ms, no steal counters.
  applyEntryCharges(c, m, req, 12'000'000);
  KJ_EXPECT(m.snapshot().startDelayNs == 11'000'000);
  KJ_EXPECT(m.snapshot().resumeDelayEstNs == 0);
  chainOnSliceEnd(c, m.chainMark, 12'000'000, 12'100'000);

  // Foreign 20ms more.
  chainOnSliceBegin(c, 12'150'000);
  chainOnSliceEnd(c, foreignMark, 12'150'000, 32'150'000);

  // Timer-driven entry: suppression consumes the flag, no estimator charge.
  req.suppressNextResumeCharge = true;
  applyEntryCharges(c, m, req, 32'160'000);
  KJ_EXPECT(m.snapshot().resumeDelayEstNs == 0);
  KJ_EXPECT(req.suppressNextResumeCharge == false);
  chainOnSliceEnd(c, m.chainMark, 32'160'000, 32'170'000);

  // Foreign 30ms more, then a plain resume: charge = 15ms.
  chainOnSliceBegin(c, 32'180'000);
  chainOnSliceEnd(c, foreignMark, 32'180'000, 62'180'000);
  applyEntryCharges(c, m, req, 62'190'000);
  KJ_EXPECT(m.snapshot().resumeDelayEstNs == 15'000'000);
}

}  // namespace
}  // namespace workerd::quarvo
