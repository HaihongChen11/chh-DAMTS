# 分布式多媒体异步转码处理服务

一个基于 **C++17** 的分布式视频异步转码后端，采用「HTTP API 网关 + 转码 Worker」双进程架构。
核心亮点：**手写 epoll Reactor 网络库、流式 multipart 解析、连接池/线程池、Redis 滑动窗口限流与分布式锁、
MySQL 事务、RabbitMQ 可靠性投递（confirm / 手动 ACK / 死信队列）、MinIO 对象存储、Elasticsearch 全文检索，
以及基于 FFmpeg 底层 C 库（libav\*）的转码与缩略图抽取**。

> 说明：本仓库为简历/学习项目，代码聚焦于 C++ 服务端工程实践与分布式中间件整合，运行环境为 Linux（Ubuntu）。
> 由于依赖 FFmpeg、aws-sdk、prometheus-cpp 等原生库，**无法在 Windows 下直接编译**，请在 WSL / 云主机 /
> VSCode Remote-SSH 的 Ubuntu 环境中构建运行。

---

## 一、整体架构

```
                          ┌─────────────────────────────────────────────┐
                          │               api_server (HTTP)             │
                          │                                             │
  client ──HTTP──►  ┌────►│  epoll Reactor  ──► 线程池 ──► Router 处理层  │
  (curl/App)        │     │   (ET 模式 / 超时扫描 / fd 防泄漏)             │
                    │     └──────────┬───────────────┬───────────────┬──┘
                    │                │               │               │
                    │        Redis 限流/锁      MySQL 事务      MinIO 上传
                    │        (滑动窗口/        (任务落库)      (源视频对象)
                    │         分布式锁)
                    │                └───────────────┬───────────────┘
                    │                                │
                    │                   RabbitMQ (durable + confirm)
                    │                                │ 手动 ACK / DLX
                    │                                ▼
                    │     ┌─────────────────────────────────────────────┐
                    └────►│            transcode_worker (多实例)          │
                          │  消费 MQ ──► 分布式锁抢占 ──► MinIO 下载源视频   │
                          │  ──► FFmpeg(libav*) 转码 + 缩略图 ──► 回传 MinIO│
                          │  ──► MySQL 状态流转(事务) + ES 元数据同步       │
                          └─────────────────────────────────────────────┘

  中间件: MySQL · Redis · RabbitMQ · MinIO(S3) · Elasticsearch
  可观测: Prometheus 指标 (/metrics) · spdlog 日志
```

### 数据流（一次完整的转码请求）

1. **提交**：客户端 `POST /api/tasks` 携带 `multipart/form-data` 视频文件。
   - Reactor 边解析边把文件流写入临时文件（大文件不占内存）。
   - 线程池内依次：`鉴权` → `滑动窗口限流` → `幂等校验(Redis)` → `上传 MinIO` → `MySQL 事务插入 pending` → `RabbitMQ confirm 发布`。
2. **消费**：`transcode_worker` 从 MQ 拿到任务，`Redis 分布式锁` 抢占（多 worker 防重复消费），状态置 `processing`。
3. **转码**：从 MinIO 下载源视频 → libav\* 解码/缩放/重采样 → H.264 + AAC 编码 → 抽取 MJPEG 缩略图 → 回传 MinIO。
4. **收尾**：`MySQL 事务` 更新为 `success` 并写 `video_meta`，`ES` 同步元数据，`手动 ACK`。
   失败则按重试次数重投；超限 `reject(no-requeue)` 进入**死信队列**。

---

## 二、功能特性

| 模块 | 实现要点 |
| ---- | ---- |
| **网络层** | 手写 epoll（ET + EPOLLRDHUP）Reactor，非阻塞 IO + `EAGAIN` 处理；`eventfd` 跨线程唤醒、`timerfd` 定时器队列 |
| **HTTP 解析** | 手写 HTTP/1.1 解析器，**流式 multipart/form-data**（边界跨包保留、临时文件落盘、超大文件不 OOM） |
| **连接管理** | TCP 连接状态机，空闲连接超时扫描（`lastActiveTime` + 定时遍历），fd 防泄漏 |
| **并发模型** | 泛型连接池 `ConnectionPool<T>`（RAII 自动归还）+ 业务线程池（`packaged_task/future`） |
| **限流** | Redis Lua 脚本实现**滑动窗口**（ZSET + `ZREMRANGEBYSCORE`） |
| **分布式锁** | Redis Lua 脚本（`GET + DEL` 比对 owner）释放，TTL 防死锁 |
| **幂等** | `Idempotency-Key` → Redis `idem:` 键，重复提交直接返回已有 task_id |
| **存储** | MinIO（S3 协议，`useVirtualAddressing=false`），上传流式 `Aws::FStream`，预签名 URL 下载 |
| **消息队列** | RabbitMQ：durable 交换机/队列、publisher confirm、手动 ACK、QoS(prefetch=1)、DLX 死信队列 |
| **缓存** | Cache-Aside 任务状态缓存（`task:` 键，读缓存→回源→写缓存，写时失效） |
| **检索** | Elasticsearch（ES7 no-type）多字段 `multi_match` 全文检索 |
| **转码** | FFmpeg `libavformat/libavcodec/libswscale/libswresample` 直接调用（非 `system()`），H.264 + AAC + MJPEG 缩略图 |
| **可观测** | prometheus-cpp 指标（QPS / 提交 / 成功 / 失败 / 在途任务），spdlog 分级日志 |

