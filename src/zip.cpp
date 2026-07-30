// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "zip.hpp"
#include "util.hpp"

#include <fstream>
#include <algorithm>
#include <set>
#include <cstdio>
#include <cstddef>
#include <utility>

namespace co {

namespace {

// 标记"未找到"的哨兵值
constexpr size_t NPOS = static_cast<size_t>(-1);

// 本地文件头 / 中央目录头 / EOCD 签名（小端存储）
constexpr uint32_t SIG_LOCAL_HEADER = 0x04034b50;   // PK\x03\x04
constexpr uint32_t SIG_CENTRAL_DIR  = 0x02014b50;   // PK\x01\x02
constexpr uint32_t SIG_EOCD         = 0x06054b50;   // PK\x05\x06

// EOCD 最小 22 字节，加上最长 65535 字节注释，搜索范围为最后 65557 字节
constexpr size_t EOCD_MAX_SEARCH = 65557;
constexpr size_t EOCD_MIN_SIZE   = 22;
constexpr size_t CDH_FIXED_SIZE  = 46;   // 中央目录条目固定长度
constexpr size_t LFH_FIXED_SIZE  = 30;   // 本地文件头固定长度

// 判断是否为目录条目（名称以 '/' 结尾）
bool isDirectory(const std::string& name) {
    return !name.empty() && name.back() == '/';
}

// 类似 Go path.Dir：取最后一个 '/' 之前的部分；无 '/' 返回 "."；根返回 "/"
// 注意：仅对非目录名调用（目录名在上层已提前返回）
std::string pathDir(const std::string& name) {
    size_t pos = name.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return name.substr(0, pos);
}

// 为缺失的父目录补全 "dir/" 的 Store 条目（对齐 Go addDirectoryEntries）
void addDirectoryEntries(std::map<std::string, ZipEntry>& entries, const std::string& name) {
    if (isDirectory(name)) return;
    std::string dir = pathDir(name);
    while (dir != "." && dir != "/") {
        std::string dirName = dir + "/";
        if (entries.find(dirName) == entries.end()) {
            ZipEntry e;
            e.name = dirName;
            e.method = 0;            // Store
            e.preserveRaw = false;
            e.createVersion = 20;
            e.extractVersion = 20;
            // modTime=0, modDate=0x0021 (1980-01-01) 为结构体默认值
            entries[dirName] = std::move(e);
        }
        dir = pathDir(dir);
    }
}

// 读取整个文件到内存
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (!f) return false;
    }
    return true;
}

// 从文件末尾向前搜索 EOCD 签名，返回其偏移；未找到返回 NPOS
size_t findEOCD(const std::vector<uint8_t>& buf) {
    if (buf.size() < EOCD_MIN_SIZE) return NPOS;
    size_t start = buf.size() - EOCD_MIN_SIZE;
    size_t minPos = (buf.size() >= EOCD_MAX_SEARCH) ? (buf.size() - EOCD_MAX_SEARCH) : 0;
    for (size_t i = start + 1; i-- > minPos; ) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4b &&
            buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
            return i;
        }
    }
    return NPOS;
}

// 解析后的写入用数据：实际写入的压缩字节 + 元数据
struct Resolved {
    uint16_t method = 0;
    uint32_t crc = 0;          // 真实 CRC（中央目录用）
    uint32_t compSize = 0;     // 真实压缩大小（中央目录用）
    uint32_t uncompSize = 0;   // 真实未压缩大小（中央目录用）
    bool dataDescriptor = false;  // 本地头是否用 data descriptor（本地头 crc/sizes 写 0）
    std::vector<uint8_t> data;  // 本地头之后要写入的字节（含可能的 data descriptor）
};

