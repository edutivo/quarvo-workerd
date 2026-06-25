# Handoff brief: make quarvo-workerd CI builds fast (Bazel caching)

> Hand this whole file to the new session as its kickoff prompt. It is self-contained.

## Mission

The multi-arch release build (`.github/workflows/quarvo-release.yml`) currently does a **cold
~3 h Bazel/V8 compile per architecture** on every run. Make subsequent builds **fast** (minutes when
only the enforcement C++ changed) by persisting a **Bazel action cache** across CI runs. Verify the
win empirically. Public repo → free runners (incl. native arm64), so no paid infra is required for
the first, biggest win.

## Current state (already done — don't redo)

- Fork: `edutivo/quarvo-workerd` (of `cloudflare/workerd`). Working branch `quarvo/limit-enforcement`
  (PR #1 → `quarvo-main`). Base = upstream tag `v1.20260623.1`.
- The fork adds per-isolate `memoryMB` enforcement + a multi-arch OCI image
  `ghcr.io/edutivo/quarvo-workerd:1.20260623.1-quarvo.1` (`linux/amd64` + `linux/arm64`), already
  built and enforcement-verified on both arches. **That part is finished; this task is only about CI speed.**
- Build entry point: **`quarvo/Dockerfile`** → `RUN bazel build --config=release_linux //src/workerd/server:workerd`.
- Release workflow: **`.github/workflows/quarvo-release.yml`** — matrix `{amd64: ubuntu-latest,
  arm64: ubuntu-24.04-arm}`, each runs `docker build` of `quarvo/Dockerfile`, pushes by digest; a
  `merge` job assembles the manifest. There's also `.github/workflows/quarvo-image-test.yml`.
- `gh` is authenticated on this box as `igorvbt`; the PAT **lacks `packages` scope** (CI uses the
  built-in `GITHUB_TOKEN` for GHCR). gh default repo is `edutivo/quarvo-workerd`. Note an `upstream`
  remote = `cloudflare/workerd` exists; pass `--repo edutivo/quarvo-workerd` to gh commands.
- **This box cannot build workerd** (1 core / tight RAM). All builds happen in CI. (A `~/.bazelrc`
  here points local builds at `/mnt/ephemeral` with `CC=/usr/bin/clang-19` — that's for *local* use,
  not CI.)

## Why builds are slow, and why the *current* cache does nothing

`quarvo-release.yml` already has `cache-from/to: type=gha,scope=<arch>` on `build-push-action`. That
caches **Docker layers**. But the entire expensive compile is a **single `RUN bazel build …` layer**,
so the layer cache only hits when that layer's inputs are byte-identical — i.e. **any source commit
busts it** and you eat the full ~3 h again. Docker-layer caching is the wrong level here.

**The right level is the Bazel action cache.** ~99% of the ~8,500 build actions are V8 / abseil /
ICU / boringssl — *identical across fork commits*. Bazel's `--disk_cache` (or a remote cache) keys
each action by a content hash, so on a rebuild every unchanged action is reused and only the handful
of changed files (our enforcement C++: `io-channels.h`, `server.c++`, `io-context.c++`) recompile +
relink. That turns ~3 h → minutes. **Bonus:** workerd's build already redacts timestamps
(`-D__DATE__="redacted"` etc.) and the version string comes from `release-version.txt` (source, not
build time), so the build is deterministic enough to cache well.

## The canonical reference (READ THIS FIRST)

Upstream already solves exactly this. Read:
- **`.github/workflows/_bazel.yml`** — uses `actions/cache@v5` on `~/bazel-disk-cache` with key
  `…-${{ runner.arch }}-${{ hashFiles('.bazelversion', '.bazelrc', 'MODULE.bazel') }}` (no
  `restore-keys` — intentional, to avoid snowballing cache size), a `--remote_cache=…` (Cloudflare's,
  NOT available to us), `--config=ci`, and a **"Drop large Bazel cache files"** step
  (`find ~/bazel-disk-cache -size +100M -type f -delete`) before saving, because GitHub Actions cache
  has a **10 GB per-repo limit**.
- **`Dockerfile.release`** (repo root) — shows the in-Docker disk-cache pattern: `COPY .bazel-cache
  /bazel-disk-cache`, `bazel build --disk_cache=/bazel-disk-cache …`, then `COPY --from=builder
  /bazel-disk-cache /.bazel-cache` to export the updated cache.

Mirror these for `quarvo-release.yml`.

## Recommended approach (free, no infra): persist `--disk_cache` via `actions/cache`

Pick one of two shapes. **A1 is cleaner for caching; A2 is lower-churn.**

### A1 — build Bazel natively in the workflow, then package a thin image (recommended)
Restructure the `build` job so the heavy compile happens in the workflow (where `actions/cache` can
see the disk cache), then `docker build` a tiny runtime image from the prebuilt binary:

```yaml
    steps:
      - uses: actions/checkout@v4
      - name: Reclaim disk space
        run: sudo rm -rf /usr/share/dotnet /opt/ghc /usr/local/lib/android /opt/hostedtoolcache || true
      - name: Install toolchain            # mirror quarvo/Dockerfile's builder stage
        run: |
          sudo apt-get update && sudo apt-get install -y --no-install-recommends \
            git python3 tcl dpkg-dev ca-certificates \
            clang-19 lld-19 libc++-19-dev libc++abi-19-dev libunwind-19-dev libclang-rt-19-dev
          npm install -g pnpm@10.32.1 @bazel/bazelisk
      - name: Restore Bazel disk cache
        uses: actions/cache@v4
        with:
          path: ~/bazel-disk-cache
          key: bazel-disk-${{ matrix.arch }}-${{ hashFiles('.bazelversion','.bazelrc','MODULE.bazel') }}
      - name: Build
        run: |
          pnpm install
          echo "build:linux --repo_env=CC=/usr/bin/clang-19" >> .bazelrc
          bazel build --config=release_linux --disk_cache=$HOME/bazel-disk-cache //src/workerd/server:workerd
          cp bazel-bin/src/workerd/server/workerd ./workerd-bin
      - name: Trim cache to fit GitHub's 10 GB limit
        run: find ~/bazel-disk-cache -size +100M -type f -delete || true
      # …then `docker build` a thin Dockerfile (FROM debian:trixie-slim; COPY workerd-bin …) and
      #   push by digest exactly as today; the merge job is unchanged.
```

Notes:
- **Per-arch key** (`matrix.arch`) — amd64 and arm64 action hashes differ; never share one cache.
- `actions/cache` saves on job success. First run = cold (populates). Second run with unchanged V8 =
  mostly hits → minutes.
- The thin runtime image still needs `RUN /usr/local/bin/workerd --version` as a smoke test.

### A2 — keep the Docker build, plumb the cache in/out (mirrors `Dockerfile.release`)
Lower churn to the workflow, but more Docker plumbing:
1. `actions/cache` restore a `.bazel-cache/` dir into the build context.
2. In `quarvo/Dockerfile`: `COPY .bazel-cache* /bazel-disk-cache/` and add `--disk_cache=/bazel-disk-cache`
   to the `bazel build` line; add a `FROM scratch AS bazelcache` stage with `COPY --from=builder
   /bazel-disk-cache /`.
3. Build twice / or use `docker buildx build --target bazelcache --output type=local,dest=.bazel-cache`
   to extract the updated cache, then `actions/cache` saves it (after the >100 MB trim).

(There's also BuildKit `RUN --mount=type=cache,target=/root/.cache/bazel …` + `cache-to:
type=gha,mode=max`, but exporting a multi-GB cache mount to the gha backend is slow and bumps the
10 GB limit; prefer explicit `--disk_cache` + `actions/cache`.)

## Scale-ups (if the disk cache doesn't fit 10 GB, or you want cross-runner sharing)

- **B — Bazel remote cache.** Stand up `buildbarn`/`bazel-remote` (a container backed by a bucket) or
  use a SaaS (BuildBuddy/EngFlow free tier), pass `--remote_cache=<url>` + a secret. No 10 GB cap,
  shared across arches/runs/PRs. This is what upstream does (their cache is private to them).
- **C — self-hosted runner(s)** with a persistent on-disk `--disk_cache` (or the bazel `output_base`)
  that survives between jobs — fastest incremental, no upload/download, no size cap; costs a VM to run.

## Gotchas / must-knows
- **10 GB GitHub cache limit.** workerd+V8's disk cache likely exceeds it; the `find … -size +100M
  -delete` trim keeps the many small action outputs (the bulk of hits) and drops the few huge ones
  (they'll re-run — acceptable). **Measure** `du -sh ~/bazel-disk-cache` after a cold build to decide
  whether disk_cache (A) suffices or you need remote cache (B).
- **Exact cache key, no `restore-keys`** (snowballing — see upstream's comment).
- **`pnpm install` is non-frozen** today and can drift `@types/node` between runs (we've seen 25.9.4);
  that only re-runs the cheap `//src/node` tsproject action, not V8 — but pinning would make even that
  a stable hit. (See the `noCheck` stopgap in `quarvo/MAINTENANCE.md`; leave it in place.)
- Cache the **disk_cache** dir, NOT bazel's `output_base`/working tree.
- Keep the existing `merge` job and `quarvo-image-test.yml` as-is.

## How to verify the win
1. Trigger a cold build (move the `v…-quarvo.*` tag) → note duration (~3 h, populates cache).
2. Make a **trivial** change to one enforcement file (e.g. a comment in `src/workerd/server/server.c++`),
   re-tag, re-trigger → it should finish in **minutes** (V8 = cache hits; only that file recompiles + relink).
   Watch with `gh run watch <id> --repo edutivo/quarvo-workerd` or the monitor pattern.
3. Confirm correctness still holds: push an `imgtest-*` tag to run `quarvo-image-test` (both arches).

## Triggering reference (how this repo's CI is driven)
- Release: push/move a tag matching `v*-quarvo.*` (e.g. `git tag -f v1.20260623.1-quarvo.1 <branch> &&
  git push -f origin <tag>`), or `gh workflow run quarvo-release.yml` once it's on the default branch.
- Image test: push a tag `imgtest-*`.
- The inherited upstream workflows are **disabled** (pruned) — only `quarvo-release` and
  `quarvo-image-test` are active; leave the rest disabled.

## Definition of done
`quarvo-release.yml` persists a per-arch Bazel cache; a no-V8-change rebuild completes in minutes
instead of ~3 h; both-arch `quarvo-image-test` still green; approach + any new secrets documented in
`quarvo/MAINTENANCE.md`.
