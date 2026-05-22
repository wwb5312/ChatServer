#include "chatservice.hpp"

#include <arpa/inet.h>
#include <muduo/base/Logging.h>

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "password.hpp"
#include "public.hpp"
using namespace std;
using namespace muduo;

namespace {

// 把 JSON 字符串加上 4 字节大端长度前缀后发送,与 onMessage 的解帧一致
void sendFramed(const TcpConnectionPtr &conn, const string &payload) {
  int32_t len_be = htonl(static_cast<int32_t>(payload.size()));
  string frame;
  frame.reserve(sizeof(int32_t) + payload.size());
  frame.append(reinterpret_cast<const char *>(&len_be), sizeof(int32_t));
  frame.append(payload);
  conn->send(frame);
}

}  // namespace

// 获取单例对象的接口函数
ChatService *ChatService::instance() {
  static ChatService service;
  return &service;
}
// 注册消息以及对应的Handler回调操作
ChatService::ChatService() {
  _msgHandlerMap.insert(
      {LOGIN_MSG, std::bind(&ChatService::login, this, _1, _2, _3)});
  _msgHandlerMap.insert(
      {REG_MSG, std::bind(&ChatService::reg, this, _1, _2, _3)});
  _msgHandlerMap.insert(
      {ONE_CHAT_MSG, std::bind(&ChatService::oneChat, this, _1, _2, _3)});
  _msgHandlerMap.insert(
      {ADD_FRIEND_MSG, std::bind(&ChatService::addFriend, this, _1, _2, _3)});

  // 群组业务管理相关事件处理回调注册
  _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup,
                                                     this, _1, _2, _3)});
  _msgHandlerMap.insert(
      {ADD_GROUP_MSG, std::bind(&ChatService::addGroup, this, _1, _2, _3)});
  _msgHandlerMap.insert(
      {GROUP_CHAT_MSG, std::bind(&ChatService::groupChat, this, _1, _2, _3)});
  _msgHandlerMap.insert(
      {LOGINOUT_MSG, std::bind(&ChatService::loginOut, this, _1, _2, _3)});
  // 链接redis服务器
  if (_redis.connect()) {
    _redis.init_notify_handler(
        std::bind(&ChatService::handleRedisSubscribeMessage, this, _1, _2));
  }
}

// 服务器异常,业务重置方法
void ChatService::reset() { _userModel.resetState(); }

// 获取消息对应的处理器
MsgHandler ChatService::getHandler(int msgid) {
  auto it = _msgHandlerMap.find(msgid);
  if (it == _msgHandlerMap.end()) {
    return [=](const TcpConnectionPtr &conn, json &js, Timestamp time) {
      LOG_ERROR << "msgid:" << msgid << " can not find handler!";
    };
  }
  return it->second;
}

// 处理登录业务  id  pwd   pwd
void ChatService::login(const TcpConnectionPtr &conn, json &js,
                        Timestamp time) {
  int id = js.value("id", -1);
  string pwd = js.value("password", "");

  User user = _userModel.query(id);
  if (user.getId() == id &&
      pwd::verifyPassword(user.getPassword(), pwd)) {
    if (user.getState() == "online") {
      // 该用户已经登录,不允许重复登录
      json response;
      response["msgid"] = LOGIN_MSG_ACK;
      response["errno"] = 2;
      response["errmsg"] = "this account is using, input another!";
      sendFramed(conn, response.dump());
    } else {
      // 登录成功,记录用户连接信息
      {
        lock_guard<mutex> lock(_connMutex);
        _userConnMap.insert({id, conn});
      }

      // 把 userid 挂到连接上,断连时 O(1) 反查
      conn->setContext(id);

      // id用户登录成功后,向redis订阅channel(id)
      _redis.subscribe(id);
      // 登录成功,更新用户状态信息 state offline=>online
      user.setState("online");
      _userModel.updateState(user);

      json response;
      response["msgid"] = LOGIN_MSG_ACK;
      response["errno"] = 0;
      response["id"] = user.getId();
      response["name"] = user.getName();
      // 查询该用户是否有离线消息
      vector<string> vec = _offlineMsgModel.query(id);
      if (!vec.empty()) {
        response["offlinemsg"] = vec;
        _offlineMsgModel.remove(id);
      }

      // 查询该用户的好友信息并返回
      vector<User> userVec = _friendModel.query(id);
      if (!userVec.empty()) {
        vector<string> vec2;
        for (User &u : userVec) {
          json jsu;
          jsu["id"] = u.getId();
          jsu["name"] = u.getName();
          jsu["state"] = u.getState();
          vec2.push_back(jsu.dump());
        }
        response["friends"] = vec2;
      }
      // 查询用户的群组信息
      vector<Group> groupuserVec = _groupModel.queryGroups(id);
      if (!groupuserVec.empty()) {
        vector<string> groupV;
        for (Group &group : groupuserVec) {
          json grpjson;
          grpjson["id"] = group.getId();
          grpjson["groupname"] = group.getName();
          grpjson["groupdesc"] = group.getDesc();
          vector<string> userV;
          for (GroupUser &u : group.getUsers()) {
            json jsu;
            jsu["id"] = u.getId();
            jsu["name"] = u.getName();
            jsu["state"] = u.getState();
            jsu["role"] = u.getRole();
            userV.push_back(jsu.dump());
          }
          grpjson["users"] = userV;
          groupV.push_back(grpjson.dump());
        }
        response["groups"] = groupV;
      }

      sendFramed(conn, response.dump());
    }
  } else {
    // 该用户不存在,用户存在但是密码错误,登录失败
    json response;
    response["msgid"] = LOGIN_MSG_ACK;
    response["errno"] = 1;
    response["errmsg"] = "id or password is invalid!";
    sendFramed(conn, response.dump());
  }
}

