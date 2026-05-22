#include "usermodel.hpp"

#include <iostream>

#include "db.h"
using namespace std;

// user表的增加方法
bool UserModel::insert(User &user) {
  MySQL mysql;
  if (!mysql.connect()) return false;

  // 字符串经 escape 后再拼 SQL,整型直接走格式化
  string name = mysql.escape(user.getName());
  string password = mysql.escape(user.getPassword());
  string state = mysql.escape(user.getState());

  char sql[4096] = {0};
  snprintf(sql, sizeof(sql),
           "insert into user(name, password, state) values('%s', '%s', '%s')",
           name.c_str(), password.c_str(), state.c_str());

  if (mysql.update(sql)) {
    user.setId(mysql_insert_id(mysql.getConnection()));
    return true;
  }
  return false;
}

User UserModel::query(int id) {
  // 1.组装sql语句
  char sql[1024] = {0};
  snprintf(sql, sizeof(sql), "select * from user where id = %d", id);

  MySQL mysql;
  if (mysql.connect()) {
    MYSQL_RES *res = mysql.query(sql);
    if (res != nullptr) {
      MYSQL_ROW row = mysql_fetch_row(res);
      if (row != nullptr) {
        User user;
        user.setId(atoi(row[0]));
        user.setName(row[1]);
        user.setPassword(row[2]);
        user.setState(row[3]);

        mysql_free_result(res);
        return user;
      }
      mysql_free_result(res);
    }
  }
  return User();
}
// 更新用户状态信息
bool UserModel::updateState(User user) {
  MySQL mysql;
  if (!mysql.connect()) return false;

  string state = mysql.escape(user.getState());

  char sql[1024] = {0};
  snprintf(sql, sizeof(sql), "update user set state = '%s' where id = %d",
           state.c_str(), user.getId());

  return mysql.update(sql);
}

// 重置用户状态信息
void UserModel::resetState() {
  // 1.组装sql语句
  const char *sql = "update user set state = 'offline'";

  MySQL mysql;
  if (mysql.connect()) {
    mysql.update(sql);
  }
}
