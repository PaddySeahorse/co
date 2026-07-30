// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "zip.hpp"
#include "objectstore.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace co {

struct Commit {
    std::string hash;
    std::string tree;
    std::vector<std::string> parents;  // 多父提交（merge commit 有 2 个；首父为主干）
    std::string parent;                // = parents.empty() ? "" : parents[0]（兼容旧字段）
    std::string authorName;
    std::string authorEmail;
    int64_t timestamp = 0;  // Unix 秒
    std::string message;
};

struct TreeEntry {
    std::string path;
    std::string hash;
};

// 创建提交（单 doc：history 与 content 同在一个 Document，内嵌模式）。
// nowUnix 是 Unix 时间戳。返回哈希。
std::string createCommit(Document& doc, const std::string& message, int64_t nowUnix);

// 创建提交（双 doc：historyDoc 存 .co/ 对象，contentDoc 是 Office 文件内容）。
// 用于外部 .co 模式（第四章）。
std::string createCommitExternal(Document& historyDoc, const Document& contentDoc,
                                 const std::string& message, int64_t nowUnix);

// 创建 merge commit：给定已计算好的 tree 与多个父提交（第五章 bundle-merge 用）。
std::string createMergeCommit(Document& historyDoc, const std::string& treeHash,
                              const std::vector<std::string>& parents,
                              const std::string& message, int64_t nowUnix);

// 遍历提交历史（沿首父链）
std::vector<Commit> logCommits(const Document& doc);

// 检出指定提交（单 doc）。
bool checkoutCommit(Document& doc, const std::string& commitHash);

// 检出指定提交（双 doc：从 historyDoc 读对象，把内容写回 contentDoc）。
bool checkoutCommitExternal(Document& historyDoc, Document& contentDoc,
                            const std::string& commitHash);

// 解析 commit 对象内容
Commit parseCommit(const std::string& hash, const std::vector<uint8_t>& content);

// 解析 tree 对象内容
std::vector<TreeEntry> parseTree(const std::vector<uint8_t>& content);

// 读取并解析指定 commit（从 doc）。失败返回 nullopt。
std::optional<Commit> readCommit(const Document& doc, const std::string& hash);

// 读取并解析指定 tree（从 doc）。失败返回空。
std::vector<TreeEntry> readTree(const Document& doc, const std::string& hash);

// 计算 tree 对象哈希（不写入）。供 bundle-merge 构建合并 tree 用。
std::string buildTreeHash(Store& store, const std::vector<TreeEntry>& entries);

// 把 tree entries 写成 tree 对象，返回 tree hash。
std::string writeTree(Store& store, const std::vector<TreeEntry>& entries);

// 格式化 Unix 时间戳为 RFC1123Z (UTC): "Mon, 02 Jan 2006 15:04:05 +0000"
std::string formatTimestampRFC1123Z(int64_t unixTime);

// 判断是否为 .co 条目（路径以 .co/ 开头或等于 .co）
bool isCoEntry(const std::string& name);

} // namespace co
