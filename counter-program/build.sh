#!/usr/bin/env bash
# Build the counter program reproducibly (Docker) and copy the result to ./counter.bin.
# Needs cargo-risczero 3.0.5 (`rzup install cargo-risczero 3.0.5`) and a running Docker.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
# Same guest-builder image LEZ builds its own programs with (Justfile:
# RISC0_DOCKER_CONTAINER_TAG); the default r0.1.88 is too old for its deps.
RISC0_DOCKER_CONTAINER_TAG="${RISC0_DOCKER_CONTAINER_TAG:-r0.1.91.1}" cargo risczero build --manifest-path Cargo.toml
BIN=$(find target -path '*riscv32im-risc0-zkvm-elf/docker/*' -name 'counter.bin' | head -1)
[ -n "$BIN" ] || { echo "counter.bin not found under target/" >&2; exit 1; }
cp "$BIN" "$HERE/counter.bin"
ls -l "$HERE/counter.bin"
