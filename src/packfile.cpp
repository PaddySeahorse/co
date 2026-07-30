// SPDX-License-Identifier: GPL-3.0-or-later

#include "packfile.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace co {

namespace {

// pack 文件常量
constexpr const char* kPackMagic = "PACK";
constexpr uint32_t kPackVersion = 2;

// idx 文件 magic 字节: 0xff 0x74 0x4f 0x63
constexpr uint8_t kIdxMagic[4] = {0xff, 0x74, 0x4f, 0x63};

// 对象类型编号
constexpr uint8_t kObjCommit = 1;
constexpr uint8_t kObjTree = 2;
constexpr uint8_t kObjBlob = 3;

// 类型名 -> pack type 编号，未知返回 0
uint8_t typeToPackType(const std::string& typeName) {
    if (typeName == "commit") return kObjCommit;
    if (typeName == "tree") return kObjTree;
    if (typeName == "blob") return kObjBlob;
    return 0;
}

// pack type 编号 -> 类型名，未知返回空串
std::string packTypeToName(uint8_t packType) {
    switch (packType) {
        case kObjCommit: return "commit";
        case kObjTree: return "tree";
        case kObjBlob: return "blob";
        default: return "";
    }
}

// 写入 pack 索引文件（内部函数）
bool writePackIndex(Document& doc, const PackIndex& index, const std::string& packName) {
    std::vector<uint8_t> idxBuf;

    // magic
    for (size_t i = 0; i < 4; ++i) idxBuf.push_back(kIdxMagic[i]);
    // version = 2
    writeBE32(idxBuf, 2);

    // fanout table: 256 个 uint32（累积计数）
    std::vector<uint32_t> fanout(256, 0);
    for (const auto& hash : index.hashes) {
        if (hash.size() < 2) continue;
        std::vector<uint8_t> b = hexDecode(hash.substr(0, 2));
        if (!b.empty()) {
            uint8_t firstByte = b[0];
            for (int i = static_cast<int>(firstByte); i < 256; ++i) {
                fanout[static_cast<size_t>(i)]++;
            }
        }
    }
    for (uint32_t count : fanout) writeBE32(idxBuf, count);

    // sha 数组（每个 kHashLen 字节）
    for (const auto& hash : index.hashes) {
        std::vector<uint8_t> b = hexDecode(hash);
        idxBuf.insert(idxBuf.end(), b.begin(), b.end());
    }

    // crc 数组
    for (uint32_t crc : index.crcs) writeBE32(idxBuf, crc);

    // offset 数组
    for (uint32_t offset : index.offsets) writeBE32(idxBuf, offset);

    // kHashLen 字节摘要 trailer
    std::vector<uint8_t> idxHash = hashDigest(idxBuf);
    idxBuf.insert(idxBuf.end(), idxHash.begin(), idxHash.end());

    std::string idxPath = std::string(kPackDir) + "/" + packName + ".idx";
    doc.set(idxPath, idxBuf);
    return true;
}

} // namespace

// ============ writePack ============

bool writePack(Document& doc, const std::vector<PackedObject>& objects, const std::string& packName) {
    // 按 hash 排序
    std::vector<PackedObject> sorted = objects;
    std::sort(sorted.begin(), sorted.end(), [](const PackedObject& a, const PackedObject& b) {
        return a.hash < b.hash;
    });

    std::vector<uint8_t> packBuf;

    // 头部：magic + version + count
    for (size_t i = 0; i < 4; ++i) packBuf.push_back(static_cast<uint8_t>(kPackMagic[i]));
    writeBE32(packBuf, kPackVersion);
    writeBE32(packBuf, static_cast<uint32_t>(sorted.size()));

    PackIndex index;
    index.hashes.resize(sorted.size());
    index.crcs.resize(sorted.size());
    index.offsets.resize(sorted.size());

    for (size_t i = 0; i < sorted.size(); ++i) {
        const PackedObject& obj = sorted[i];
        uint32_t offset = static_cast<uint32_t>(packBuf.size());
        index.hashes[i] = obj.hash;
        index.offsets[i] = offset;

        size_t objStart = packBuf.size();

        uint8_t packType = typeToPackType(obj.type);
        uint64_t size = static_cast<uint64_t>(obj.data.size());

        // 变长编码 type+size：第一个字节高 4 位是 type，低 4 位是 size 最低 4 位
        uint64_t typeAndSize = (static_cast<uint64_t>(packType) << 4) | (size & 0x0F);
        uint64_t sizeBits = size >> 4;
        while (sizeBits > 0) {
            packBuf.push_back(static_cast<uint8_t>(typeAndSize | 0x80));
            typeAndSize = sizeBits & 0x7F;
            sizeBits >>= 7;
        }
        packBuf.push_back(static_cast<uint8_t>(typeAndSize));

        // zlib 压缩对象数据
        std::vector<uint8_t> compressed = compressZlib(obj.data);
        packBuf.insert(packBuf.end(), compressed.begin(), compressed.end());

        // CRC32 = 对象区（type/size 头 + 压缩数据）的校验
        index.crcs[i] = crc32IEEE(packBuf.data() + objStart, packBuf.size() - objStart);
    }

    // kHashLen 字节摘要 trailer
    std::vector<uint8_t> packHash = hashDigest(packBuf);
    packBuf.insert(packBuf.end(), packHash.begin(), packHash.end());

    // 写入 pack 文件
    std::string packPath = std::string(kPackDir) + "/" + packName + ".pack";
    doc.set(packPath, packBuf);

    // 写入 idx 文件
    writePackIndex(doc, index, packName);
    return true;
}

