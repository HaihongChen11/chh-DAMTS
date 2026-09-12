-- =============================================================
-- 分布式多媒体异步转码服务 数据库初始化脚本 (MySQL 8.0)
-- 用法: mysql -uroot -p < sql/init.sql
-- =============================================================

CREATE DATABASE IF NOT EXISTS transcode
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_unicode_ci;

USE transcode;

-- 用户表
CREATE TABLE IF NOT EXISTS `user` (
    `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '自增主键',
    `username`      VARCHAR(64)     NOT NULL COMMENT '用户名',
    `password_salt` CHAR(32)        NOT NULL COMMENT '随机盐 (hex)',
    `password_hash` CHAR(64)        NOT NULL COMMENT 'SHA256(salt + password) hex',
    `create_time`   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_username` (`username`)
) ENGINE = InnoDB COMMENT = '用户表';

-- 转码任务表
CREATE TABLE IF NOT EXISTS `transcode_task` (
    `task_id`       CHAR(36)        NOT NULL COMMENT '任务 ID (uuid) 主键',
    `user_id`       BIGINT UNSIGNED NOT NULL COMMENT '提交用户',
    `source_bucket` VARCHAR(128)    NOT NULL COMMENT '源视频所在桶',
    `source_key`    VARCHAR(512)    NOT NULL COMMENT '源视频对象 key',
    `output_bucket` VARCHAR(128)    NOT NULL COMMENT '产物所在桶',
    `output_key`    VARCHAR(512)    DEFAULT NULL COMMENT '产物对象 key',
    `thumbnail_key` VARCHAR(512)    DEFAULT NULL COMMENT '缩略图对象 key',
    `resolution`    VARCHAR(32)     NOT NULL DEFAULT '1280x720' COMMENT '目标分辨率',
    `bitrate`       INT             NOT NULL DEFAULT 1500000 COMMENT '目标码率 bps',
    `status`        ENUM('pending','processing','success','failed') NOT NULL DEFAULT 'pending',
    `retry_count`   INT             NOT NULL DEFAULT 0 COMMENT '已重试次数',
    `error_msg`     VARCHAR(1024)   DEFAULT NULL COMMENT '失败原因',
    `create_time`   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `finish_time`   DATETIME        DEFAULT NULL,
    PRIMARY KEY (`task_id`),
    KEY `idx_user_id` (`user_id`),
    KEY `idx_status` (`status`)
) ENGINE = InnoDB COMMENT = '转码任务表';

-- 视频元数据表 (用于 ES 同步源 + 检索兜底)
CREATE TABLE IF NOT EXISTS `video_meta` (
    `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `task_id`       CHAR(36)        NOT NULL COMMENT '关联任务',
    `video_name`    VARCHAR(256)    NOT NULL COMMENT '视频名称',
    `tag`           VARCHAR(256)    DEFAULT NULL COMMENT '标签/关键词',
    `duration`      DOUBLE          NOT NULL DEFAULT 0 COMMENT '时长(秒)',
    `width`         INT             NOT NULL DEFAULT 0,
    `height`        INT             NOT NULL DEFAULT 0,
    `size`          BIGINT          NOT NULL DEFAULT 0 COMMENT '文件大小字节',
    `thumbnail_key` VARCHAR(512)    DEFAULT NULL,
    `create_time`   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_task_id` (`task_id`)
) ENGINE = InnoDB COMMENT = '视频元数据表';
