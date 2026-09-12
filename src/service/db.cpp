#include "service/db.h"

#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/statement.h>
#include <mysql_driver.h>

#include "common/logger.h"

namespace transcode {

MysqlPool::MysqlPool(const Config::Mysql& cfg) {
    auto factory = [cfg]() -> std::unique_ptr<sql::Connection> {
        try {
            sql::Driver* driver = sql::mysql::get_driver_instance();
            auto conn = std::unique_ptr<sql::Connection>(
                driver->connect(cfg.host + ":" + std::to_string(cfg.port), cfg.user, cfg.password));
            conn->setSchema(cfg.database);
            return conn;
        } catch (const sql::SQLException& e) {
            LOG_ERROR("mysql connect failed: {}", e.what());
            return nullptr;
        }
    };

    auto validator = [](sql::Connection* c) -> bool {
        try {
            return c != nullptr && !c->isClosed() && c->isValid();
        } catch (...) {
            return false;
        }
    };

    pool_ = std::make_shared<ConnectionPool<sql::Connection>>(factory, cfg.pool_size, validator);
}

MysqlPool::ConnPtr MysqlPool::acquire() {
    return pool_->acquire();
}

Transaction::Transaction(MysqlPool* pool) {
    conn_ = pool->acquire();
    if (conn_) {
        conn_->setAutoCommit(false);
    }
}

Transaction::~Transaction() {
    if (!done_ && conn_) {
        try {
            conn_->rollback();
        } catch (...) {
        }
    }
    if (conn_) {
        try {
            conn_->setAutoCommit(true);
        } catch (...) {
        }
    }
}

void Transaction::commit() {
    if (conn_ && !done_) {
        conn_->commit();
        done_ = true;
    }
}

void Transaction::rollback() {
    if (conn_ && !done_) {
        conn_->rollback();
        done_ = true;
    }
}

} // namespace transcode
