#include "db.h"
#include "offlinemessagemodel.hpp"

// 存储用户离线消息
void OfflineMessageModel::insert(int userid, string msg) {
  MySQL mysql;
  if (!mysql.connect()) return;

  string safe = mysql.escape(msg);
  // 为了能放下较大消息,给一个较大的缓冲
  string sql;
  sql.reserve(safe.size() + 64);
  sql.append("insert into offlinemessage values('")
      .append(std::to_string(userid))
      .append("', '")
      .append(safe)
      .append("')");

  mysql.update(sql);
}

// 删除用户离线消息
void OfflineMessageModel::remove(int userid) {
  // 1.组装sql语句
  char sql[1024] = {0};
  snprintf(sql, sizeof(sql), "delete from offlinemessage where userid=%d",
           userid);

  MySQL mysql;
  if (mysql.connect()) {
    mysql.update(sql);
  }
}

// 查询用户离线消息
vector<string> OfflineMessageModel::query(int userid) {
  // 1.组装sql语句
  char sql[1024] = {0};
  snprintf(sql, sizeof(sql),
           "select message from offlinemessage where userid = %d", userid);
  vector<string> vec;
  MySQL mysql;
  if (mysql.connect()) {
    MYSQL_RES *res = mysql.query(sql);
    if (res != nullptr) {
      MYSQL_ROW row;
      while ((row = mysql_fetch_row(res)) != nullptr) {
        vec.push_back(row[0]);
      }
      mysql_free_result(res);
      return vec;
    }
  }

  return vec;
}
