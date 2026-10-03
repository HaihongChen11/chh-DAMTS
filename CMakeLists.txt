cmake_minimum_required(VERSION 3.16)
project(transcode_service LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -ggdb")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# ------------------------------------------------------------------
# FFmpeg 底层 C 库（转码 + 缩略图，通过 pkg-config 查找）
# ------------------------------------------------------------------
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libswresample)

# ------------------------------------------------------------------
# OpenSSL（密码加盐哈希 + amqp-cpp TLS）
# ------------------------------------------------------------------
find_package(OpenSSL REQUIRED)

# ------------------------------------------------------------------
# 其它第三方中间件客户端库
# 注：这些库需提前安装（见 README「依赖安装」），包名随发行版略有差异，
# 若 find_package 找不到，可改为手动指定 include 目录与库名。
# ------------------------------------------------------------------
find_package(spdlog QUIET)
find_package(nlohmann_json QUIET)
find_package(prometheus-cpp CONFIG QUIET)
find_package(AWSSDK QUIET COMPONENTS s3 core)
find_package(MySQLConnectorCpp QUIET)

# redis-plus-plus 官方 CMake 包会强依赖 hiredis 的 CMake config，
# 但 Ubuntu 上的 hiredis 只提供 pkg-config，直接用 pkg-config 规避该问题。
pkg_check_modules(REDISPP REQUIRED IMPORTED_TARGET redis++)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

# ------------------------------------------------------------------
# 公共核心源码（编译为静态库，两个可执行程序共享）
# ------------------------------------------------------------------
set(CORE_SOURCES
    src/common/logger.cpp
    src/common/config.cpp
    src/common/uuid.cpp
    src/metrics/metrics.cpp
    src/reactor/buffer.cpp
    src/reactor/epoller.cpp
    src/reactor/event_loop.cpp
    src/reactor/tcp_connection.cpp
    src/reactor/http_parser.cpp
    src/reactor/http_response.cpp
    src/reactor/router.cpp
    src/reactor/http_server.cpp
    src/pool/thread_pool.cpp
    src/redis/rate_limiter.cpp
    src/redis/distributed_lock.cpp
    src/service/db.cpp
    src/service/auth_service.cpp
    src/service/storage_service.cpp
    src/service/mq_service.cpp
    src/service/es_service.cpp
    src/service/task_service.cpp
    src/service/service_registry.cpp
    src/ffmpeg/ffmpeg_helper.cpp
)

add_library(transcode_core STATIC ${CORE_SOURCES})
target_include_directories(transcode_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(transcode_core PUBLIC -Wall -Wextra)

# 基础依赖
target_link_libraries(transcode_core PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
)

# FFmpeg
target_link_libraries(transcode_core PUBLIC PkgConfig::FFMPEG)

# 中间件客户端库（按需调整）
target_link_libraries(transcode_core PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    mysqlcppconn
    PkgConfig::REDISPP
    PkgConfig::HIREDIS
    amqpcpp
    boost_system
    aws-cpp-sdk-s3
    aws-cpp-sdk-core
    elasticlient
    cpr
    curl
    prometheus-cpp::core
)

# ------------------------------------------------------------------
# 可执行程序
# ------------------------------------------------------------------
add_executable(api_server
    src/api_server/main.cpp
    src/api_server/http_handler.cpp
)
target_link_libraries(api_server PRIVATE transcode_core)

add_executable(transcode_worker
    src/worker/main.cpp
)
target_link_libraries(transcode_worker PRIVATE transcode_core)

enable_testing()
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    add_executable(unit_tests tests/unit_tests.cpp)
    target_link_libraries(unit_tests PRIVATE transcode_core)
    add_test(NAME unit_tests COMMAND unit_tests)
endif()

install(TARGETS api_server transcode_worker RUNTIME DESTINATION bin)
