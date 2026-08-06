// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "packfile.hpp"
#include "delta.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

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
constexpr uint8_t kObjRefDelta = 7; // REF_DELTA：紧随 kHashLen 字节 base hash

// delta 启发式参数
constexpr size_t kMaxDeltaChain = 8;    // delta 链最大深度
constexpr size_t kDeltaWindow = 10;     // 滑动窗口内候选 base 数
constexpr double kDeltaThreshold = 0.7; // delta 压缩后须小于全量压缩的此比例才采用
constexpr size_t kMinDeltaSize = 64;    // 小于此值不尝试 delta，开销不值当

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

// 写入对象记录头（变长 type+size）。返回记录起点位置。
// 调用方随后追加 baseHash（若 delta）与压缩数据，并据此记录 CRC。
static size_t writeObjHeader(std::vector<uint8_t>& packBuf, uint8_t packType, uint64_t size) {
    uint64_t typeAndSize = (static_cast<uint64_t>(packType) << 4) | (size & 0x0F);
    uint64_t sizeBits = size >> 4;
    size_t start = packBuf.size();
    while (sizeBits > 0) {
        packBuf.push_back(static_cast<uint8_t>(typeAndSize | 0x80));
        typeAndSize = sizeBits & 0x7F;
        sizeBits >>= 7;
    }
    packBuf.push_back(static_cast<uint8_t>(typeAndSize));
    return start;
}

// 按 (类型优先级, size) 排序，使相同文件的相邻版本 blob 在 pack 内相邻，
// 利于 delta 窗口命中。commit/tree 优先写出，避免依赖后续 blob。
static int typeRank(const std::string& t) {
    if (t == "commit") return 0;
    if (t == "tree") return 1;
    return 2; // blob
}

} // namespace

// ============ writePack ============