// ============ readPackIndex ============

bool readPackIndex(const Document& doc, const std::string& packName, PackIndex& out) {
    std::string idxPath = std::string(kPackDir) + "/" + packName + ".idx";
    std::vector<uint8_t> data;
    if (!doc.get(idxPath, data)) return false;

    if (data.size() < 8) return false;

    // 验证 magic
    if (data[0] != kIdxMagic[0] || data[1] != kIdxMagic[1] ||
        data[2] != kIdxMagic[2] || data[3] != kIdxMagic[3]) {
        return false;
    }

    // 验证 version
    uint32_t version = readBE32(data.data() + 4);
    if (version != 2) return false;

    size_t pos = 8;

    // 读 fanout table（256 个 uint32）
    if (pos + 256 * 4 > data.size()) return false;
    std::vector<uint32_t> fanout(256);
    for (size_t i = 0; i < fanout.size(); ++i) {
        fanout[i] = readBE32(data.data() + pos);
        pos += 4;
    }

    uint32_t objCount = fanout[255];
    if (objCount == 0) {
        out = PackIndex{};
        return true;
    }

    // 验证整个索引文件的最小预期大小，防止 objCount 带来的整数溢出与 OOM 漏洞
    uint64_t expectedSize = 1032 + static_cast<uint64_t>(objCount) * (kHashLen + 8) + kHashLen;
    if (expectedSize > data.size()) return false;

    // 读 sha 数组（每个 kHashLen 字节）
    out.hashes.resize(objCount);
    for (uint32_t i = 0; i < objCount; ++i) {
        out.hashes[i] = hexEncode(data.data() + pos, kHashLen);
        pos += kHashLen;
    }

    // 读 crc 数组
    out.crcs.resize(objCount);
    for (uint32_t i = 0; i < objCount; ++i) {
        out.crcs[i] = readBE32(data.data() + pos);
        pos += 4;
    }

    // 读 offset 数组
    out.offsets.resize(objCount);
    for (uint32_t i = 0; i < objCount; ++i) {
        out.offsets[i] = readBE32(data.data() + pos);
        pos += 4;
    }

    return true;
}

// ============ readPackedObject ============

bool readPackedObject(const Document& doc, const std::string& packName,
                      const std::string& hash, std::string& outType,
                      std::vector<uint8_t>& outData) {
    PackIndex index;
    if (!readPackIndex(doc, packName, index)) return false;

    // 在索引中查找 hash
    bool found = false;
    size_t idx = 0;
    for (size_t i = 0; i < index.hashes.size(); ++i) {
        if (index.hashes[i] == hash) {
            idx = i;
            found = true;
            break;
        }
    }
    if (!found) return false;

    uint32_t offset = index.offsets[idx];

    std::string packPath = std::string(kPackDir) + "/" + packName + ".pack";
    std::vector<uint8_t> data;
    if (!doc.get(packPath, data)) return false;

    if (static_cast<size_t>(offset) >= data.size()) return false;

    size_t pos = offset;
    uint8_t c = data[pos]; ++pos;
    uint8_t objType = static_cast<uint8_t>((c >> 4) & 0x07);
    uint64_t size = static_cast<uint64_t>(c & 0x0F);
    uint32_t shift = 4;
    while ((c & 0x80) != 0) {
        if (pos >= data.size()) return false;
        if (shift >= 60) return false; // 防止位移溢出未定义行为
        c = data[pos]; ++pos;
        size |= static_cast<uint64_t>(c & 0x7F) << shift;
        shift += 7;
    }
    (void)size;  // size 已解析但未使用（与 Go 行为一致，用于推进 pos）

    // zlib 解压剩余数据
    if (pos >= data.size()) return false;
    std::vector<uint8_t> decompressed;
    try {
        decompressed = decompressZlib(data.data() + pos, data.size() - pos);
    } catch (...) {
        return false;
    }

    std::string typeName = packTypeToName(objType);
    if (typeName.empty()) return false;

    outType = typeName;
    outData = std::move(decompressed);
    return true;
}

// ============ listPackFiles ============

std::vector<std::string> listPackFiles(const Document& doc) {
    std::vector<std::string> packs;
    std::string prefix = std::string(kPackDir) + "/";
    std::string suffix = ".pack";
    for (const auto& name : doc.list()) {
        if (name.size() >= prefix.size() + suffix.size() &&
            name.compare(0, prefix.size(), prefix) == 0 &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            std::string baseName = name.substr(prefix.size(),
                                               name.size() - prefix.size() - suffix.size());
            packs.push_back(baseName);
        }
    }
    return packs;
}

} // namespace co
