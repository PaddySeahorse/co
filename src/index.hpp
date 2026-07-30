// SPDX-License-Identifier: GPL-3.0-or-later
//
// 增量 Commit 索引（对应改造清单第九章）。
//
// 记录每个 ZIP 内部文件的上次 commit 指纹（未压缩大小 + CRC32）与 blob hash。
// commit 时先用指纹筛掉未变化文件（直接复用 blob hash，免读数据/免重算 hash），
// 再配合 writeObject 内部的对象级去重，双重压缩 commit 体积与开销。
//
// 存储路径：.co/index（文本格式）。缺失或格式不合法时优雅降级为全量 commit。

#pragma once
#include "zip.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace co {

inline constexpr const char* kIndexFile = ".co/index";

struct IndexEntry {
    std::string path;
    uint64_t size = 0;       // 未压缩大小
    uint32_t crc = 0;        // CRC32（与 ZIP 中央目录一致）
    std::string blobHash;    // 上次 commit 写入的 blob hash
};

struct CommitIndex {
    std::vector<IndexEntry> entries;
    bool valid = false;      // false 表示无索引或格式不兼容（走全量）
};

// 读取 .co/index。缺失/损坏返回 valid=false。
CommitIndex loadIndex(const Document& doc);

// 写入 .co/index。
bool saveIndex(Document& doc, const CommitIndex& idx);

// 删除 .co/index（checkout/migrate 后失效重建）。
void removeIndex(Document& doc);

// 在索引中按路径查找条目。
const IndexEntry* findEntry(const CommitIndex& idx, const std::string& path);

} // namespace co