// 根据条目状态解析出实际写入数据
Resolved resolveEntry(const std::string& name, const ZipEntry& e) {
    Resolved r;
    if (e.preserveRaw) {
        // 保留原始压缩字节，不重新压缩
        r.method = e.method;
        r.crc = e.crc;
        r.compSize = static_cast<uint32_t>(e.compressedSize);
        r.uncompSize = static_cast<uint32_t>(e.uncompressedSize);
        // flags bit 3 置位表示使用 data descriptor：本地头 crc/sizes 为 0，
        // 真实值在压缩数据之后的描述符中（已并入 compressedData）。
        r.dataDescriptor = (e.flags & 0x0008) != 0;
        r.data = e.compressedData;
    } else if (isDirectory(name)) {
        // 目录条目：Store，无数据
        r.method = 0;
        r.crc = 0;
        r.compSize = 0;
        r.uncompSize = 0;
    } else if (e.method == 8) {
        // 新条目 DEFLATE 压缩（raw deflate，ZIP method=8）
        r.method = 8;
        r.data = compressDeflateRaw(e.data);
        r.crc = crc32IEEE(e.data);
        r.compSize = static_cast<uint32_t>(r.data.size());
        r.uncompSize = static_cast<uint32_t>(e.data.size());
    } else {
        // 新条目 Store：直接写未压缩数据
        r.method = 0;
        r.data = e.data;
        r.crc = crc32IEEE(e.data);
        r.compSize = static_cast<uint32_t>(r.data.size());
        r.uncompSize = static_cast<uint32_t>(e.data.size());
    }
    return r;
}

// 追加本地文件头（不含数据区）
void appendLocalHeader(std::vector<uint8_t>& out, const std::string& name,
                       const ZipEntry& e, const Resolved& r) {
    writeLE32(out, SIG_LOCAL_HEADER);
    writeLE16(out, e.extractVersion);                  // version_needed
    writeLE16(out, e.flags);
    writeLE16(out, r.method);
    writeLE16(out, e.modTime);
    writeLE16(out, e.modDate);
    if (r.dataDescriptor) {
        // data descriptor 模式：本地头 crc/sizes 写 0，真实值在数据后的描述符中
        writeLE32(out, 0);
        writeLE32(out, 0);
        writeLE32(out, 0);
    } else {
        writeLE32(out, r.crc);
        writeLE32(out, r.compSize);
        writeLE32(out, r.uncompSize);
    }
    writeLE16(out, static_cast<uint16_t>(name.size()));
    writeLE16(out, static_cast<uint16_t>(e.extraField.size()));
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), e.extraField.begin(), e.extraField.end());
}

// 追加中央目录文件头
void appendCentralHeader(std::vector<uint8_t>& out, const std::string& name,
                         const ZipEntry& e, const Resolved& r, uint32_t localOffset) {
    writeLE32(out, SIG_CENTRAL_DIR);
    writeLE16(out, e.createVersion);                   // version_made_by
    writeLE16(out, e.extractVersion);                  // version_needed
    writeLE16(out, e.flags);
    writeLE16(out, r.method);
    writeLE16(out, e.modTime);
    writeLE16(out, e.modDate);
    writeLE32(out, r.crc);
    writeLE32(out, r.compSize);
    writeLE32(out, r.uncompSize);
    writeLE16(out, static_cast<uint16_t>(name.size()));
    writeLE16(out, static_cast<uint16_t>(e.extraField.size()));
    writeLE16(out, 0);                                 // comment_len
    writeLE16(out, 0);                                 // disk_start
    writeLE16(out, 0);                                 // internal_attr
    writeLE32(out, 0);                                 // external_attr
    writeLE32(out, localOffset);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), e.extraField.begin(), e.extraField.end());
    // 无 comment
}

// 追加 EOCD
void appendEOCD(std::vector<uint8_t>& out, uint32_t cdOffset, uint32_t cdSize,
                uint16_t entryCount) {
    writeLE32(out, SIG_EOCD);
    writeLE16(out, 0);                  // disk_num
    writeLE16(out, 0);                  // cd_disk
    writeLE16(out, entryCount);         // cd_entries_disk
    writeLE16(out, entryCount);         // cd_entries
    writeLE32(out, cdSize);
    writeLE32(out, cdOffset);
    writeLE16(out, 0);                  // comment_len
}

} // namespace

// ============ load ============

