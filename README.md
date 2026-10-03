# 分布式多媒体异步转码处理服务

基于 C++17 的分布式视频异步转码后端，采用「HTTP API 网关 + 转码 Worker」双进程架构。客户端通过 HTTP 上传视频并提交任务，API 服务将任务投递到 RabbitMQ，由多个 Worker 异步消费并调用 FFmpeg 完成转码与缩略图。

核心能力：

- 手写 epoll Reactor 网络层
- 流式 multipart 大文件上传
- Redis 限流、幂等、分布式锁
- RabbitMQ 可靠消息、死信队列
- MinIO 对象存储与客户端直传
- MySQL 事务、Elasticsearch 检索
- FFmpeg 分片并行转码
- Web 操作台、Prometheus 指标

---

## 一、整体架构

```text
客户端 / 浏览器
    │ HTTP/2
    ▼
Nginx（反向代理 + 负载均衡）
    │ HTTP/1.1
    ▼
api_server（多 Reactor + 线程池）
    ├─ 鉴权、限流、幂等
    ├─ MinIO 直传 / 上传
    ├─ MySQL 落库
    └─ RabbitMQ 发布
         │
         ▼
RabbitMQ（durable + confirm + 手动 ACK + DLX）
         │
         ▼
transcode_worker × N
    ├─ Redis 分布式锁
    ├─ MinIO 下载源视频
    ├─ FFmpeg 分片并行转码 + 缩略图
    └─ 回传 MinIO，更新 MySQL，同步 ES
```

Nginx 支持：

- 反向代理
- 多实例负载均衡
- HTTP/2 接入
- TLS 终止
- 静态资源服务



任务状态机：

```text
pending → processing → success / failed
```

## 一、详细数据流

### 提交任务流程

1. 客户端向 `api_server` 发起 `POST /api/tasks`，使用 `multipart/form-data` 上传视频。
2. Reactor 线程接收 TCP 数据，交给 HTTP 解析器。
3. HTTP 解析器边接收边把文件写入 `/tmp/transcode_upload_<uuid>.part`，避免大文件占满内存。
4. 请求解析完成后，投递到业务线程池。
5. 业务线程依次执行：
   - 鉴权：从 `Authorization: Bearer <token>` 获取 token，Redis 查询用户 ID。
   - 限流：Redis Lua 滑动窗口限流。
   - 幂等：检查 Redis `idem:<Idempotency-Key>`。
   - 上传：源视频流式上传 MinIO `source-videos`。
   - 落库：MySQL 事务插入 `transcode_task`，状态为 `pending`。
   - 投递：RabbitMQ 发布任务消息，等待 publisher confirm。
6. 返回 `task_id` 给客户端。

### 直传流程

1. 客户端请求 `GET /api/presign-upload?filename=xxx`。
2. `api_server` 返回 MinIO 预签名上传 URL 和 `source_key`。
3. 客户端直接 `PUT` 文件到 MinIO。
4. 客户端请求 `POST /api/tasks/direct`，提交 `source_key` 创建任务。
5. 后续流程与普通提交一致。

### Worker 转码流程

1. Worker 从 RabbitMQ 消费任务消息。
2. 使用 Redis 分布式锁抢占 `lock:task:<task_id>`，防止重复消费。
3. MySQL 更新任务状态为 `processing`。
4. 从 MinIO 下载源视频到本地。
5. FFmpeg 按配置分片并行转码：
   - 源文件复制到 `/dev/shm`。
   - 按时间切成 N 段。
   - 多线程并行转码。
   - 合并分片为最终 MP4。
6. 抽取缩略图并上传 MinIO。
7. 上传转码产物到 MinIO。
8. MySQL 事务更新任务为 `success`，写入 `video_meta`。
9. Elasticsearch 同步元数据。
10. RabbitMQ 手动 ACK。

### 失败流程

1. 转码失败时，`retry_count + 1`。
2. 未超过最大重试次数：重新发布任务消息。
3. 超过最大重试次数：更新任务为 `failed`，消息 `reject` 进入死信队列。

### 查询流程

1. 客户端请求 `GET /api/tasks?task_id=xxx`。
2. 先查 Redis `task:<task_id>` 缓存。
3. 缓存命中直接返回。
4. 缓存未命中查 MySQL，回写 Redis。
5. 任务成功后，可请求 `GET /api/tasks/presign` 获取下载 URL。

---

## 二、功能特性

