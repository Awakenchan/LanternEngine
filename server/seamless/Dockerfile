FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make libssl-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY engine/gpu/bbnet/room/ engine/gpu/bbnet/room/
COPY engine/gpu/bbnet/third_party/httplib/ engine/gpu/bbnet/third_party/httplib/
COPY engine/tests/test_room_host.cpp engine/tests/test_room_host.cpp
COPY server/seamless/ server/seamless/
RUN cmake -S server/seamless -B /build -DCMAKE_BUILD_TYPE=Release
RUN cmake --build /build -j2

FROM build AS test
RUN ctest --test-dir /build --output-on-failure

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 libstdc++6 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=test /build/private-coop-relay /usr/local/bin/private-coop-relay
COPY NOTICE LICENSE /usr/share/doc/private-coop-relay/
COPY server/seamless/README.md /usr/share/doc/private-coop-relay/README.md
USER 65534:65534
EXPOSE 38765
ENTRYPOINT ["/usr/local/bin/private-coop-relay"]
