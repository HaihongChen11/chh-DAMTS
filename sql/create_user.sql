CREATE USER IF NOT EXISTS 'transcode'@'localhost' IDENTIFIED BY 'transcode123';
CREATE USER IF NOT EXISTS 'transcode'@'127.0.0.1' IDENTIFIED BY 'transcode123';
GRANT ALL PRIVILEGES ON transcode.* TO 'transcode'@'localhost';
GRANT ALL PRIVILEGES ON transcode.* TO 'transcode'@'127.0.0.1';
FLUSH PRIVILEGES;
