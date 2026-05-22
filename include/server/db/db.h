#ifndef DB_H
#define DB_H

#include <mysql/mysql.h>

#include <string>
using namespace std;

// 数据库操作类
class MySQL {
 public:
  // 初始化数据库连接
  MySQL();
  // 释放数据库连接资源
  ~MySQL();
  // 连接数据库
  bool connect();
  // 更新操作
  bool update(string sql);
  // 查询操作
  MYSQL_RES *query(string sql);
  // 获取连接
  MYSQL *getConnection();
  // 对字符串做 mysql_real_escape_string,供拼 SQL 时使用
  // 必须在 connect() 成功后调用
  string escape(const string &in);

 private:
  MYSQL *_conn;
};
#endif
