# Craft Survival server + web client, one container (arm64/amd64).
# The server is the native client binary in --server mode (headless: it
# never opens a window), so this builds the full game with raylib. The
# build dir is a BuildKit cache mount, so raylib compiles once and only
# changed game files rebuild on redeploys.
# Build:  docker build -t craft-server .
# Run:    docker run -d --name craft --restart unless-stopped \
#           -p 8080:8080 -v craft_world:/data craft-server
# World persists in the craft_world volume. deploy_pi.cmd stages everything.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libgl1-mesa-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src/ src/
COPY tests/ tests/
COPY assets/ assets/
RUN --mount=type=cache,target=/src/build \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" --target craft \
    && cp build/craft /craft

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libx11-6 libxrandr2 libxinerama1 libxcursor1 libxi6 libgl1 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /craft .
COPY web/ web/
ENV STATIC=/app/web CRAFT_DATA=/data
VOLUME /data
EXPOSE 8080
CMD ["./craft", "--server"]
