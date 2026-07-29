// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "zip.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace co {

struct Commit {
    std::string hash;
    std::string tree;
    std::string parent;
    std::string authorName;
    std::string authorEmail;
    int64_t timestamp = 0;  // Unix 秒
    std::string message;
};

struct TreeEntry {
    std::string path;
    std::string hash;
};

// 创建提交。nowUnix 是 Unix 时间戳。返回哈希。
std::string createCommit(Document& doc, const std::string& message, int64_t nowUnix);

// 遍历提交历史
std::vector<Commit> logCommits(const Document& doc);

// 检出指定提交。成功返回 true。
bool checkoutCommit(Document& doc, const std::string& commitHash);

// 解析 commit 对象内容
Commit parseCommit(const std::string& hash, const std::vector<uint8_t>& content);

// 解析 tree 对象内容
std::vector<TreeEntry> parseTree(const std::vector<uint8_t>& content);

// 格式化 Unix 时间戳为 RFC1123Z (UTC): "Mon, 02 Jan 2006 15:04:05 +0000"
std::string formatTimestampRFC1123Z(int64_t unixTime);

} // namespace co
