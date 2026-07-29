// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "zip.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace co {

// pack 目录常量
inline constexpr const char* kPackDir = ".co/objects/pack";

struct PackedObject {
    std::string hash;
    std::string type;   // "commit", "tree", "blob"
    std::vector<uint8_t> data;
    uint32_t offset = 0;
    uint32_t crc32 = 0;
};

struct PackIndex {
    std::vector<std::string> hashes;
    std::vector<uint32_t> crcs;
    std::vector<uint32_t> offsets;
};

// 写入 packfile + idx。成功返回 true
bool writePack(Document& doc, const std::vector<PackedObject>& objects, const std::string& packName);

// 读取 pack 索引。成功返回 true
bool readPackIndex(const Document& doc, const std::string& packName, PackIndex& out);

// 从 pack 中读取指定哈希的对象。成功返回 true
bool readPackedObject(const Document& doc, const std::string& packName, const std::string& hash, std::string& outType, std::vector<uint8_t>& outData);

// 列出所有 pack 文件名（不含路径和 .pack 后缀）
std::vector<std::string> listPackFiles(const Document& doc);

} // namespace co