std::unique_ptr<Document> Document::load(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!readFile(path, buf)) return nullptr;
    if (buf.size() < EOCD_MIN_SIZE) return nullptr;

    size_t eocd = findEOCD(buf);
    if (eocd == NPOS) return nullptr;

    const uint8_t* base = buf.data();
    // EOCD: cd_entries(10) cd_size(12) cd_offset(16)
    uint16_t cdEntries = readLE16(base + eocd + 10);
    uint32_t cdOffset  = readLE32(base + eocd + 16);

    auto doc = std::make_unique<Document>();

    size_t p = cdOffset;
    for (uint16_t i = 0; i < cdEntries; ++i) {
        // 读取中央目录条目
        if (p + CDH_FIXED_SIZE > buf.size()) return nullptr;
        if (readLE32(base + p) != SIG_CENTRAL_DIR) return nullptr;

        uint16_t versionMadeBy  = readLE16(base + p + 4);
        uint16_t versionNeeded  = readLE16(base + p + 6);
        uint16_t flags          = readLE16(base + p + 8);
        uint16_t method         = readLE16(base + p + 10);
        uint16_t modTime        = readLE16(base + p + 12);
        uint16_t modDate        = readLE16(base + p + 14);
        uint32_t crc            = readLE32(base + p + 16);
        uint32_t compSize       = readLE32(base + p + 20);
        uint32_t uncompSize     = readLE32(base + p + 24);
        uint16_t nameLen        = readLE16(base + p + 28);
        uint16_t extraLen       = readLE16(base + p + 30);
        uint16_t commentLen     = readLE16(base + p + 32);
        uint32_t localOffset    = readLE32(base + p + 42);

        size_t remaining = buf.size() - p - CDH_FIXED_SIZE;
        if (static_cast<size_t>(nameLen) + extraLen + commentLen > remaining) return nullptr;
        size_t entryEnd = p + CDH_FIXED_SIZE + nameLen + extraLen + commentLen;

        std::string name(reinterpret_cast<const char*>(base + p + CDH_FIXED_SIZE), nameLen);

        // 读取本地文件头，获取本地 extra field 与数据区起点
        if (localOffset > buf.size() || buf.size() - localOffset < LFH_FIXED_SIZE) return nullptr;
        if (readLE32(base + localOffset) != SIG_LOCAL_HEADER) return nullptr;
        uint16_t localNameLen  = readLE16(base + localOffset + 26);
        uint16_t localExtraLen = readLE16(base + localOffset + 28);

        size_t remainingLocal = buf.size() - localOffset - LFH_FIXED_SIZE;
        if (static_cast<size_t>(localNameLen) + localExtraLen > remainingLocal) return nullptr;
        size_t dataStart = static_cast<size_t>(localOffset) + LFH_FIXED_SIZE + localNameLen + localExtraLen;
        if (compSize > buf.size() - dataStart) return nullptr;

        // 本地 extra field（保留，写入本地头与中央目录均使用它）
        std::vector<uint8_t> localExtra;
        if (localExtraLen > 0) {
            localExtra.assign(base + localOffset + LFH_FIXED_SIZE + localNameLen,
                              base + localOffset + LFH_FIXED_SIZE + localNameLen + localExtraLen);
        }

        // 切片保存原始压缩字节
        std::vector<uint8_t> compData(base + dataStart, base + dataStart + compSize);

        // 解压得到未压缩内容
        std::vector<uint8_t> data;
        bool dir = isDirectory(name);
        if (!dir) {
            if (method == 8) {
                data = decompressDeflateRaw(compData.data(), compData.size(), uncompSize);
            } else if (method == 0) {
                data = compData;
            }
            // 其他 method：保留原始字节但不解压（data 留空）
        }

        // data descriptor 处理：flags bit 3 置位时，本地头 crc/sizes 为 0，
        // 真实值在压缩数据之后的描述符中。保留描述符字节以实现字节级保留。
        if ((flags & 0x0008) != 0) {
            size_t ddStart = dataStart + compSize;
            size_t ddLen = 0;
            // 描述符可能带签名 0x08074b50（共 16 字节）。用 comp/uncomp 校验确认长度。
            if (ddStart + 16 <= buf.size() &&
                readLE32(base + ddStart) == 0x08074b50 &&
                readLE32(base + ddStart + 8) == compSize &&
                readLE32(base + ddStart + 12) == uncompSize) {
                ddLen = 16;
            } else if (ddStart + 12 <= buf.size() &&
                       readLE32(base + ddStart + 4) == compSize &&
                       readLE32(base + ddStart + 8) == uncompSize) {
                ddLen = 12;
            }
            if (ddLen > 0 && ddStart + ddLen <= buf.size()) {
                compData.insert(compData.end(), base + ddStart, base + ddStart + ddLen);
            }
        }

        ZipEntry e;
        e.name = name;
        e.data = std::move(data);
        e.preserveRaw = true;
        e.method = method;
        e.createVersion = versionMadeBy;
        e.extractVersion = versionNeeded;
        e.flags = flags;
        e.crc = crc;
        e.compressedSize = compSize;
        e.uncompressedSize = uncompSize;
        e.extraField = std::move(localExtra);
        e.compressedData = std::move(compData);
        e.modTime = modTime;
        e.modDate = modDate;

        doc->entries_[name] = std::move(e);
        doc->originalOrder_.push_back(name);

        p = entryEnd;
    }

    return doc;
}