// 处理注册业务 name password
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp time) {
  string name = js.value("name", "");
  string password = js.value("password", "");

  User user;
  user.setName(name);
  user.setPassword(pwd::hashPassword(password));
  bool state = _userModel.insert(user);

  json response;
  response["msgid"] = REG_MSG_ACK;
  if (state) {
    response["errno"] = 0;
    response["id"] = user.getId();
  } else {
    response["errno"] = 1;
  }
  sendFramed(conn, response.dump());
}

// 处理注销业务
void ChatService::loginOut(const TcpConnectionPtr &conn, json &js,
                           Timestamp time) {
  int userid = js.value("id", -1);
  if (userid < 0) return;

  {
    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(userid);
    if (it != _userConnMap.end()) {
      _userConnMap.erase(it);
    }
  }

  // 清连接上下文
  conn->setContext(boost::any());

  _redis.unsubscribe(userid);
  User user(userid, "", "", "offline");
  _userModel.updateState(user);
}

// 处理客户端异常退出
void ChatService::clientCloseException(const TcpConnectionPtr &conn) {
  // 未登录的连接断开,无需处理
  if (conn->getContext().empty()) return;

  int userid = -1;
  try {
    userid = boost::any_cast<int>(conn->getContext());
  } catch (const boost::bad_any_cast &) {
    return;
  }
  if (userid < 0) return;

  {
    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(userid);
    if (it != _userConnMap.end() && it->second == conn) {
      _userConnMap.erase(it);
    }
  }

  _redis.unsubscribe(userid);

  User user(userid, "", "", "offline");
  _userModel.updateState(user);
}

// 一对一聊天业务
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js,
                          Timestamp time) {
  int toid = js.value("to", -1);
  if (toid < 0) return;

  {
    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(toid);
    if (it != _userConnMap.end()) {
      sendFramed(it->second, js.dump());
      return;
    }
  }
  // 查询toid是否在线
  User user = _userModel.query(toid);
  if (user.getState() == "online") {
    _redis.publish(toid, js.dump());
    return;
  }

  // toid不在线,存储离线消息
  _offlineMsgModel.insert(toid, js.dump());
}

// 添加好友业务
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js,
                            Timestamp time) {
  int userid = js.value("id", -1);
  int friendid = js.value("friendid", -1);
  if (userid < 0 || friendid < 0) return;

  _friendModel.insert(userid, friendid);
}

// 创建群组业务
void ChatService::createGroup(const TcpConnectionPtr &conn, json &js,
                              Timestamp time) {
  int userid = js.value("id", -1);
  string name = js.value("groupname", "");
  string desc = js.value("groupdesc", "");
  if (userid < 0 || name.empty()) return;

  Group group(-1, name, desc);
  if (_groupModel.createGroup(group)) {
    _groupModel.addGroup(userid, group.getId(), "creator");
  }
}

// 加入群组业务
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js,
                           Timestamp time) {
  int userid = js.value("id", -1);
  int groupid = js.value("groupid", -1);
  if (userid < 0 || groupid < 0) return;
  _groupModel.addGroup(userid, groupid, "normal");
}

// 群组聊天业务
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js,
                            Timestamp time) {
  int userid = js.value("id", -1);
  int groupid = js.value("groupid", -1);
  if (userid < 0 || groupid < 0) return;

  vector<int> useridVec = _groupModel.queryGroupUsers(userid, groupid);

  lock_guard<mutex> lock(_connMutex);
  for (int id : useridVec) {
    auto it = _userConnMap.find(id);
    if (it != _userConnMap.end()) {
      sendFramed(it->second, js.dump());
    } else {
      User user = _userModel.query(id);
      if (user.getState() == "online") {
        _redis.publish(id, js.dump());
      } else {
        _offlineMsgModel.insert(id, js.dump());
      }
    }
  }
}

// 从redis消息队列中获取订阅的消息
void ChatService::handleRedisSubscribeMessage(int userid, string msg) {
  lock_guard<mutex> lock(_connMutex);
  auto it = _userConnMap.find(userid);
  if (it != _userConnMap.end()) {
    sendFramed(it->second, msg);
    return;
  }

  _offlineMsgModel.insert(userid, msg);
}
