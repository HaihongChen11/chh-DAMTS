#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <amqpcpp.h>
#include <amqpcpp/reliable.h>

#include "common/config.h"
#include "common/noncopyable.h"

namespace boost { namespace asio { class io_context; } }
namespace AMQP { class LibBoostAsioHandler; class TcpConnection; class TcpChannel; }

namespace transcode {

struct TaskMessage;

// RabbitMQ 客户端封装（amqp-cpp + boost asio，独立线程运行事件循环）。
// 注意：这里的事件循环只用于「MQ 客户端」，HTTP 服务器的 epoll 仍为手写，二者互不影响。
//
// 可靠性设计：
//   - 消息持久化（durable exchange/queue + 持久化消息）
//   - 生产者 confirm：发布后等待 broker 确认
//   - 消费者手动 ACK：处理成功才 ack
//   - 死信队列 DLX：失败次数超限 reject（不重投）进入死信队列
class MqService : Noncopyable {
public:
    using ConsumeCallback = std::function<void(const TaskMessage&, uint64_t deliveryTag)>;

    explicit MqService(const Config::RabbitMq& cfg);
    ~MqService();

    bool connect();
    void stop();

    // 发布任务消息（生产者 confirm，阻塞等待确认结果）
    bool publish(const TaskMessage& msg);
    // 开始消费，收到消息后回调（回调中做业务，完成后手动 ack）
    void startConsume(ConsumeCallback cb);

    void ack(uint64_t deliveryTag);
    // 拒绝消息且不重投 -> 路由到死信队列
    void rejectToDeadLetter(uint64_t deliveryTag);

private:
    void setupTopology();

    Config::RabbitMq cfg_;
    std::unique_ptr<boost::asio::io_context> io_;
    std::unique_ptr<AMQP::LibBoostAsioHandler> handler_;
    std::unique_ptr<AMQP::TcpConnection> connection_;
    std::unique_ptr<AMQP::TcpChannel> channel_;
    std::unique_ptr<AMQP::Reliable<AMQP::Tagger>> publisher_;
    std::thread thread_;
    ConsumeCallback consumeCallback_;
    bool connected_ = false;
};

} // namespace transcode