// ============ 查询 ============

bool Document::get(const std::string& name, std::vector<uint8_t>& out) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return false;
    out = it->second.data;
    return true;
}

ZipEntry* Document::getEntryMut(const std::string& name) {
    auto it = entries_.find(name);
    if (it == entries_.end()) return nullptr;
    return &it->second;
}

const ZipEntry* Document::getEntry(const std::string& name) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return nullptr;
    return &it->second;
}

// ============ 修改 ============

void Document::set(const std::string& name, const std::vector<uint8_t>& data) {
    ZipEntry e;
    e.name = name;
    e.data = data;
    e.preserveRaw = false;
    e.method = 8;               // Deflate，写入时做 raw deflate 压缩
    e.createVersion = 20;
    e.extractVersion = 20;
    e.flags = 0;
    e.modTime = 0;
    e.modDate = 0x0021;         // 1980-01-01，保证输出可复现
    entries_[name] = std::move(e);
}

void Document::setEntry(const std::string& name, const ZipEntry& entry) {
    ZipEntry e = entry;
    e.name = name;
    entries_[name] = std::move(e);
}

void Document::remove(const std::string& name) {
    entries_.erase(name);
}

std::vector<std::string> Document::list() const {
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const auto& kv : entries_) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}

// ============ write ============

bool Document::write(const std::string& path) {
    // 1. 构建写入用条目集合（含补全的父目录条目）
    std::map<std::string, ZipEntry> work;
    for (const auto& kv : entries_) work[kv.first] = kv.second;
    for (const auto& kv : entries_) addDirectoryEntries(work, kv.first);

    // 2. 确定写入顺序：
    //    先按 originalOrder_ 写 preserveRaw=true 的条目（保持原序），
    //    再按字母序追加新条目（含被 set 覆盖的与补全的目录）。
    std::vector<std::string> writeOrder;
    std::set<std::string> written;
    for (const auto& name : originalOrder_) {
        auto it = work.find(name);
        if (it != work.end() && it->second.preserveRaw) {
            writeOrder.push_back(name);
            written.insert(name);
        }
    }
    std::vector<std::string> rest;
    for (const auto& kv : work) {
        if (written.find(kv.first) == written.end()) {
            rest.push_back(kv.first);
        }
    }
    std::sort(rest.begin(), rest.end());
    for (auto& name : rest) writeOrder.push_back(std::move(name));

    // 3. 预解析每个条目的实际写入数据
    std::map<std::string, Resolved> resolved;
    for (const auto& name : writeOrder) {
        resolved[name] = resolveEntry(name, work[name]);
    }

    // 4. 构建输出缓冲
    std::vector<uint8_t> out;
    std::map<std::string, uint32_t> offsets;
    for (const auto& name : writeOrder) {
        offsets[name] = static_cast<uint32_t>(out.size());
        appendLocalHeader(out, name, work[name], resolved[name]);
        const std::vector<uint8_t>& d = resolved[name].data;
        out.insert(out.end(), d.begin(), d.end());
    }

    // 5. 中央目录
    uint32_t cdOffset = static_cast<uint32_t>(out.size());
    for (const auto& name : writeOrder) {
        appendCentralHeader(out, name, work[name], resolved[name], offsets[name]);
    }
    uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdOffset;

    // 6. EOCD
    appendEOCD(out, cdOffset, cdSize, static_cast<uint16_t>(writeOrder.size()));

    // 7. 写临时文件 + 原子 rename
    std::string tempPath = path + ".tmp";
    std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!out.empty()) {
        f.write(reinterpret_cast<const char*>(out.data()),
                static_cast<std::streamsize>(out.size()));
    }
    f.close();
    if (!f) {
        std::remove(tempPath.c_str());
        return false;
    }
    if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
        std::remove(tempPath.c_str());
        return false;
    }
    return true;
}

} // namespace co
