FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config ca-certificates curl \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    libssl-dev libboost-system-dev libcurl4-openssl-dev libspdlog-dev \
    nlohmann-json3-dev libhiredis-dev libmysqlcppconn-dev libjsoncpp-dev && \
    rm -rf /var/lib/apt/lists/*

# 使用 gitclone 镜像加速 GitHub 源码下载
RUN git config --global url."https://gitclone.com/github.com/".insteadOf "https://github.com/"

WORKDIR /opt/third_party_src

# AMQP-CPP（开启 Linux TCP）
RUN git clone --depth 1 https://github.com/CopernicaMarketingSoftware/AMQP-CPP.git && \
    cd AMQP-CPP && \
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DAMQP-CPP_LINUX_TCP=ON . && \
    cmake --build build -j"$(nproc)" && cmake --install build

# redis-plus-plus
RUN git clone --depth 1 https://github.com/sewenew/redis-plus-plus.git && \
    cd redis-plus-plus && \
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DREDIS_PLUS_PLUS_CXX_STANDARD=17 . && \
    cmake --build build -j"$(nproc)" && cmake --install build

# cpr
RUN git clone --depth 1 https://github.com/libcpr/cpr.git && \
    cd cpr && \
    cmake -B build -DCPR_USE_SYSTEM_CURL=ON . && \
    cmake --build build -j"$(nproc)" && cmake --install build

# elasticlient
RUN git clone --depth 1 https://github.com/seznam/elasticlient.git && \
    cd elasticlient && \
    cmake -B build -DUSE_ALL_SYSTEM_LIBS=YES -DBUILD_ELASTICLIENT_TESTS=OFF \
      -DBUILD_ELASTICLIENT_EXAMPLE=OFF -DBUILD_SHARED_LIBS=ON . && \
    cmake --build build -j"$(nproc)" && cmake --install build

# prometheus-cpp
RUN git clone --depth 1 https://github.com/jupp0r/prometheus-cpp.git && \
    cd prometheus-cpp && \
    cmake -B build -DBUILD_SHARED_LIBS=ON -DENABLE_TESTING=OFF . && \
    cmake --build build -j"$(nproc)" && cmake --install build

# AWS SDK C++（仅 S3，使用系统 OpenSSL，关闭 s2n 以缩短构建时间）
RUN git clone --depth 1 --recurse-submodules --shallow-submodules \
      https://github.com/aws/aws-sdk-cpp.git && \
    cd aws-sdk-cpp && \
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_ONLY=s3 \
      -DBUILD_SHARED_LIBS=ON -DENABLE_TESTING=OFF \
      -DUSE_OPENSSL=ON -DUSE_S2N=OFF -DENFORCE_SUBMODULE_VERSIONS=OFF . && \
    cmake --build build -j"$(nproc)" && cmake --install build

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release . && \
    cmake --build build -j"$(nproc)" && \
    ./build/unit_tests

FROM ubuntu:22.04 AS runtime

COPY --from=builder /src/build/api_server /usr/local/bin/api_server
COPY --from=builder /src/build/transcode_worker /usr/local/bin/transcode_worker
COPY --from=builder /src/config.json /etc/transcode/config.json

# 运行时依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
    libavformat58 libavcodec58 libavutil56 libswscale5 libswresample3 \
    libssl3 libcurl4 libmysqlcppconn7v5 libboost-system1.74.0 && \
    rm -rf /var/lib/apt/lists/*

EXPOSE 8080

CMD ["api_server", "/etc/transcode/config.json"]
