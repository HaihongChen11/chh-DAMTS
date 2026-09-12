#pragma once

#include <memory>
#include <string>

#include <cppconn/connection.h>

#include "common/config.h"
#include "common/noncopyable.h"
#include "pool/connection_pool.h"

namespace transcode {

// MySQL 连接池：基于通用 ConnectionPool 模板封装 sql::Connection。
// 减少频繁创建/销毁 TCP 连接的三次握手与鉴权开销，并控制最大连接数。
class MysqlPool : Noncopyable {
public:
    using ConnPtr = std::shared_ptr<sql::Connection>;

    explicit MysqlPool(const Config::Mysql& cfg);
    ConnPtr acquire();

private:
    std::shared_ptr<ConnectionPool<sql::Connection>> pool_;
};

// 事务 RAII 封装：构造时 acquire 连接并关闭自动提交，析构时未提交则回滚。
// 用于保障任务状态流转的原子性。
class Transaction : Noncopyable {
public:
    explicit Transaction(MysqlPool* pool);
    ~Transaction();

    sql::Connection* conn() const { return conn_.get(); }
    void commit();
    void rollback();
    bool done() const { return done_; }

private:
    MysqlPool::ConnPtr conn_;
    bool done_ = false;
};

} // namespace transcode
