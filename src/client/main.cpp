#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "json.hpp"
using namespace std;
using json = nlohmann::json;

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>

#include "group.hpp"
#include "public.hpp"
#include "user.hpp"

// 记录当前系统登录的用户信息
User g_currentUser;
// 记录当前登录用户的好友列表信息
vector<User> g_currentUserFriendList;
// 记录当前登录用户的群组列表信息
vector<Group> g_currentUserGroupList;

// 控制主菜单页面程序
bool isMainMenuRunning = false;

// 用于读写线程之间的通信
sem_t rwsem;
// 记录登录状态
atomic_bool g_isLoginSuccess{false};

// 接收线程
void readTaskHandler(int clientfd);
// 获取系统时间(聊天信息需要添加时间信息)
string getCurrentTime();
// 主聊天页面程序
void mainMenu(int);
// 显示当前登录成功用户的基本信息
void showCurrentUserData();

// 单条消息最大 8MB
static constexpr int32_t kMaxMsgLen = 8 * 1024 * 1024;

// 以 4B 大端长度前缀 + payload 的形式发送
static bool sendFramed(int fd, const string& payload) {
  int32_t len_be = htonl(static_cast<int32_t>(payload.size()));
  // 为了避免两次 send 导致的意外拆分,合并到一个缓冲里
  string buf;
  buf.reserve(sizeof(int32_t) + payload.size());
  buf.append(reinterpret_cast<const char*>(&len_be), sizeof(int32_t));
  buf.append(payload);

  const char* p = buf.data();
  size_t left = buf.size();
  while (left > 0) {
    ssize_t n = ::send(fd, p, left, 0);
    if (n <= 0) return false;
    p += n;
    left -= n;
  }
  return true;
}

// 处理一条已拆帧的 JSON 文本
static void dispatchMessage(const string& payload);

// 聊天客户端程序实现,main线程用作发送线程,子线程用作接收线程
int main(int argc, char **argv) {
  if (argc < 3) {
    cerr << "command invalid! example: ./ChatClient 127.0.0.1 6000" << endl;
    exit(-1);
  }

  // 解析通过命令行参数传递的ip和port
  char *ip = argv[1];
  uint16_t port = atoi(argv[2]);

  // 创建client端的socket
  int clientfd = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == clientfd) {
    cerr << "socket create error" << endl;
    exit(-1);
  }

  // 填写client需要连接的server信息ip+port
  sockaddr_in server;
  memset(&server, 0, sizeof(sockaddr_in));

  server.sin_family = AF_INET;
  server.sin_port = htons(port);
  server.sin_addr.s_addr = inet_addr(ip);

  // client和server进行连接
  if (-1 == connect(clientfd, (sockaddr *)&server, sizeof(sockaddr_in))) {
    cerr << "connect server error" << endl;
    close(clientfd);
    exit(-1);
  }

  // 初始化读写线程通信用的信号量
  sem_init(&rwsem, 0, 0);

  // 连接服务器成功,启动接收子线程
  std::thread readTask(readTaskHandler, clientfd);
  readTask.detach();

  // main线程用于接收用户输入,负责发送数据
  for (;;) {
    cout << "========================" << endl;
    cout << "1. login" << endl;
    cout << "2. register" << endl;
    cout << "3. quit" << endl;
    cout << "========================" << endl;
    cout << "choice:";
    int choice = 0;
    cin >> choice;
    cin.get();  // 读掉缓冲区残留的回车

    switch (choice) {
      case 1: {
        int id = 0;
        char pwd[50] = {0};
        cout << "userid:";
        cin >> id;
        cin.get();
        cout << "userpassword:";
        cin.getline(pwd, 50);

        json js;
        js["msgid"] = LOGIN_MSG;
        js["id"] = id;
        js["password"] = pwd;
        string request = js.dump();

        g_isLoginSuccess = false;

        if (!sendFramed(clientfd, request)) {
          cerr << "send login msg error:" << request << endl;
        }

        sem_wait(&rwsem);

        if (g_isLoginSuccess) {
          isMainMenuRunning = true;
          mainMenu(clientfd);
        }
      } break;
      case 2: {
        char name[50] = {0};
        char pwd[50] = {0};
        cout << "username:";
        cin.getline(name, 50);
        cout << "userpassword:";
        cin.getline(pwd, 50);

        json js;
        js["msgid"] = REG_MSG;
        js["name"] = name;
        js["password"] = pwd;
        string request = js.dump();

        if (!sendFramed(clientfd, request)) {
          cerr << "send reg msg error:" << request << endl;
        }

        sem_wait(&rwsem);
      } break;
      case 3:
        close(clientfd);
        sem_destroy(&rwsem);
        exit(0);
      default:
        cerr << "invalid input!" << endl;
        break;
    }
  }

  return 0;
}

