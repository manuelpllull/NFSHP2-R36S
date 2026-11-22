#!/bin/sh
# Builds nfshp2-unpack in Docker: r36s/build/armhf/nfshp2-unpack, one static
# 32-bit ARM program (musl), so it runs wherever the port does and needs
# no libraries from the firmware. r36s/package.sh puts it in nfshp2/tools/.
#
#   r36s/installer/unpack/build.sh
set -eu
here=$(cd "$(dirname "$0")" && (pwd -W 2>/dev/null || pwd))
r36s=$(cd "$(dirname "$0")/../.." && (pwd -W 2>/dev/null || pwd))
target=armv7-unknown-linux-musleabihf
MSYS_NO_PATHCONV=1 docker run --rm -v "$here:/src:ro" -v "$r36s/build:/out" \
    "${IMAGE:-rust:1.88-bookworm}" sh -ec "
        rustup target add $target >/dev/null
        CARGO_TARGET_DIR=/tmp/t RUSTFLAGS='-C linker=rust-lld -C target-cpu=cortex-a7' \
            cargo build --quiet --release --locked --manifest-path /src/Cargo.toml --target $target
        mkdir -p /out/armhf
        cp /tmp/t/$target/release/nfshp2-unpack /out/armhf/nfshp2-unpack"
echo "r36s/build/armhf/nfshp2-unpack"
