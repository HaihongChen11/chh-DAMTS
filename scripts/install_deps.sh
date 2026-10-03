#!/usr/bin/env bash
set -euo pipefail

sudo() { command sudo "$@"; }

install_cmake_project() {
  local url="$1"
  shift
  local name
  name="$(basename "$url" .git)"
  git clone --depth 1 "$url" "/tmp/$name"
  cd "/tmp/$name"
  cmake -B build -DCMAKE_BUILD_TYPE=Release "$@"
  sudo cmake --build build --target install -j"$(nproc)"
  cd /
}

# AMQP-CPP with Linux TCP
install_cmake_project https://github.com/CopernicaMarketingSoftware/AMQP-CPP.git -DAMQP-CPP_LINUX_TCP=ON

# redis-plus-plus
install_cmake_project https://github.com/sewenew/redis-plus-plus.git -DREDIS_PLUS_PLUS_CXX_STANDARD=17

# cpr
install_cmake_project https://github.com/libcpr/cpr.git -DCPR_USE_SYSTEM_CURL=ON

# elasticlient
install_cmake_project https://github.com/seznam/elasticlient.git -DUSE_ALL_SYSTEM_LIBS=YES -DBUILD_ELASTICLIENT_TESTS=OFF -DBUILD_ELASTICLIENT_EXAMPLE=OFF -DBUILD_SHARED_LIBS=ON

# prometheus-cpp
install_cmake_project https://github.com/jupp0r/prometheus-cpp.git -DBUILD_SHARED_LIBS=ON -DENABLE_TESTING=OFF

# AWS SDK C++ (S3 only)
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/aws/aws-sdk-cpp.git /tmp/aws-sdk-cpp
cd /tmp/aws-sdk-cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_ONLY=s3 -DBUILD_SHARED_LIBS=ON -DENABLE_TESTING=OFF -DUSE_OPENSSL=ON -DUSE_S2N=OFF -DENFORCE_SUBMODULE_VERSIONS=OFF .
sudo cmake --build build --target install -j"$(nproc)"
