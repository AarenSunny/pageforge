# syntax=docker/dockerfile:1

FROM gcc:14-bookworm AS build
WORKDIR /src

COPY Makefile ./
COPY include ./include
COPY src ./src

RUN make build/pageforge CXX=g++ \
    CXXFLAGS="-std=c++20 -O2 -g0 -Wall -Wextra -Wpedantic -Werror \
    -static-libstdc++ -static-libgcc"

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install --yes --no-install-recommends passwd \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system pageforge \
    && useradd --system --gid pageforge --create-home pageforge \
    && install --directory --owner=pageforge --group=pageforge /data

COPY --from=build /src/build/pageforge /usr/local/bin/pageforge

LABEL org.opencontainers.image.source="https://github.com/AarenSunny/pageforge" \
      org.opencontainers.image.description="PageForge relational database engine demo"

USER pageforge
WORKDIR /data
ENTRYPOINT ["pageforge"]