---

## 三、目录结构

```
.
├── CMakeLists.txt            # CMake 构建脚本
├── config.json               # 服务配置（服务器/中间件/worker）
├── sql/init.sql              # MySQL 建库建表脚本
├── include/                  # 头文件
│   ├── reactor/              #   手写网络库：event_loop / tcp_connection / epoller / http_parser / http_server / router / http_response / buffer
│   ├── pool/                 #   线程池 / 泛型连接池
│   ├── redis/                #   滑动窗口限流 / 分布式锁
│   ├── service/              #   业务层：auth / task / db / storage / mq / es / service_registry / types
│   ├── ffmpeg/               #   FFmpeg libav* 封装（转码 + 缩略图）
│   ├── metrics/              #   Prometheus 指标
│   ├── api_server/           #   HTTP 路由注册与回调
│   └── common/               #   配置加载 / 日志 / uuid / noncopyable
└── src/                      # 实现（与 include 一一对应）+ 两个 main
    ├── api_server/main.cpp   # HTTP 服务进程入口
    └── worker/main.cpp       # 转码 Worker 进程入口
```

---

## 四、依赖安装（Ubuntu 22.04）

```bash
# 1. 基础编译工具
sudo apt update && sudo apt install -y build-essential cmake git pkg-config

# 2. FFmpeg 底层 C 库（转码）
sudo apt install -y libavformat-dev libavcodec-dev libavutil-dev \
                    libswscale-dev libswresample-dev

# 3. OpenSSL / Boost / 通用库
sudo apt install -y libssl-dev libboost-system-dev libcurl4-openssl-dev \
                    libspdlog-dev nlohmann-json3-dev libhiredis-dev libamqpcpp-dev
```

### 源码构建的库（无官方 apt 包或需要指定版本）

```bash
# prometheus-cpp（指标）
git clone https://github.com/jupp0r/prometheus-cpp.git && cd prometheus-cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DENABLE_TESTING=OFF .
sudo cmake --build build --target install

# redis-plus-plus（Redis 客户端，依赖 hiredis 已装）
git clone https://github.com/sewenew/redis-plus-plus.git && cd redis-plus-plus
cmake -B build -DCMAKE_BUILD_TYPE=Release -DREDIS_PLUS_PLUS_CXX_STANDARD=17 .
sudo cmake --build build --target install

# cpr（elasticlient 的 HTTP 依赖）
git clone https://github.com/libcpr/cpr.git && cd cpr
cmake -B build -DCPR_USE_SYSTEM_CURL=ON .
sudo cmake --build build --target install

# elasticlient（ES 客户端，header-only）
git clone https://github.com/seznam/elasticlient.git && cd elasticlient
sudo cmake --install build --prefix /usr/local

# aws-cpp-sdk（MinIO S3，仅需 s3 模块以缩短编译时间）
git clone https://github.com/aws/aws-sdk-cpp.git && cd aws-sdk-cpp
cmake -B build -DBUILD_ONLY=s3 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON .
sudo cmake --build build --target install

# MySQL Connector/C++（或使用发行版 libmysqlcppconn-dev）
sudo apt install -y libmysqlcppconn-dev
```

> 若某些库 `find_package` 找不到，可在 [CMakeLists.txt](CMakeLists.txt) 的「中间件客户端库」段落中
> 手动替换为对应 `target_include_directories` + 库名（见文件内注释）。

---

## 五、启动中间件（Docker）

```bash
docker run -d --name mysql -e MYSQL_ROOT_PASSWORD=root -e MYSQL_DATABASE=transcode \
  -e MYSQL_USER=transcode -e MYSQL_PASSWORD=transcode123 -p 3306:3306 mysql:8.0

docker run -d --name redis -p 6379:6379 redis:7

docker run -d --name rabbitmq -p 5672:5672 -p 15672:15672 rabbitmq:3-management

docker run -d --name minio -p 9000:9000 -p 9001:9001 \
  -e MINIO_ROOT_USER=minioadmin -e MINIO_ROOT_PASSWORD=minioadmin \
  minio/minio server /data --console-address ":9001"

docker run -d --name elasticsearch -p 9200:9200 \
  -e "discovery.type=single-node" -e "xpack.security.enabled=false" \
  -e "ES_JAVA_OPTS=-Xms512m -Xmx512m" elasticsearch:7.17.0
```

