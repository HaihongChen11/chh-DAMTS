FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config ca-certificates curl \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    libssl-dev libboost-system-dev libcurl4-openssl-dev libspdlog-dev \
    nlohmann-json3-dev libhiredis-dev libmysqlcppconn-dev libjsoncpp-dev \
    libcivetweb-dev civetweb zlib1g-dev && \
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
    cmake -B build -DCPR_USE_SYSTEM_CURL=ON -DBUILD_SHARED_LIBS=ON . && \
    cmake --build build -j"$(nproc)" && cmake --install build

# elasticlient
RUN git clone --depth 1 https://github.com/seznam/elasticlient.git && \
    cd elasticlient && \
    sed -i 's/c++11/c++17/g; s/CMAKE_CXX_STANDARD 11/CMAKE_CXX_STANDARD 17/g; s/cmake_minimum_required(VERSION 2\.8\.7)/cmake_minimum_required(VERSION 3.10)/' CMakeLists.txt && \
    sed -i 's/set(JSONCPP_LIBRARIES ${JSONCPP_LIBRARY} CACHE INTERNAL "")/set(JSONCPP_LIBRARIES jsoncpp CACHE INTERNAL "")/' external/CMakeLists.txt && \
    cmake -B build -DCMAKE_CXX_STANDARD=17 -DUSE_ALL_SYSTEM_LIBS=YES -DBUILD_ELASTICLIENT_TESTS=OFF \
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

# 运行阶段直接复用 builder，保留所有已安装的源码依赖与构建产物，
# 避免动态库缺失导致 api_server / transcode_worker 无法启动。
FROM builder AS runtime

WORKDIR /src
EXPOSE 8080

CMD ["./build/api_server", "config.json"]