| 模块 | 说明 |
| ---- | ---- |
| 网络层 | 主从多 Reactor、epoll ET、Keep-Alive |
| HTTP | 手写 HTTP/1.1 解析、multipart 流式上传 |
| 并发 | 有界线程池、通用连接池、连接超时 |
| 鉴权 | PBKDF2 密码哈希、token 会话 |
| 限流 | Redis 滑动窗口 |
| 幂等 | Idempotency-Key |
| 分布式锁 | Redis SET NX PX + Lua 释放 |
| 消息队列 | RabbitMQ confirm、手动 ACK、死信队列 |
| 对象存储 | MinIO、Multipart Upload、客户端直传 |
| 数据库 | MySQL 事务、Cache-Aside |
| 检索 | Elasticsearch multi_match |
| 转码 | FFmpeg 分片并行、缩略图 |
| 可观测 | trace_id、Prometheus、spdlog |
| 部署 | Dockerfile、Compose、K8s YAML、CI |
| 接入层 | Nginx 反向代理、负载均衡、HTTP/2 |

---

## 三、目录结构

```text
.
├── CMakeLists.txt
├── Dockerfile
├── docker-compose.yml
├── config.json
├── README.md
├── sql/
│   ├── init.sql
│   └── create_user.sql
├── include/
│   ├── reactor/
│   ├── pool/
│   ├── redis/
│   ├── service/
│   ├── ffmpeg/
│   ├── metrics/
│   ├── api_server/
│   └── common/
├── src/
│   ├── api_server/
│   ├── worker/
│   └── ...
├── tests/
│   └── unit_tests.cpp
├── web/
│   └── index.html
└── deploy/
    ├── nginx.conf
    ├── k8s.yaml
    ├── prometheus.yml
    ├── alert.rules.yml
    ├── redis-sentinel.conf
    ├── redis-sentinel-cluster.yml
    └── opentelemetry-collector.yml
```

## 三、模块详解与代码位置

| 模块 | 关键文件 | 作用 |
| ---- | ---- | ---- |
| 事件循环 | `src/reactor/event_loop.cpp` | epoll 事件分发、定时器、跨线程唤醒 |
| TCP 连接 | `src/reactor/tcp_connection.cpp` | 连接状态机、读写缓冲、半关闭 |
| HTTP 解析 | `src/reactor/http_parser.cpp` | HTTP/1.1 与 multipart 流式解析 |
| HTTP 服务 | `src/reactor/http_server.cpp` | 监听、accept、多 Reactor 分发 |
| 线程池 | `src/pool/thread_pool.cpp` | 有界队列、异常隔离 |
| 连接池 | `include/pool/connection_pool.h` | 连接复用、超时等待 |
| 鉴权 | `src/service/auth_service.cpp` | PBKDF2、token |
| 限流 | `src/redis/rate_limiter.cpp` | 滑动窗口 |
| 分布式锁 | `src/redis/distributed_lock.cpp` | SET NX PX + Lua |
| 消息队列 | `src/service/mq_service.cpp` | confirm、ACK、DLX |
| 对象存储 | `src/service/storage_service.cpp` | MinIO、Multipart、直传 |
| 任务服务 | `src/service/task_service.cpp` | 提交、查询、状态流转 |
| 检索 | `src/service/es_service.cpp` | ES 索引与搜索 |
| 转码 | `src/ffmpeg/ffmpeg_helper.cpp` | 分片转码、缩略图 |
| 可观测 | `src/metrics/metrics.cpp` | Prometheus 指标 |

---

## 四、依赖安装

Ubuntu 22.04：

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libssl-dev libboost-system-dev libcurl4-openssl-dev libspdlog-dev \
  nlohmann-json3-dev libhiredis-dev libmysqlcppconn-dev libjsoncpp-dev
```

源码构建依赖：AMQP-CPP、redis-plus-plus、cpr、elasticlient、prometheus-cpp、AWS SDK C++。

---

## 五、启动中间件

Docker Compose 已包含 MySQL、Redis、RabbitMQ、MinIO、Elasticsearch、Prometheus、Grafana、Jaeger。

MinIO 需要创建桶：

```bash
mc alias set local http://127.0.0.1:9000 minioadmin minioadmin
mc mb -p local/source-videos local/output-videos
```

MySQL 初始化：

```bash
mysql -uroot -p < sql/create_user.sql
mysql -uroot -p transcode < sql/init.sql
```

---

## 六、构建与运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build -j$(nproc)

./build/unit_tests

./build/api_server config.json &
./build/transcode_worker config.json &
```

敏感配置可用环境变量覆盖：

