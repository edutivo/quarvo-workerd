# Builds a runnable / COPY-able workerd image carrying the quarvo per-isolate limit-enforcement
# patch. Mirrors the upstream Dockerfile.release toolchain (clang-19 + Bazel + vendored V8).
#
# HEAVY build: the vendored V8 compile needs a CAPABLE builder -- roughly >=40 GB free disk,
# >=16 GB RAM, and several cores (a 1-core / 17 GB box cannot finish it; see quarvo/MAINTENANCE.md).
#
# This Dockerfile carries a persistent Bazel ACTION cache (--disk_cache) so rebuilds are fast: ~99%
# of the ~8,500 build actions (V8/abseil/ICU/boringssl) are identical across our fork's commits, so a
# warm cache turns the ~3 h cold compile into minutes. It has four useful targets:
#
#   * runtime (default) -- a runnable image. A plain `docker build` compiles cold (empty seed) and
#       produces the runtime image, exactly as before:
#         docker build -f quarvo/Dockerfile -t ghcr.io/edutivo/quarvo-workerd:<tag> .
#   * export            -- emits the workerd binary + the *updated* disk cache to a local dir; this is
#       what quarvo-release.yml builds so actions/cache can persist the disk cache across CI runs:
#         docker buildx build -f quarvo/Dockerfile --target export \
#           --build-context bazelcache=<restored-cache-dir> \
#           --output type=local,dest=out .
#   * quarvo-test       -- runs the quarvo bazel test targets against the same build tree + disk
#       cache; target list via the QUARVO_TEST_TARGETS build-arg (CI runs this on PRs to
#       quarvo-main and on quarvo-main pushes).
#   * builder           -- the heavy compile stage the three above share.
#
# See quarvo/MAINTENANCE.md "CI build caching" for the full workflow + the thin push image
# (quarvo/Dockerfile.runtime).
#
# quarvo consumes the published image via:
#   COPY --from=ghcr.io/edutivo/quarvo-workerd:<tag> /usr/local/bin/workerd ...

# Default (empty) Bazel disk-cache seed. CI overrides this stage with a real cache directory via
# `--build-context bazelcache=<dir>`; a plain `docker build` leaves it empty (a fully cold compile).
FROM scratch AS bazelcache

FROM node:trixie AS builder
WORKDIR /workerd

# Debian trixie (node:trixie) ships LLVM 19 natively (clang-19 19.1.7), so install it straight from
# the distro repos. No apt.llvm.org PPA / software-properties-common (that package is not in trixie,
# and was the original cause of the build failing at this step).
RUN apt-get update && apt-get install -y --no-install-recommends \
      git python3 tcl dpkg-dev ca-certificates \
      clang-19 lld-19 libc++-19-dev libc++abi-19-dev libunwind-19-dev libclang-rt-19-dev

COPY . .
RUN echo "build:linux --repo_env=CC=/usr/bin/clang-19" >> .bazelrc

# bazelisk provides the `bazel` command and honors .bazelversion. workerd does not declare it as a
# pnpm dependency, so install it explicitly rather than relying on `pnpm exec bazel`.
RUN npm install -g pnpm@10.32.1 @bazel/bazelisk && pnpm install

# Seed the Bazel disk cache from the (overridable) bazelcache build context. Empty on a cold build;
# on a warm CI run it carries ~all of V8/abseil/ICU/boringssl so only changed files recompile + relink.
COPY --from=bazelcache / /bazel-disk-cache/

# Fast-fail the TypeScript type-check (//src/node) FIRST: it is cheap (~minutes, no V8) and is the
# step most sensitive to the typescript / @types/node versions, so a dependency regression surfaces
# here in minutes instead of after the ~3h V8 compile. Shares the same disk cache as the build below.
RUN bazel build --disk_cache=/bazel-disk-cache //src/node:node@tsproject

# The release config produces an optimized, statically-linked binary (upstream ships it FROM
# scratch), so the runtime stage below needs no extra libraries.
RUN bazel build --disk_cache=/bazel-disk-cache --config=release_linux //src/workerd/server:workerd \
    && cp bazel-bin/src/workerd/server/workerd /workerd-bin

# Trim the handful of huge cache entries (>100 MB) before the cache is exported/persisted: the GitHub
# Actions cache is capped at 10 GB/repo. The many small action outputs (the bulk of cache hits) are
# kept; the few large ones re-run on the next cold build -- acceptable. Mirrors upstream
# _bazel.yml's "Drop large Bazel cache files" step. Harmless for a plain `docker build` (the builder
# layer is discarded). NOTE: requires GNU find (Debian) for the `-size +100M` suffix.
RUN find /bazel-disk-cache -size +100M -type f -delete || true

# CI extraction target: emit ONLY the binary + the updated (trimmed) disk cache to a local dir via
# `--output type=local`. FROM scratch keeps the output to exactly these two paths.
FROM scratch AS export
COPY --from=builder /workerd-bin /workerd-bin
COPY --from=builder /bazel-disk-cache /bazel-disk-cache

# CI test target: run the quarvo regression test targets against the same build tree + disk cache.
# The target list is a BUILD-ARG (not baked into this file) so extending it later does NOT change
# this file's hash — which is part of the CI cache key (editing this file forces a ~3h cold build).
# worker-loader-test@ is excluded by default: it is tagged requires-network and flaky in builders.
FROM builder AS quarvo-test
ARG QUARVO_TEST_TARGETS="//src/workerd/api/tests:worker-loader-memory-test@ //src/workerd/api/tests:worker-loader-limits-test@"
RUN bazel test --disk_cache=/bazel-disk-cache --config=release_linux \
      --test_output=errors ${QUARVO_TEST_TARGETS} \
    && touch /quarvo-tests-ok

# Default target: a runnable image built in one shot from the freshly compiled binary. CI does NOT
# use this stage (it builds the thin quarvo/Dockerfile.runtime from the exported binary to avoid a
# second compile); it exists for local `docker build` convenience and is kept in sync with that file.
FROM debian:trixie-slim AS runtime
COPY --from=builder /workerd-bin /usr/local/bin/workerd
# Fail the build early if the binary is not self-contained on a slim base.
RUN /usr/local/bin/workerd --version
ENTRYPOINT ["/usr/local/bin/workerd"]
CMD ["--help"]
