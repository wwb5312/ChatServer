#include "password.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace {

constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;  // SHA256

std::string toHex(const unsigned char* data, size_t len) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < len; ++i) {
    oss << std::setw(2) << static_cast<int>(data[i]);
  }
  return oss.str();
}

bool fromHex(const std::string& hex, unsigned char* out, size_t out_len) {
  if (hex.size() != out_len * 2) return false;
  for (size_t i = 0; i < out_len; ++i) {
    unsigned int byte = 0;
    if (std::sscanf(hex.c_str() + i * 2, "%2x", &byte) != 1) return false;
    out[i] = static_cast<unsigned char>(byte);
  }
  return true;
}

// 计算 sha256(salt || plain)
bool sha256(const unsigned char* salt, size_t salt_len,
            const std::string& plain, unsigned char out[kHashBytes]) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return false;
  bool ok = true;
  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) ok = false;
  if (ok && EVP_DigestUpdate(ctx, salt, salt_len) != 1) ok = false;
  if (ok && !plain.empty() &&
      EVP_DigestUpdate(ctx, plain.data(), plain.size()) != 1) {
    ok = false;
  }
  unsigned int out_len = 0;
  if (ok && EVP_DigestFinal_ex(ctx, out, &out_len) != 1) ok = false;
  EVP_MD_CTX_free(ctx);
  return ok && out_len == kHashBytes;
}

}  // namespace

namespace pwd {

std::string hashPassword(const std::string& plain) {
  unsigned char salt[kSaltBytes];
  if (RAND_bytes(salt, kSaltBytes) != 1) {
    // 极端情况下 OpenSSL 未初始化/熵不足,退化为错误哈希——让验证一定不过
    return {};
  }
  unsigned char hash[kHashBytes];
  if (!sha256(salt, kSaltBytes, plain, hash)) return {};

  return toHex(salt, kSaltBytes) + "$" + toHex(hash, kHashBytes);
}

bool verifyPassword(const std::string& stored, const std::string& plain) {
  auto pos = stored.find('$');
  if (pos == std::string::npos) return false;
  std::string salt_hex = stored.substr(0, pos);
  std::string hash_hex = stored.substr(pos + 1);

  unsigned char salt[kSaltBytes];
  unsigned char expected[kHashBytes];
  if (!fromHex(salt_hex, salt, kSaltBytes)) return false;
  if (!fromHex(hash_hex, expected, kHashBytes)) return false;

  unsigned char actual[kHashBytes];
  if (!sha256(salt, kSaltBytes, plain, actual)) return false;

  // 常量时间比较
  unsigned char diff = 0;
  for (size_t i = 0; i < kHashBytes; ++i) diff |= actual[i] ^ expected[i];
  return diff == 0;
}

}  // namespace pwd
