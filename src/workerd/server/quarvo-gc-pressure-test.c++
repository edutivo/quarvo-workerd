#include "quarvo-gc-pressure.h"

#include <kj/test.h>

namespace workerd::server {
namespace {

KJ_TEST("quarvo gc-pressure: env parsing — disabled unless explicitly enabled") {
  KJ_EXPECT(parseQuarvoGcPressureEnv(kj::none, kj::none, kj::none, kj::none) == kj::none);
  KJ_EXPECT(parseQuarvoGcPressureEnv("off"_kj, kj::none, kj::none, kj::none) == kj::none);
  KJ_EXPECT(parseQuarvoGcPressureEnv("0"_kj, kj::none, kj::none, kj::none) == kj::none);
  KJ_EXPECT(parseQuarvoGcPressureEnv("banana"_kj, kj::none, kj::none, kj::none) == kj::none);
  for (auto v: {"on"_kj, "1"_kj, "true"_kj, "yes"_kj, "ON"_kj, "TRUE"_kj}) {
    KJ_EXPECT(parseQuarvoGcPressureEnv(v, kj::none, kj::none, kj::none) != kj::none, v);
  }
}

KJ_TEST("quarvo gc-pressure: env parsing — values and defaults") {
  auto c = KJ_ASSERT_NONNULL(parseQuarvoGcPressureEnv("on"_kj, kj::none, kj::none, kj::none));
  KJ_EXPECT(c.thresholdPct == 60);
  KJ_EXPECT(c.thresholdBytes == kj::none);
  KJ_EXPECT(c.minIntervalMs == 30000);

  auto c2 = KJ_ASSERT_NONNULL(parseQuarvoGcPressureEnv("on"_kj, "45"_kj, "192"_kj, "5000"_kj));
  KJ_EXPECT(c2.thresholdPct == 45);
  KJ_EXPECT(KJ_ASSERT_NONNULL(c2.thresholdBytes) == 192ull << 20);
  KJ_EXPECT(c2.minIntervalMs == 5000);

  // Invalid values fall back to defaults (fail-safe), not to disabled.
  auto c3 = KJ_ASSERT_NONNULL(parseQuarvoGcPressureEnv("on"_kj, "0"_kj, "zap"_kj, "-3"_kj));
  KJ_EXPECT(c3.thresholdPct == 60);
  KJ_EXPECT(c3.thresholdBytes == kj::none);
  KJ_EXPECT(c3.minIntervalMs == 30000);
  auto c4 = KJ_ASSERT_NONNULL(parseQuarvoGcPressureEnv("on"_kj, "101"_kj, kj::none, kj::none));
  KJ_EXPECT(c4.thresholdPct == 60);

  // Rejection branches: implausibly large THRESHOLD_MB (> 1 PiB after <<20) and
  // MIN_INTERVAL_MS exceeding 24h both fall back to defaults, not to disabled.
  auto c5 = KJ_ASSERT_NONNULL(
      parseQuarvoGcPressureEnv("on"_kj, kj::none, "1073741825"_kj, "86400001"_kj));
  KJ_EXPECT(c5.thresholdBytes == kj::none);
  KJ_EXPECT(c5.minIntervalMs == 30000);
}

KJ_TEST("quarvo gc-pressure: cgroup value parsing") {
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupValue("268435456\n"_kj)) == 268435456ull);
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupValue("0"_kj)) == 0);
  KJ_EXPECT(parseCgroupValue("max\n"_kj) == kj::none);
  KJ_EXPECT(parseCgroupValue(""_kj) == kj::none);
  KJ_EXPECT(parseCgroupValue("bogus\n"_kj) == kj::none);
}

KJ_TEST("quarvo gc-pressure: /proc/self/cgroup v2 path extraction") {
  // Pure cgroup v2 (container with private namespace).
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupV2Path("0::/\n"_kj)) == "/");
  // Bare host leaf cgroup.
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupV2Path(
                "0::/user.slice/user-1000.slice/session-1.scope\n"_kj)) ==
      "/user.slice/user-1000.slice/session-1.scope");
  // Hybrid v1+v2: only the "0::" line counts.
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupV2Path(
                "12:pids:/init.scope\n1:name=systemd:/init.scope\n0::/foo\n"_kj)) == "/foo");
  // No trailing newline (defensive; kernel output normally ends with one).
  KJ_EXPECT(KJ_ASSERT_NONNULL(parseCgroupV2Path("0::/foo"_kj)) == "/foo");
  // No v2 entry at all.
  KJ_EXPECT(parseCgroupV2Path("12:pids:/init.scope\n"_kj) == kj::none);
}

KJ_TEST("quarvo gc-pressure: effective threshold — lower wins") {
  QuarvoGcPressureConfig c{.thresholdPct = 50, .thresholdBytes = kj::none, .minIntervalMs = 1};

  // pct of a 256 MiB limit = 128 MiB.
  KJ_EXPECT(KJ_ASSERT_NONNULL(effectiveThresholdBytes(c, 256ull << 20)) == 128ull << 20);
  // Unlimited cgroup, no MB set => cannot trigger.
  KJ_EXPECT(effectiveThresholdBytes(c, kj::none) == kj::none);

  // MB set lower than pct => MB wins.
  c.thresholdBytes = 64ull << 20;
  KJ_EXPECT(KJ_ASSERT_NONNULL(effectiveThresholdBytes(c, 256ull << 20)) == 64ull << 20);
  // MB set higher than pct => pct wins.
  c.thresholdBytes = 200ull << 20;
  KJ_EXPECT(KJ_ASSERT_NONNULL(effectiveThresholdBytes(c, 256ull << 20)) == 128ull << 20);
  // Unlimited cgroup, MB set => MB.
  KJ_EXPECT(KJ_ASSERT_NONNULL(effectiveThresholdBytes(c, kj::none)) == 200ull << 20);

  // Multiply-before-divide precision: 50% of 199 bytes must be exactly 99 (199*50/100), not 50
  // (199/100*50, the div-first regression).
  c.thresholdBytes = kj::none;
  KJ_EXPECT(KJ_ASSERT_NONNULL(effectiveThresholdBytes(c, uint64_t(199))) == 99);
}

}  // namespace
}  // namespace workerd::server