---

## 六、构建与运行

```bash
# 1. 初始化数据库
mysql -uroot -p < sql/create_user.sql
mysql -uroot -p < sql/init.sql

# 2. 构建
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build -j$(nproc)

# 3. 启动 API 服务（默认读取 config.json）
./build/api_server config.json &

# 4. 启动转码 Worker（可多开模拟分布式消费）
./build/transcode_worker config.json &
```

MinIO 需要预先创建桶（与 `config.json` 一致）：`source-videos`、`output-videos`。
可在 MinIO 控制台（`http://localhost:9001`）或使用 `mc mb` 创建。

---

## 七、HTTP 接口示例（curl）

```bash
BASE=http://localhost:8080

# 1. 健康检查
curl $BASE/healthz

# 2. 注册
curl -X POST $BASE/api/register \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"123456"}'

# 3. 登录（返回 token）
TOKEN=$(curl -s -X POST $BASE/api/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"123456"}' | jq -r .token)

# 4. 提交转码任务（multipart 上传视频；支持幂等键）
TASK=$(curl -s -X POST $BASE/api/tasks \
  -H "Authorization: Bearer $TOKEN" \
  -H "Idempotency-Key: $(uuidgen)" \
  -F "file=@./test.mp4" \
  -F "resolution=1280x720" \
  -F "bitrate=1500000" \
  -F "video_name=my_first_video" \
  -F "tag=travel" | jq -r .task_id)

# 5. 查询任务状态
curl -s "$BASE/api/tasks?task_id=$TASK" -H "Authorization: Bearer $TOKEN" | jq

# 6. 获取产物预签名下载 URL（success 后可用）
curl -s "$BASE/api/tasks/presign?task_id=$TASK" -H "Authorization: Bearer $TOKEN" | jq

# 7. 全文检索（按名称/标签匹配）
curl -s "$BASE/api/search?q=travel" -H "Authorization: Bearer $TOKEN" | jq

# 8. Prometheus 指标
curl $BASE/metrics
```

### 接口一览

| 方法 | 路径 | 说明 | 鉴权 |
| ---- | ---- | ---- | ---- |
| GET  | `/healthz` | 健康检查 | 否 |
| GET  | `/metrics` | Prometheus 指标 | 否 |
| POST | `/api/register` | 注册 | 否 |
| POST | `/api/login` | 登录，返回 token | 否 |
| POST | `/api/logout` | 登出 | 是 |
| POST | `/api/tasks` | 提交转码任务（multipart） | 是 |
| GET  | `/api/tasks?task_id=` | 查询任务状态 | 是 |
| GET  | `/api/tasks/presign?task_id=` | 产物预签名 URL | 是 |
| GET  | `/api/search?q=` | 全文检索 | 是 |

**任务状态机**：`pending → processing → success / failed`（失败会按 `worker.max_retry` 重试，超限进入死信队列）。

---

## 八、核心设计说明

- **Reactor 模型**：`EventLoop` 统一管理两类 fd —— TCP 连接（`connections_`）与外部 fd（监听套接字、
  `eventfd`、`timerfd`）。`HttpServer` 的解析回调将**整个请求**投递到业务线程池执行，epoll 主循环即刻回归，
  使阻塞的 DB/MQ/S3 调用不拖累网络线程；响应在池线程内 `send + shutdown`。
- **流式上传**：multipart 解析器对文件 part 边读边写临时文件 `/tmp/transcode_upload_<uuid>.part`，
  处理完用 `Aws::FStream` 流式上传 MinIO，内存占用与文件大小无关。
- **可靠消息**：交换机/队列均 durable，发布走 confirm（`future.get()` 阻塞确认），消费 `prefetch=1` +
  手动 ACK；业务失败 `reject(no-requeue)` 落入 DLX 死信队列供排查。
- **一致性**：任务落库用 `RAII Transaction` 包裹，异常自动回滚；转码成功后的状态更新 + 元数据写入在同一事务内完成。
- **缓存与幂等**：任务状态查询走 Cache-Aside，状态变更即失效缓存；提交侧以 `Idempotency-Key` 保证重试不产生重复任务。

---

## 九、可配置项（config.json）

- `server`：监听地址/端口、线程池大小、最大连接数、连接超时、最大上传字节。
- `mysql` / `redis` / `rabbitmq` / `minio` / `elasticsearch`：各中间件连接参数。
- `worker`：最大重试次数、默认码率、消费并发。
- `metrics`：指标服务（`/metrics` 随 API 服务同端口暴露）。

按需修改后重启对应进程即可。
