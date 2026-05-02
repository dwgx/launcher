# 备用：用容器跑也行，主路径是 systemd
FROM rust:1.75-bookworm AS build
WORKDIR /src
COPY . .
RUN apt-get update && apt-get install -y protobuf-compiler && rm -rf /var/lib/apt/lists/*
RUN cargo build --release -p launcher-api

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libssl3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/target/release/systembackend /app/systembackend
COPY --from=build /src/migrations /app/migrations
EXPOSE 1337
ENV LAUNCHER_CONFIG=/app/config.toml
CMD ["/app/systembackend"]
