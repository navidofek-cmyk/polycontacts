# Stage 1: build
FROM rust:1.87-slim AS builder

WORKDIR /app

# Install build dependencies (OpenSSL, pkg-config for reqwest)
RUN apt-get update && \
    apt-get install -y --no-install-recommends pkg-config libssl-dev && \
    rm -rf /var/lib/apt/lists/*

# Cache dependencies by copying manifests first
COPY Cargo.toml ./
RUN mkdir src && echo 'fn main(){}' > src/main.rs && cargo build --release && rm -rf src

# Build the real source
COPY src ./src
# Touch main.rs so cargo rebuilds it
RUN touch src/main.rs && cargo build --release

# Stage 2: runtime
FROM debian:bookworm-slim

RUN apt-get update && \
    apt-get install -y --no-install-recommends curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/target/release/search-rust /usr/local/bin/search-rust

EXPOSE 8081

ENV RUST_LOG=info

ENTRYPOINT ["/usr/local/bin/search-rust"]