// 处理注册的响应逻辑
void doRegResponse(json &responsejs) {
  if (0 != responsejs.value("errno", -1)) {
    cerr << "name is already exist, register error!" << endl;
  } else {
    cout << "name register success, userid is " << responsejs["id"]
         << ", do not forget it!" << endl;
  }
}

// 处理登录的响应逻辑
void doLoginResponse(json &responsejs) {
  if (0 != responsejs.value("errno", -1)) {
    cerr << responsejs.value("errmsg", "login failed") << endl;
    g_isLoginSuccess = false;
  } else {
    g_currentUser.setId(responsejs["id"].get<int>());
    g_currentUser.setName(responsejs["name"]);

    if (responsejs.contains("friends")) {
      g_currentUserFriendList.clear();

      vector<string> vec = responsejs["friends"];
      for (string &str : vec) {
        json js = json::parse(str);
        User user;
        user.setId(js["id"].get<int>());
        user.setName(js["name"]);
        user.setState(js["state"]);
        g_currentUserFriendList.push_back(user);
      }
    }

    if (responsejs.contains("groups")) {
      g_currentUserGroupList.clear();

      vector<string> vec1 = responsejs["groups"];
      for (string &groupstr : vec1) {
        json grpjs = json::parse(groupstr);
        Group group;
        group.setId(grpjs["id"].get<int>());
        group.setName(grpjs["groupname"]);
        group.setDesc(grpjs["groupdesc"]);

        vector<string> vec2 = grpjs["users"];
        for (string &userstr : vec2) {
          GroupUser user;
          json js = json::parse(userstr);
          user.setId(js["id"].get<int>());
          user.setName(js["name"]);
          user.setState(js["state"]);
          user.setRole(js["role"]);
          group.getUsers().push_back(user);
        }

        g_currentUserGroupList.push_back(group);
      }
    }

    showCurrentUserData();

    if (responsejs.contains("offlinemsg")) {
      vector<string> vec = responsejs["offlinemsg"];
      for (string &str : vec) {
        json js = json::parse(str);
        if (ONE_CHAT_MSG == js["msgid"].get<int>()) {
          cout << js["time"].get<string>() << " [" << js["id"] << "]"
               << js["name"].get<string>()
               << " said: " << js["msg"].get<string>() << endl;
        } else {
          cout << "群消息[" << js["groupid"] << "]:" << js["time"].get<string>()
               << " [" << js["id"] << "]" << js["name"].get<string>()
               << " said: " << js["msg"].get<string>() << endl;
        }
      }
    }

    g_isLoginSuccess = true;
  }
}

// 处理一条已拆帧的 JSON 文本
static void dispatchMessage(const string& payload) {
  json js;
  try {
    js = json::parse(payload);
  } catch (const std::exception& e) {
    cerr << "bad response from server: " << e.what() << endl;
    return;
  }

  int msgtype = js.value("msgid", -1);

  if (ONE_CHAT_MSG == msgtype) {
    cout << js["time"].get<string>() << " [" << js["id"] << "]"
         << js["name"].get<string>() << " said: " << js["msg"].get<string>()
         << endl;
    return;
  }
  if (GROUP_CHAT_MSG == msgtype) {
    cout << "群消息[" << js["groupid"] << "]:" << js["time"].get<string>()
         << " [" << js["id"] << "]" << js["name"].get<string>()
         << " said: " << js["msg"].get<string>() << endl;
    return;
  }
  if (LOGIN_MSG_ACK == msgtype) {
    doLoginResponse(js);
    sem_post(&rwsem);
    return;
  }
  if (REG_MSG_ACK == msgtype) {
    doRegResponse(js);
    sem_post(&rwsem);
    return;
  }
}

