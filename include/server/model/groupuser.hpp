#ifndef GROUPUSER_H
#define GROUPUSER_H

#include "user.hpp"

// 群组user是一个user知识多了一个role角色信息，所以继承
class GroupUser : public User {
 private:
  string role;

 public:
  void setRole(string role) { this->role = role; }
  string getRole() { return this->role; }
};

#endif