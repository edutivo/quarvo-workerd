# Builds a runnable / COPY-able workerd image carrying the quarvo per-isolate limit-enforcement
# patch. Mirrors the upstream Dockerfile.release toolchain (clang-19 + Bazel + vendored V8).
#
# HEAVY build: the vendored V8 compile needs a CAPABLE builder — roughly >=40 GB free disk,
# >=16 GB RAM, and several cores (a 1-core / 17 GB box cannot finish it; see quarvo/MAINTENANCE.md).
# Use the bazel disk cache (mounted or baked) for fast incremental rebuilds.
#
#   docker build -f quarvo/Dockerfile \
#     -t ghcr.io/edutivo/quarvo-workerd:1.20260623.1-quarvo.1 .
#
# quarvo consumes it via:  COPY --from=ghcr.io/edutivo/quarvo-workerd:<tag> /usr/local/bin/workerd ...
FROM node:trixie AS builder
WORKDIR /workerd

RUN apt-get update && apt-get install -y --no-install-recommends \
      curl dpkg-dev git lsb-release wget software-properties-common gnupg tcl

# clang/lld/libc++ 19, matching upstream Dockerfile.release.
RUN wget https://apt.llvm.org/llvm.sh \
    && sed -i '/apt-get install/d' llvm.sh \
    && chmod +x llvm.sh \
    && ./llvm.sh 19
RUN apt-get install -y --no-install-recommends \
      clang-19 lld-19 libunwind-19 libc++abi1-19 libc++1-19 libc++-19-dev libclang-rt-19-dev

COPY . .
RUN echo "build:linux --repo_env=CC=/usr/lib/llvm-19/bin/clang" >> .bazelrc

RUN npm install -g pnpm@10.32.1 && pnpm install

# The release config produces an optimized, statically-linked binary (upstream ships it FROM
# scratch), so the runtime stage below needs no extra libraries.
RUN pnpm exec bazel build --config=release_linux //src/workerd/server:workerd \
    && cp bazel-bin/src/workerd/server/workerd /workerd-bin

FROM debian:trixie-slim
COPY --from=builder /workerd-bin /usr/local/bin/workerd
# Fail the build early if the binary is not self-contained on a slim base.
RUN /usr/local/bin/workerd --version
ENTRYPOINT ["/usr/local/bin/workerd"]
CMD ["--help"]