```bash
export TRANSCODE_MYSQL_PASSWORD=xxx
export TRANSCODE_REDIS_PASSWORD=xxx
export TRANSCODE_RABBITMQ_PASSWORD=xxx
export TRANSCODE_MINIO_SECRET_KEY=xxx
```

---

## 七、Web 操作台

浏览器访问：

```text
http://localhost:8080/
```

支持注册登录、上传视频、提交转码、进度查看、下载链接、全文检索。

---

## 八、HTTP 接口

| 方法 | 路径 | 说明 | 鉴权 |
| ---- | ---- | ---- | ---- |
| GET | `/healthz` | 健康检查 | 否 |
| GET | `/readyz` | 依赖就绪检查 | 否 |
| GET | `/metrics` | Prometheus 指标 | 否 |
| POST | `/api/register` | 注册 | 否 |
| POST | `/api/login` | 登录 | 否 |
| POST | `/api/logout` | 登出 | 是 |
| POST | `/api/tasks` | multipart 提交任务 | 是 |
| POST | `/api/tasks/direct` | 直传后创建任务 | 是 |
| GET | `/api/presign-upload` | 获取上传预签名 URL | 是 |
| GET | `/api/tasks` | 查询任务状态 | 是 |
| GET | `/api/tasks/presign` | 获取下载 URL | 是 |
| GET | `/api/search` | 全文检索 | 是 |

---

## 九、配置说明

`config.json` 主要字段：

- `server`：监听地址、端口、线程池大小
- `mysql`：数据库连接
- `redis`：缓存、限流、锁
- `rabbitmq`：交换机、队列、死信队列
- `minio`：对象存储桶
- `elasticsearch`：检索服务
- `worker`：重试次数、默认码率、分片数

---

## 十、性能总结

### HTTP 层

| 指标 | 结果 |
| ---- | ---- |
| 最佳长连接并发 | 600 |
| 最佳长连接 QPS | 约 26186 |
| 1000 长连接 QPS | 约 21809 |
| 最大已测并发 | 20000，失败 0 |
| 20000 并发 QPS | 约 3322 |
| api_server 空闲内存 | 约 36.7MB |
| api_server 20000 并发内存 | 约 94MB |

### 

### 多机部署估算

| 部署 | 估算 QPS |
| ---- | ---: |
| 1 台 `api_server` 直连 | 约 26000 |
| 3 台 `api_server` + Nginx | 约 60000～70000 |
| 5 台 `api_server` + Nginx | 约 90000～120000 |

### 转码层估算

| 部署 | 估算吞吐 |
| ---- | ---: |
| 1 台机器 4 Worker | 约 1.6 任务/秒 |
| 3 台机器，每台 4 Worker | 约 4.8 任务/秒 |
| 5 台机器，每台 4 Worker | 约 8 任务/秒 |



### 瓶颈

当前主要瓶颈是 FFmpeg CPU 编解码，其次是 Worker 内存和 MinIO 带宽。HTTP 层不是瓶颈。

---

## 十一、测试

已覆盖：

- 功能：注册登录、鉴权、限流、幂等、上传、状态流转、转码、下载、检索
- 性能：HTTP QPS、并发上传、转码吞吐、MQ 积压
- 可靠性：死信队列、Redis 降级、分布式锁、MySQL 事务、Worker 崩溃恢复
- 安全：越权、SQL 注入、token、文件上传
- 单元测试：UUID、Buffer、线程池、HTTP 解析

---

## 十二、部署

- `Dockerfile`
- `docker-compose.yml`
- `deploy/k8s.yaml`
- `deploy/nginx.conf`
- `.github/workflows/ci.yml`

### Nginx 多实例负载均衡

生产环境可将多个 `api_server` 实例放在 Nginx 后面：

```nginx
upstream transcode_api {
    server 192.168.1.10:8080;
    server 192.168.1.11:8080;
    server 192.168.1.12:8080;
}

server {
    listen 443 ssl http2;
    server_name example.com;

    ssl_certificate     /etc/nginx/certs/server.crt;
    ssl_certificate_key /etc/nginx/certs/server.key;

    location / {
        proxy_pass http://transcode_api;
        proxy_http_version 1.0;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
```

本地模拟时，可使用 `network_mode: host` 。

---

## 十三、当前水平

当前项目是“完整可演示、可容器化、具备工业级加固雏形”的工程原型。距离生产级还差真实 K8s 集群、Redis Sentinel 接入、OpenTelemetry SDK、GPU 硬件转码和完整集成测试。