bool writePack(Document& doc, const std::vector<PackedObject>& objects, const std::string& packName) {
    std::vector<PackedObject> sorted = objects;
    std::sort(sorted.begin(), sorted.end(), [](const PackedObject& a, const PackedObject& b) {
        int ra = typeRank(a.type), rb = typeRank(b.type);
        if (ra != rb) return ra < rb;
        if (a.data.size() != b.data.size()) return a.data.size() < b.data.size();
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

    // 最近写出的对象，作为同类型 delta 的候选 base。深度限制避免链过长。
    struct Recent { size_t idx; uint8_t depth; };
    std::vector<Recent> recent; // 滑动窗口

    for (size_t i = 0; i < sorted.size(); ++i) {
        PackedObject& obj = sorted[i];
        uint32_t offset = static_cast<uint32_t>(packBuf.size());
        index.hashes[i] = obj.hash;
        index.offsets[i] = offset;

        uint8_t packType = typeToPackType(obj.type);

        // 选 delta base：在同类型、窗口内、链深度合格、尺寸相近的候选里挑产生最小 delta 的。
        bool useDelta = false;
        std::vector<uint8_t> bestDelta;
        std::vector<uint8_t> bestDeltaCompressed;
        std::string bestBaseHash;
        int bestBaseDepth = 0;
        size_t bestDeltaCompressedSize = SIZE_MAX;

        if (packType != 0 && obj.data.size() >= kMinDeltaSize) {
            std::vector<uint8_t> fullCompressed = compressZstd(obj.data);
            size_t fullSize = fullCompressed.size();

            size_t winStart = recent.size() > kDeltaWindow ? recent.size() - kDeltaWindow : 0;
            for (size_t w = winStart; w < recent.size(); ++w) {
                size_t j = recent[w].idx;
                const PackedObject& cand = sorted[j];
                if (cand.type != obj.type) continue;            // 仅同类型 delta
                if (recent[w].depth >= kMaxDeltaChain) continue; // base 链已过深
                // 尺寸差距过大收益低，跳过
                if (cand.data.size() > obj.data.size() * 3 ||
                    obj.data.size() > cand.data.size() * 3) continue;

                std::vector<uint8_t> d = createDelta(cand.data, obj.data);
                std::vector<uint8_t> dcomp = compressZstd(d);
                // delta 比 REF_DELTA 头开销大（要多 kHashLen 字节 + 压缩差），仅当显著更小才用
                size_t deltaTotal = dcomp.size() + kHashLen;
                if (deltaTotal < bestDeltaCompressedSize) {
                    bestDeltaCompressedSize = deltaTotal;
                    bestDelta = std::move(d);
                    bestDeltaCompressed = std::move(dcomp);
                    bestBaseHash = cand.hash;
                    bestBaseDepth = recent[w].depth;
                }
            }

            if (!bestDelta.empty() &&
                bestDeltaCompressedSize < fullSize * kDeltaThreshold) {
                useDelta = true;
            }
        }

        size_t objStart;
        if (useDelta) {
            // REF_DELTA 记录：[type/size 头][base hash 二进制][zstd(delta)]
            objStart = writeObjHeader(packBuf, kObjRefDelta,
                                      static_cast<uint64_t>(bestDelta.size()));
            std::vector<uint8_t> baseBin = hexDecode(bestBaseHash);
            packBuf.insert(packBuf.end(), baseBin.begin(), baseBin.end());
            packBuf.insert(packBuf.end(), bestDeltaCompressed.begin(),
                           bestDeltaCompressed.end());
            obj.depth = uint8_t(bestBaseDepth + 1);
            obj.baseHash = bestBaseHash;
        } else {
            objStart = writeObjHeader(packBuf, packType,
                                      static_cast<uint64_t>(obj.data.size()));
            std::vector<uint8_t> compressed = compressZstd(obj.data);
            packBuf.insert(packBuf.end(), compressed.begin(), compressed.end());
            obj.depth = 0;
        }

        index.crcs[i] = crc32IEEE(packBuf.data() + objStart, packBuf.size() - objStart);
        recent.push_back({i, obj.depth});
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

namespace {

// 解析从 offset 开始的对象记录头，输出 objType（pack 编号）与数据负载起点 pos。
// 失败返回 false。
bool parseObjHeader(const std::vector<uint8_t>& data, size_t offset,
                    uint8_t& objType, size_t& pos) {
    if (offset >= data.size()) return false;
    size_t p = offset;
    uint8_t c = data[p]; ++p;
    objType = static_cast<uint8_t>((c >> 4) & 0x07);
    uint64_t size = static_cast<uint64_t>(c & 0x0F);
    uint32_t shift = 4;
    while ((c & 0x80) != 0) {
        if (p >= data.size()) return false;
        if (shift >= 60) return false;
        c = data[p]; ++p;
        size |= static_cast<uint64_t>(c & 0x7F) << shift;
        shift += 7;
    }
    (void)size; // 与历史行为一致：仅推进 pos，不校验 size
    pos = p;
    return true;
}

// 递归解析对象，返回其全量数据与对象类型名。
// depth 用于防御性递归上限，防构造的环或恶意深链。
bool resolveObject(const Document& doc, const std::string& packName,
                   const std::vector<uint8_t>& data, const PackIndex& index,
                   const std::string& hash, std::string& outType,
                   std::vector<uint8_t>& outData, size_t depth) {
    if (depth > 64) return false; // delta 链过深

    // 索引中找 offset
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < index.hashes.size(); ++i) {
        if (index.hashes[i] == hash) { idx = i; break; }
    }
    if (idx == SIZE_MAX) return false;
    uint32_t offset = index.offsets[idx];
    if (static_cast<size_t>(offset) >= data.size()) return false;

    uint8_t objType;
    size_t pos;
    if (!parseObjHeader(data, offset, objType, pos)) return false;

    if (objType == kObjRefDelta) {
        // REF_DELTA：紧随 kHashLen 字节 base hash，其后为 zlib(delta)
        if (pos + kHashLen > data.size()) return false;
        std::string baseHash = hexEncode(data.data() + pos, kHashLen);
        pos += kHashLen;
        if (pos >= data.size()) return false;
        std::vector<uint8_t> deltaData;
        try {
            deltaData = decompressAuto(data.data() + pos, data.size() - pos);
        } catch (...) {
            return false;
        }
        std::string baseType;
        std::vector<uint8_t> baseData;
        if (!resolveObject(doc, packName, data, index, baseHash,
                           baseType, baseData, depth + 1)) {
            return false;
        }
        if (!applyDelta(baseData, deltaData, outData)) return false;
        outType = baseType; // delta 对象继承 base 类型
        return true;
    }

    // 全量对象
    std::string typeName = packTypeToName(objType);
    if (typeName.empty()) return false;
    if (pos >= data.size()) return false;
    try {
        outData = decompressAuto(data.data() + pos, data.size() - pos);
    } catch (...) {
        return false;
    }
    outType = typeName;
    return true;
}

} // namespace

bool readPackedObject(const Document& doc, const std::string& packName,
                      const std::string& hash, std::string& outType,
                      std::vector<uint8_t>& outData) {
    PackIndex index;
    if (!readPackIndex(doc, packName, index)) return false;

    std::string packPath = std::string(kPackDir) + "/" + packName + ".pack";
    std::vector<uint8_t> data;
    if (!doc.get(packPath, data)) return false;

    return resolveObject(doc, packName, data, index, hash, outType, outData, 0);
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
