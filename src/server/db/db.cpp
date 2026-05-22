#include "db.h"

#include <muduo/base/Logging.h>

#include <cstdlib>
#include <iostream>

// 从环境变量读,缺省回退到 def
static string envOr(const char *key, const char *def) {
  const char *v = std::getenv(key);
  return (v != nullptr && *v != '\0') ? string(v) : string(def);
}

// 初始化数据库连接
MySQL::MySQL() { _conn = mysql_init(nullptr); }

// 释放数据库连接资源
MySQL::~MySQL() {
  if (_conn != nullptr) mysql_close(_conn);
}

// 连接数据库
bool MySQL::connect() {
  // 数据库配置由环境变量提供,默认 password 为空以强制显式配置
  string server = envOr("CHAT_MYSQL_HOST", "127.0.0.1");
  string user = envOr("CHAT_MYSQL_USER", "root");
  string password = envOr("CHAT_MYSQL_PASSWORD", "");
  string dbname = envOr("CHAT_MYSQL_DB", "chat");
  unsigned int port =
      static_cast<unsigned int>(std::atoi(envOr("CHAT_MYSQL_PORT", "3306").c_str()));

  MYSQL *p =
      mysql_real_connect(_conn, server.c_str(), user.c_str(), password.c_str(),
                         dbname.c_str(), port, nullptr, 0);
  if (p != nullptr) {
    // 使用 utf8mb4,正确存储 JSON / 中文 / emoji
    mysql_query(_conn, "set names utf8mb4");
    LOG_INFO << "connect mysql success!";
  } else {
    LOG_INFO << "connect mysql fail: " << mysql_error(_conn);
  }

  return p;
}

// 更新操作
bool MySQL::update(string sql) {
  if (mysql_query(_conn, sql.c_str())) {
    LOG_ERROR << __FILE__ << ":" << __LINE__ << ":" << sql << " 更新失败: "
              << mysql_error(_conn);
    return false;
  }

  return true;
}

// 查询操作
MYSQL_RES *MySQL::query(string sql) {
  if (mysql_query(_conn, sql.c_str())) {
    LOG_ERROR << __FILE__ << ":" << __LINE__ << ":" << sql << " 查询失败: "
              << mysql_error(_conn);
    return nullptr;
  }

  return mysql_use_result(_conn);
}

// 获取连接
MYSQL *MySQL::getConnection() { return _conn; }

// 转义字符串,防 SQL 注入
string MySQL::escape(const string &in) {
  if (_conn == nullptr || in.empty()) return string();
  // mysql_real_escape_string 输出最多为 2*len+1 字节
  string out;
  out.resize(in.size() * 2 + 1);
  unsigned long n = mysql_real_escape_string(_conn, &out[0], in.data(),
                                             static_cast<unsigned long>(in.size()));
  out.resize(n);
  return out;
}
