#!/usr/bin/env bash
set -euo pipefail

sudo() { command sudo "$@"; }

sudo apt-get update -qq
sudo apt-get install -y -qq libjsoncpp-dev libcivetweb-dev civetweb zlib1g-dev

install_cmake_project() {
  local url="$1"
  shift
  local name
  name="$(basename "$url" .git)"
  rm -rf "/tmp/$name"
  git clone --depth 1 "$url" "/tmp/$name"
  cd "/tmp/$name"
  if [ "$name" = "elasticlient" ]; then
    # elasticlient 仍硬编码 C++11，但新版 cpr 头文件使用了 C++17 CTAD，
    # 这里将 elasticlient 的 C++ 标准提升为 17，并顺手消除 CMake <2.8.12 弃用警告。
    sed -i 's/c++11/c++17/g; s/CMAKE_CXX_STANDARD 11/CMAKE_CXX_STANDARD 17/g; s/cmake_minimum_required(VERSION 2\.8\.7)/cmake_minimum_required(VERSION 3.10)/' CMakeLists.txt
    # Ubuntu 的 jsoncpp 以 CMake 导出目标提供，目标作用域到不了 elasticlient 的 src 子目录，
    # 其源码仍用 ${JSONCPP_LIBRARIES} 拼成 -ljsoncpp_lib，导致链接失败；改成标准库名。
    sed -i 's/set(JSONCPP_LIBRARIES ${JSONCPP_LIBRARY} CACHE INTERNAL "")/set(JSONCPP_LIBRARIES jsoncpp CACHE INTERNAL "")/' external/CMakeLists.txt
  fi
  cmake -B build -DCMAKE_BUILD_TYPE=Release "$@"
  sudo cmake --build build --target install -j"$(nproc)"
  cd /
}

# AMQP-CPP with Linux TCP
install_cmake_project https://github.com/CopernicaMarketingSoftware/AMQP-CPP.git -DAMQP-CPP_LINUX_TCP=ON

# redis-plus-plus
install_cmake_project https://github.com/sewenew/redis-plus-plus.git -DREDIS_PLUS_PLUS_CXX_STANDARD=17

# cpr
install_cmake_project https://github.com/libcpr/cpr.git -DCPR_USE_SYSTEM_CURL=ON -DBUILD_SHARED_LIBS=ON

# elasticlient
install_cmake_project https://github.com/seznam/elasticlient.git -DCMAKE_CXX_STANDARD=17 -DUSE_ALL_SYSTEM_LIBS=YES -DBUILD_ELASTICLIENT_TESTS=OFF -DBUILD_ELASTICLIENT_EXAMPLE=OFF -DBUILD_SHARED_LIBS=ON

# prometheus-cpp
install_cmake_project https://github.com/jupp0r/prometheus-cpp.git -DBUILD_SHARED_LIBS=ON -DENABLE_TESTING=OFF

# AWS SDK C++ (S3 only)
if [ -f /usr/local/lib/libaws-cpp-sdk-s3.so ] && [ -d /usr/local/lib/cmake/aws-cpp-sdk-s3 ]; then
  echo "AWS SDK C++ (S3) already installed, skip build."
else
  rm -rf /tmp/aws-sdk-cpp
  git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/aws/aws-sdk-cpp.git /tmp/aws-sdk-cpp
  cd /tmp/aws-sdk-cpp
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_ONLY=s3 -DBUILD_SHARED_LIBS=ON -DENABLE_TESTING=OFF -DUSE_OPENSSL=ON -DUSE_S2N=OFF -DENFORCE_SUBMODULE_VERSIONS=OFF .
  sudo cmake --build build --target install -j"$(nproc)"
fi
