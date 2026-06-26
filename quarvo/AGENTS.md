# AGENTS.md — `quarvo/` (fork layer)

Component context for the **quarvo fork layer** on top of upstream workerd. quarvo adds real
per-isolate resource-limit enforcement for Worker-Loader-loaded workers (starting with `memoryMB`)
plus the OCI build/release tooling. The enforcement C++ lives in the normal tree
(`src/workerd/server/server.c++`, `src/workerd/io/…`); this directory holds the fork's docs, the
Docker build, and ops kits.

## Where to look

| Topic | File |
|---|---|
| Fork overview + capability matrix + doc map | [README.md](README.md) |
| Per-capability behavior / semantics (source of truth) | [FEATURES.md](FEATURES.md) |
| `WorkerCode.limits` contract + pinning the artifact | [INTEGRATION.md](INTEGRATION.md) |
| Rebase seams, build/test, CI caching | [MAINTENANCE.md](MAINTENANCE.md) |
| Timeline of quarvo changes vs upstream (for users) | [CHANGELOG.md](CHANGELOG.md) |
| GHCR→ACR image mirror (git-excluded, local only) | `ops/` |

## Local conventions

- **Fork deltas use a `quarvo:` commit-subject prefix** and land on `quarvo-main` via **squash PRs**
  (never merge a whole feature branch into `quarvo-main`).
- **Releases** are git tags `v<upstream-workerd-version>-quarvo.<N>` (e.g. `v1.20260623.1-quarvo.3`),
  which trigger `quarvo-release` to build + publish the multi-arch image. See MAINTENANCE.md.
- Keep the upstream diff minimal: with no `limits` declared, a loaded isolate must stay
  byte-for-byte equivalent to stock workerd at the same tag.

## REQUIRED: keep `CHANGELOG.md` current

Any time you make a **quarvo-specific change** (a `quarvo:` fork delta — whether the code lives in
`quarvo/` or in `src/`) or **cut a release**, you MUST update [CHANGELOG.md](CHANGELOG.md):

1. Add a bullet under the top **`## [Unreleased]`** section (create it if missing), written **from
   the user's perspective, relative to upstream** — the *what / why*, not the implementation detail.
2. Categorize it: **Added** (new capability) · **Changed** (behavior change) · **Fixed** (bug) ·
   **Internal** (build / CI / docs — no runtime effect for users).
3. **On release**, rename `## [Unreleased]` → `## [<version>] — <YYYY-MM-DD>`, add its
   `**Upstream base:** workerd v<X>` + `**Image:**` lines, then open a fresh `## [Unreleased]`.
4. Newest-first; link the PR as `(#N)`; never restate upstream's own changelog — only quarvo deltas.

An out-of-date `CHANGELOG.md` is a release blocker: it is how quarvo-workerd consumers see what
changed between updates.
