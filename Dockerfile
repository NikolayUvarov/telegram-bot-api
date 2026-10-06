# Telegram Bot API server with MTProxy support.
#
#   git clone --recursive https://github.com/NikolayUvarov/telegram-bot-api.git
#   cd telegram-bot-api
#   docker build -t telegram-bot-api .
#   docker run -d -p 8081:8081 -v telegram-bot-api:/var/lib/telegram-bot-api \
#     -e TELEGRAM_API_ID=... -e TELEGRAM_API_HASH=... -e TELEGRAM_MTPROXY='tg://proxy?server=...' telegram-bot-api

FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates cmake g++ gperf make ninja-build libssl-dev zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN test -f td/CMakeLists.txt || (echo "td sources are missing: clone the repository with --recursive" && exit 1)
# the number of parallel jobs, e.g. --build-arg JOBS=2 if the build runs out of memory; all cores by default
ARG JOBS=
RUN cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -B /build . \
 && cmake --build /build --target install ${JOBS:+-j$JOBS}

FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libssl3t64 zlib1g \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home-dir /var/lib/telegram-bot-api telegram-bot-api \
 && mkdir -p /var/lib/telegram-bot-api \
 && chown telegram-bot-api /var/lib/telegram-bot-api
COPY --from=build /usr/local/bin/telegram-bot-api /usr/local/bin/telegram-bot-api
USER telegram-bot-api
VOLUME /var/lib/telegram-bot-api
EXPOSE 8081
ENTRYPOINT ["telegram-bot-api", "--dir=/var/lib/telegram-bot-api"]
