#pragma once
#include "zip.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <utility>

namespace co {

// 常量
inline constexpr const char* kObjectsDir = ".co/objects";
inline constexpr const char* kHeadFile = ".co/HEAD";

class Store {
public:
    explicit Store(Document& doc);

    // HEAD 读写
    std::string head() const;
    void setHead(const std::string& hash);

    // 写入对象，返回哈希
    std::string writeObject(const std::string& objType, const std::vector<uint8_t>& content);

    // 读取对象，返回 {type, content}。失败返回 nullopt
    std::optional<std::pair<std::string, std::vector<uint8_t>>> readObject(const std::string& hash) const;

    // 是否存在
    bool hasObject(const std::string& hash) const;

    // 列出 loose 对象哈希
    std::vector<std::string> listLooseObjects() const;

    Document& doc() { return doc_; }
    const Document& doc() const { return doc_; }

private:
    Document& doc_;
};

// 哈希 -> 对象路径 (.co/objects/xx/yyyy...)
std::string objectPath(const std::string& hash);

} // namespace co
