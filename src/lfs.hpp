// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "objectstore.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace co {

// LFS 对象库目录与指针 spec 标识
inline constexpr const char* kLfsObjectsDir = ".co/lfs/objects";
inline constexpr const char* kLfsPointerVersion = "https://co.dev/spec/lfs/v1";

// ---- 判定规则 ----
// 扩展名（小写）是否在白名单内（图片/视频/音频/pdf 等）
bool isLfsExtension(const std::string& path);

// 综合判定：扩展名命中白名单即走 LFS（不看大小）
bool shouldUseLfs(const std::string& path);

// ---- 指针编解码 ----
struct LfsPointer {
    std::string oid;   // 64 位小写十六进制（sha256）
    uint64_t size = 0;
};

// 首行匹配 version 魔数即为指针
bool isLfsPointer(const std::vector<uint8_t>& blobData);

// 解析指针；魔数匹配但格式损坏返回 false
bool parseLfsPointer(const std::vector<uint8_t>& blobData, LfsPointer& out);

// 构造指针文本（三行 + 结尾换行）
std::vector<uint8_t> makeLfsPointer(const std::string& oid, uint64_t size);

// ---- LFS 对象读写 ----
// oid -> .co/lfs/objects/xx/yyyy
std::string lfsObjectPath(const std::string& oid);

// sha256(内容) 的 64 位小写十六进制
std::string computeLfsOid(const std::vector<uint8_t>& content);

// 原样写入（去重：已存在返回已有 oid），返回 oid
std::string writeLfsObject(Document& doc, const std::vector<uint8_t>& content);

// 读取；缺失返回 false
bool readLfsObject(const Document& doc, const std::string& oid,
                   std::vector<uint8_t>& out);

bool hasLfsObject(const Document& doc, const std::string& oid);

// 枚举所有 LFS oid
std::vector<std::string> listLfsObjects(const Document& doc);

void removeLfsObject(Document& doc, const std::string& oid);

// ---- 统一 blob 内容解析 ----
// 读 blob；若为指针则从 LFS 库还原真实内容，否则原样返回。
// 指针对象缺失或非 blob 返回 false。供 checkout/status/diff 复用。
bool resolveBlobContent(const Store& store, const std::string& hash,
                        std::vector<uint8_t>& out);

} // namespace co
