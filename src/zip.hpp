// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <optional>

namespace co {

struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;  // 解压后的内容（用于 get/set）

    // 原始保留元数据（从原 ZIP 读取的条目）
    bool preserveRaw = false;       // true=保留原始压缩字节
    uint16_t method = 0;            // 0=Store, 8=Deflate
    uint16_t createVersion = 20;    // 版本由谁创建
    uint16_t extractVersion = 20;   // 解压所需版本
    uint16_t flags = 0;             // 通用标志位
    uint32_t crc = 0;
    uint64_t compressedSize = 0;
    uint64_t uncompressedSize = 0;
    std::vector<uint8_t> extraField;     // 本地文件头的 extra field（保留）
    std::vector<uint8_t> compressedData; // 原始压缩字节（preserveRaw=true 时用）
    uint16_t modTime = 0;           // MS-DOS 时间
    uint16_t modDate = 0x0021;      // MS-DOS 日期 (1980-01-01)
};

class Document {
public:
    // 从文件加载 ZIP，保留所有条目的原始压缩字节
    static std::unique_ptr<Document> load(const std::string& path);

    // 获取解压后的数据。返回 false 如果不存在
    bool get(const std::string& name, std::vector<uint8_t>& out) const;
    // 获取条目指针（可修改）
    ZipEntry* getEntryMut(const std::string& name);
    const ZipEntry* getEntry(const std::string& name) const;

    // 设置新条目（用于 .co 条目）。data 是未压缩内容，写入时做 DEFLATE
    void set(const std::string& name, const std::vector<uint8_t>& data);
    // 设置完整条目（保留元数据）
    void setEntry(const std::string& name, const ZipEntry& entry);
    // 删除条目
    void remove(const std::string& name);
    // 列出所有条目名（按字母排序）
    std::vector<std::string> list() const;

    // 写入 ZIP 到文件（临时文件 + rename）
    bool write(const std::string& path);

    // 直接访问内部 entries（供 objectstore 等模块用）
    std::map<std::string, ZipEntry>& entriesMut() { return entries_; }
    const std::map<std::string, ZipEntry>& entries() const { return entries_; }

private:
    std::map<std::string, ZipEntry> entries_;
    // 记录原始条目的顺序（用于写入时保持原序）
    std::vector<std::string> originalOrder_;
};

} // namespace co
