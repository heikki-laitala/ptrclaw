# syntax=docker/dockerfile:1

# ── Stage 1: build ───────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang lld meson ninja-build libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN meson setup builddir \
        --native-file meson-native-linux.ini \
        --buildtype=release \
        -Db_lto=true \
        -Dcatch2:tests=false \
    && meson compile -C builddir ptrclaw

# ── Stage 2: runtime ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim

# Runtime deps: TLS roots + libssl for HTTPS, no other native deps needed.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r ptrclaw \
    && useradd -r -g ptrclaw -m -d /home/ptrclaw ptrclaw

COPY --from=builder /src/builddir/ptrclaw /usr/local/bin/ptrclaw

# Config lives at ~/.ptrclaw/config.json inside the container.
# Mount a host directory here or supply values via environment variables.
VOLUME ["/home/ptrclaw/.ptrclaw"]

USER ptrclaw
WORKDIR /home/ptrclaw

ENTRYPOINT ["ptrclaw"]
