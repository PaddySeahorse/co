// SPDX-License-Identifier: GPL-3.0-or-later

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

    // 写入 blob（= writeObject("blob", data)）。显式去重入口（第七章）：
    // 相同内容 → 相同 hash → hasObject 命中即跳过写入。
    std::string writeBlob(const std::vector<uint8_t>& data);

    // 只计算对象哈希，不写入、不去重（供 status/diff 比较使用）。
    std::string hashObject(const std::string& objType, const std::vector<uint8_t>& content) const;

    // 读取对象，返回 {type, content}。失败返回 nullopt
    std::optional<std::pair<std::string, std::vector<uint8_t>>> readObject(const std::string& hash) const;

    // 是否存在
    bool hasObject(const std::string& hash) const;

    // 列出 loose 对象哈希
    std::vector<std::string> listLooseObjects() const;

    // 列出所有 pack 文件名（不含路径与 .pack 后缀）
    std::vector<std::string> listPacks() const;

    Document& doc() { return doc_; }
    const Document& doc() const { return doc_; }

private:
    Document& doc_;
};

// 哈希 -> 对象路径 (.co/objects/xx/yyyy...)
std::string objectPath(const std::string& hash);

} // namespace co
