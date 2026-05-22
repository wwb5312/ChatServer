#include "chatserver.hpp"

#include <muduo/base/Logging.h>

#include <functional>
#include <string>

#include "chatservice.hpp"
#include "json.hpp"
using namespace std;
using namespace placeholders;
using json = nlohmann::json;

namespace {
// 单条消息最大 8MB,防御畸形长度
constexpr int32_t kMaxMsgLen = 8 * 1024 * 1024;
}  // namespace

ChatServer::ChatServer(EventLoop *loop, const InetAddress &listenAddr,
                       const string &nameArg)
    : _server(loop, listenAddr, nameArg), _loop(loop) {
  // 注册链接回调
  _server.setConnectionCallback(std::bind(&ChatServer::onConnection, this, _1));

  // 注册消息回调
  _server.setMessageCallback(
      std::bind(&ChatServer::onMessage, this, _1, _2, _3));

  // 设置线程数量
  _server.setThreadNum(4);
}

void ChatServer::start() { _server.start(); }

// 上报链接相关信息的回调函数
void ChatServer::onConnection(const TcpConnectionPtr &conn) {
  // 客户端断开连接
  if (!conn->connected()) {
    ChatService::instance()->clientCloseException(conn);
    conn->shutdown();
  }
}

// 上报读写事件相关信息的回调函数
// 帧格式: [4B 大端长度][JSON payload]
void ChatServer::onMessage(const TcpConnectionPtr &conn, Buffer *buffer,
                           Timestamp time) {
  // 循环拆帧:TCP 粘包/拆包都由这里处理
  while (buffer->readableBytes() >= sizeof(int32_t)) {
    int32_t len = buffer->peekInt32();  // muduo 自动 ntohl
    if (len <= 0 || len > kMaxMsgLen) {
      LOG_ERROR << "invalid frame length=" << len << ", shutting down conn";
      conn->shutdown();
      return;
    }
    if (buffer->readableBytes() < sizeof(int32_t) + static_cast<size_t>(len)) {
      // 本帧未到齐,等下一次回调
      return;
    }
    buffer->retrieveInt32();
    string payload = buffer->retrieveAsString(len);

    try {
      json js = json::parse(payload);
      int msgid = js.value("msgid", -1);
      if (msgid < 0) {
        LOG_ERROR << "message missing msgid: " << payload;
        continue;
      }
      auto msgHandler = ChatService::instance()->getHandler(msgid);
      msgHandler(conn, js, time);
    } catch (const std::exception &e) {
      LOG_ERROR << "bad message: " << e.what() << " payload=" << payload;
      // 容忍单条坏包,不关闭连接
    }
  }
}