// 子线程 - 接收线程(累积缓冲 + 4B 长度前缀拆帧)
void readTaskHandler(int clientfd) {
  string recvBuf;
  recvBuf.reserve(16 * 1024);
  char chunk[8192];

  for (;;) {
    ssize_t len = recv(clientfd, chunk, sizeof(chunk), 0);
    if (len <= 0) {
      close(clientfd);
      exit(-1);
    }
    recvBuf.append(chunk, chunk + len);

    // 循环拆帧
    while (recvBuf.size() >= sizeof(int32_t)) {
      int32_t len_be = 0;
      memcpy(&len_be, recvBuf.data(), sizeof(int32_t));
      int32_t msglen = ntohl(len_be);
      if (msglen <= 0 || msglen > kMaxMsgLen) {
        cerr << "invalid frame length=" << msglen << ", closing" << endl;
        close(clientfd);
        exit(-1);
      }
      if (recvBuf.size() < sizeof(int32_t) + static_cast<size_t>(msglen)) {
        break;  // 等下一段
      }
      string payload = recvBuf.substr(sizeof(int32_t), msglen);
      recvBuf.erase(0, sizeof(int32_t) + msglen);
      dispatchMessage(payload);
    }
  }
}

// 显示当前登录成功用户的基本信息
void showCurrentUserData() {
  cout << "======================login user======================" << endl;
  cout << "current login user => id:" << g_currentUser.getId()
       << " name:" << g_currentUser.getName() << endl;
  cout << "----------------------friend list---------------------" << endl;
  if (!g_currentUserFriendList.empty()) {
    for (User &user : g_currentUserFriendList) {
      cout << user.getId() << " " << user.getName() << " " << user.getState()
           << endl;
    }
  }
  cout << "----------------------group list----------------------" << endl;
  if (!g_currentUserGroupList.empty()) {
    for (Group &group : g_currentUserGroupList) {
      cout << group.getId() << " " << group.getName() << " " << group.getDesc()
           << endl;
      for (GroupUser &user : group.getUsers()) {
        cout << user.getId() << " " << user.getName() << " " << user.getState()
             << " " << user.getRole() << endl;
      }
    }
  }
  cout << "======================================================" << endl;
}

// "help" command handler
void help(int fd = 0, string str = "");
void chat(int, string);
void addfriend(int, string);
void creategroup(int, string);
void addgroup(int, string);
void groupchat(int, string);
void loginout(int, string);

unordered_map<string, string> commandMap = {
    {"help", "显示所有支持的命令,格式help"},
    {"chat", "一对一聊天,格式chat:friendid:message"},
    {"addfriend", "添加好友,格式addfriend:friendid"},
    {"creategroup", "创建群组,格式creategroup:groupname:groupdesc"},
    {"addgroup", "加入群组,格式addgroup:groupid"},
    {"groupchat", "群聊,格式groupchat:groupid:message"},
    {"loginout", "注销,格式loginout"}};

unordered_map<string, function<void(int, string)>> commandHandlerMap = {
    {"help", help},           {"chat", chat},
    {"addfriend", addfriend}, {"creategroup", creategroup},
    {"addgroup", addgroup},   {"groupchat", groupchat},
    {"loginout", loginout}};

