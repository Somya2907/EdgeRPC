FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/edgerpc
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY apps apps
COPY benchmarks benchmarks
COPY tests tests
COPY scripts scripts
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel 4 \
    && ctest --test-dir build --output-on-failure

FROM ubuntu:24.04 AS validation
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake python3 libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/edgerpc
COPY --from=build /opt/edgerpc /opt/edgerpc
# Both processes run as the same unprivileged user in this container's IPC namespace.
RUN useradd --create-home --uid 10001 edgerpc && chown -R edgerpc:edgerpc /opt/edgerpc
USER edgerpc
CMD ["python3", "scripts/validate_platform.py", "--bin-dir", "build", "--output", "/tmp/edgerpc-validation"]
