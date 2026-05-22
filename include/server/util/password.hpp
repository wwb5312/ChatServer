#ifndef PASSWORD_H
#define PASSWORD_H

#include <string>

namespace pwd {

// 生成盐+哈希,返回 "<32-hex-salt>$<64-hex-sha256>"
std::string hashPassword(const std::string& plain);

// 校验。stored 若不是 "salt$hash" 格式,直接返回 false
bool verifyPassword(const std::string& stored, const std::string& plain);

}  // namespace pwd

#endif