void mainMenu(int clientfd) {
  help();

  char buffer[1024] = {0};
  while (isMainMenuRunning) {
    cin.getline(buffer, 1024);
    string commandbuf(buffer);
    string command;
    int idx = commandbuf.find(":");
    if (-1 == idx) {
      command = commandbuf;
    } else {
      command = commandbuf.substr(0, idx);
    }
    auto it = commandHandlerMap.find(command);
    if (it == commandHandlerMap.end()) {
      cerr << "invalid input command!" << endl;
      continue;
    }

    it->second(clientfd,
               commandbuf.substr(idx + 1, commandbuf.size() - idx));
  }
}

void help(int, string) {
  cout << "show command list >>> " << endl;
  for (auto &p : commandMap) {
    cout << p.first << " : " << p.second << endl;
  }
  cout << endl;
}

void addfriend(int clientfd, string str) {
  int friendid = atoi(str.c_str());
  json js;
  js["msgid"] = ADD_FRIEND_MSG;
  js["id"] = g_currentUser.getId();
  js["friendid"] = friendid;
  if (!sendFramed(clientfd, js.dump())) {
    cerr << "send addfriend msg error" << endl;
  }
}

void chat(int clientfd, string str) {
  int idx = str.find(":");
  if (-1 == idx) {
    cerr << "chat command invalid!" << endl;
    return;
  }

  int friendid = atoi(str.substr(0, idx).c_str());
  string message = str.substr(idx + 1, str.size() - idx);

  json js;
  js["msgid"] = ONE_CHAT_MSG;
  js["id"] = g_currentUser.getId();
  js["name"] = g_currentUser.getName();
  js["to"] = friendid;
  js["msg"] = message;
  js["time"] = getCurrentTime();
  if (!sendFramed(clientfd, js.dump())) {
    cerr << "send chat msg error" << endl;
  }
}

void creategroup(int clientfd, string str) {
  int idx = str.find(":");
  if (-1 == idx) {
    cerr << "creategroup command invalid!" << endl;
    return;
  }

  string groupname = str.substr(0, idx);
  string groupdesc = str.substr(idx + 1, str.size() - idx);

  json js;
  js["msgid"] = CREATE_GROUP_MSG;
  js["id"] = g_currentUser.getId();
  js["groupname"] = groupname;
  js["groupdesc"] = groupdesc;
  if (!sendFramed(clientfd, js.dump())) {
    cerr << "send creategroup msg error" << endl;
  }
}

void addgroup(int clientfd, string str) {
  int groupid = atoi(str.c_str());
  json js;
  js["msgid"] = ADD_GROUP_MSG;
  js["id"] = g_currentUser.getId();
  js["groupid"] = groupid;
  if (!sendFramed(clientfd, js.dump())) {
    cerr << "send addgroup msg error" << endl;
  }
}

void groupchat(int clientfd, string str) {
  int idx = str.find(":");
  if (-1 == idx) {
    cerr << "groupchat command invalid!" << endl;
    return;
  }

  int groupid = atoi(str.substr(0, idx).c_str());
  string message = str.substr(idx + 1, str.size() - idx);

  json js;
  js["msgid"] = GROUP_CHAT_MSG;
  js["id"] = g_currentUser.getId();
  js["name"] = g_currentUser.getName();
  js["groupid"] = groupid;
  js["msg"] = message;
  js["time"] = getCurrentTime();
  if (!sendFramed(clientfd, js.dump())) {
    cerr << "send groupchat msg error" << endl;
  }
}

void loginout(int clientfd, string) {
  json js;
  js["msgid"] = LOGINOUT_MSG;
  js["id"] = g_currentUser.getId();
  if (!sendFramed(clientfd, js.dump())) {
    cerr << "send loginout msg error" << endl;
  } else {
    isMainMenuRunning = false;
  }
}

string getCurrentTime() {
  auto tt =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  struct tm *ptm = localtime(&tt);
  char date[60] = {0};
  snprintf(date, sizeof(date), "%d-%02d-%02d %02d:%02d:%02d",
           (int)ptm->tm_year + 1900, (int)ptm->tm_mon + 1, (int)ptm->tm_mday,
           (int)ptm->tm_hour, (int)ptm->tm_min, (int)ptm->tm_sec);
  return std::string(date);
}
