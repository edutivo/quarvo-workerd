// NOTE(quarvo): boot config banner (spec §6.1, amendment 2026-07-05). One stderr line with a
// stable "quarvo-workerd: " prefix stating the fork version and the RESOLVED state of every
// quarvo feature, printed unconditionally at server startup — the single documented exception
// to "default off = byte-identical to stock". "Nothing enabled" being visible is the point: a
// pod log answers "which fork build, what's actually on?" without exec'ing into the container.
// Format is an ops contract documented in quarvo/FEATURES.md; only the prefix is guaranteed.
#pragma once

#include <kj/common.h>
#include <kj/string.h>

namespace workerd::server {

// Bumped as part of the release checklist (quarvo/MAINTENANCE.md) — a stale value here is a
// release blocker on par with the CHANGELOG rule. The unit test pins the naming scheme.
constexpr kj::StringPtr QUARVO_FORK_VERSION = "1.20260623.1-quarvo.5"_kj;

// Pure composition, unit-tested. gcFragment is the GC-pressure reclaimer's self-description
// ("gc_pressure=off" / "gc_pressure=on ..." / "gc_pressure=inert ...").
kj::String composeQuarvoBanner(kj::StringPtr version,
    bool meteringOn,
    kj::StringPtr gcFragment,
    kj::ArrayPtr<const kj::String> v8Flags);

// Composes with QUARVO_FORK_VERSION and prints to stderr, bypassing KJ_LOG (which filters INFO
// at default verbosity — the banner must always print).
void printQuarvoBanner(
    bool meteringOn, kj::StringPtr gcFragment, kj::ArrayPtr<const kj::String> v8Flags);

}  // namespace workerd::server
