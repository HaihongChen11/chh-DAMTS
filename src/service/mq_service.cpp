#include "service/mq_service.h"

#include <future>

#include <amqpcpp.h>
#include <amqpcpp/linux_tcp.h>
#include <amqpcpp/libboostasio.h>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "common/logger.h"
#include "service/types.h"

namespace transcode {

MqService::MqService(const Config::RabbitMq& cfg) : cfg_(cfg) {
    io_ = std::make_unique<boost::asio::io_context>();
}

MqService::~MqService() {
    stop();
}

bool MqService::connect() {
    try {
        handler_ = std::make_unique<AMQP::LibBoostAsioHandler>(*io_);
        AMQP::Address address(cfg_.host, cfg_.port,
                              AMQP::Login(cfg_.user, cfg_.password), cfg_.vhost);
        connection_ = std::make_unique<AMQP::TcpConnection>(handler_.get(), address);
        channel_ = std::make_unique<AMQP::TcpChannel>(connection_.get());

        // Reliable<Tagger> 会为 channel 开启 publisher confirm，并跟踪每个
        // delivery tag，通过 DeferredPublish 回调通知发布结果。
        publisher_ = std::make_unique<AMQP::Reliable<AMQP::Tagger>>(*channel_);
        publisher_->onError([this](const char* message) {
            connected_ = false;
            LOG_ERROR("rabbitmq publisher error: {}", message);
        });
        connected_ = true;
        LOG_INFO("rabbitmq connected and confirm enabled");

        setupTopology();
        thread_ = std::thread([this]() { io_->run(); });
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("rabbitmq connect failed: {}", e.what());
        return false;
    }
}

void MqService::setupTopology() {
    // 死信交换机 + 死信队列
    channel_->declareExchange(cfg_.dead_letter_exchange, AMQP::direct, AMQP::durable);
    channel_->declareQueue(cfg_.dead_letter_queue, AMQP::durable);
    channel_->bindQueue(cfg_.dead_letter_exchange, cfg_.dead_letter_queue, cfg_.dead_letter_queue);

    // 主交换机
    channel_->declareExchange(cfg_.exchange, AMQP::direct, AMQP::durable);
    // 主队列：声明时绑定死信交换机
    AMQP::Table args;
    args["x-dead-letter-exchange"] = cfg_.dead_letter_exchange;
    channel_->declareQueue(cfg_.queue, AMQP::durable, args);
    channel_->bindQueue(cfg_.exchange, cfg_.queue, cfg_.routing_key);
}

void MqService::stop() {
    if (io_) io_->stop();
    if (thread_.joinable()) thread_.join();
}

bool MqService::publish(const TaskMessage& msg) {
    auto p = std::make_shared<std::promise<bool>>();
    auto fut = p->get_future();
    std::string body = msg.toJson().dump();

    boost::asio::post(*io_, [this, p, body]() {
        // 持久化消息：delivery mode = persistent
        publisher_->publish(cfg_.exchange, cfg_.routing_key, body)
            .onAck([p]() { p->set_value(true); })
            .onNack([p]() { p->set_value(false); })
            .onError([p](const char* e) {
                LOG_ERROR("rabbitmq publish error: {}", e);
                p->set_value(false);
            });
    });
    return fut.get();
}

void MqService::startConsume(ConsumeCallback cb) {
    consumeCallback_ = std::move(cb);
    boost::asio::post(*io_, [this]() {
        channel_->setQos(cfg_.prefetch_count);
        channel_->consume(cfg_.queue)
            .onReceived([this](const AMQP::Message& message, uint64_t deliveryTag, bool /*redelivered*/) {
                std::string body(message.body(), message.bodySize());
                try {
                    TaskMessage m = TaskMessage::fromJson(nlohmann::json::parse(body));
                    if (consumeCallback_) consumeCallback_(m, deliveryTag);
                } catch (const std::exception& e) {
                    LOG_ERROR("parse task message failed: {}", e.what());
                    channel_->ack(deliveryTag); // 脏消息直接丢弃
                }
            })
            .onError([](const char* e) { LOG_ERROR("rabbitmq consume error: {}", e); });
    });
}

void MqService::ack(uint64_t deliveryTag) {
    boost::asio::post(*io_, [this, deliveryTag]() { channel_->ack(deliveryTag); });
}

void MqService::rejectToDeadLetter(uint64_t deliveryTag) {
    // 手动拒绝且不重投（requeue=false），消息路由到死信队列
    boost::asio::post(*io_, [this, deliveryTag]() { channel_->reject(deliveryTag, 0); });
}

} // namespace transcode
