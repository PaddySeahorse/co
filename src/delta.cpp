// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "delta.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace co {

namespace {

// 滚动哈希窗口。16 字节兼顾匹配精度与代价；过小误匹配多，过大对微小编辑不敏感。
constexpr size_t kWin = 16;
// 单个哈希桶保留的最多位置数，避免热门区域退化为线性扫描。
constexpr size_t kBucketCap = 64;
// 只对不超过此大小的 base 建逐字节索引，控制内存与时间。
constexpr size_t kMaxIndexable = 32 * 1024 * 1024;
// 单次 copy 的最大长度（Git 限制 0xffffff）。
constexpr size_t kMaxCopy = 0xFFFFFF;

// 读取 varint（7-bit LE），返回值并通过 len 输出占用字节数。
// end 为缓冲区边界，越界时置 len=0 并返回 0。
uint64_t readVarint(const uint8_t* p, const uint8_t* end, size_t& len) {
    uint64_t v = 0;
    size_t shift = 0;
    len = 0;
    do {
        if (p >= end) { len = 0; return 0; }
        v |= uint64_t(*p & 0x7F) << shift;
        ++p;
        ++len;
        shift += 7;
    } while (*(p - 1) & 0x80);
    return v;
}

void writeVarint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(uint8_t(v & 0x7F) | 0x80);
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}

// 16 字节窗口哈希：FNV-1a 风格，乘以素数并折叠到 32 位。
uint32_t windowHash(const uint8_t* p) {
    uint64_t h = 1469598103934665603ULL; // FNV offset basis
    for (size_t i = 0; i < kWin; ++i) {
        h ^= uint64_t(p[i]);
        h *= 1099511628211ULL;
    }
    return uint32_t((h ^ (h >> 32)) & 0xFFFFFFFFu);
}

// 发射 insert 指令流（按 127 字节切块）
void emitInsert(std::vector<uint8_t>& out, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        size_t n = std::min<size_t>(len - off, 127);
        out.push_back(uint8_t(n)); // 非 delta 指令，最高位 0，长度即 cmd
        out.insert(out.end(), data + off, data + off + n);
        off += n;
    }
}

// 发射 copy 指令；offset/size 超过单指令容量时自动分片。
void emitCopy(std::vector<uint8_t>& out, size_t offset, size_t size) {
    while (size > 0) {
        size_t chunk = std::min(size, kMaxCopy);
        uint8_t cmd = 0x80; // copy 标志位
        std::vector<uint8_t> extra;
        // 偏移最多 4 字节
        uint32_t off = uint32_t(offset);
        for (int b = 0; b < 4; ++b) {
            uint8_t byte = uint8_t((off >> (8 * b)) & 0xFF);
            if (byte) {
                cmd |= (1 << b);
                extra.push_back(byte);
            }
        }
        // 长度最多 3 字节
        uint32_t sz = uint32_t(chunk);
        for (int b = 0; b < 3; ++b) {
            uint8_t byte = uint8_t((sz >> (8 * b)) & 0xFF);
            if (byte) {
                cmd |= (1 << (4 + b));
                extra.push_back(byte);
            }
        }
        out.push_back(cmd);
        out.insert(out.end(), extra.begin(), extra.end());
        offset += chunk;
        size -= chunk;
    }
}

// 给定 target[i..] 与 base[bpos..] 的 kWin 字节窗口已确认相等，
// 向前向后扩展得到完整匹配区间。前向扩展不超过 anchor（已发出的 insert 区）。
// 输出 copyT/copyB/len：target[copyT..copyT+len) == base[copyB..copyB+len)。
void growMatch(const std::vector<uint8_t>& base,
               const std::vector<uint8_t>& target,
               size_t i, size_t bpos, size_t anchor,
               size_t& copyT, size_t& copyB, size_t& len) {
    const size_t tn = target.size(), bn = base.size();
    // 前向：向 i 之前回退直到越过 anchor 或失配
    size_t back = 0;
    while (i - back > anchor && bpos - back < bn &&
           target[i - back - 1] == base[bpos - back - 1]) {
        ++back;
    }
    copyT = i - back;
    copyB = bpos - back;
    // 后向：从窗口末尾继续逐字节比较（窗口本身已确认）
    len = kWin;
    while (copyT + len < tn && copyB + len < bn &&
           target[copyT + len] == base[copyB + len]) {
        ++len;
    }
}

} // namespace

std::vector<uint8_t> createDelta(const std::vector<uint8_t>& base,
                                 const std::vector<uint8_t>& target) {
    std::vector<uint8_t> out;
    writeVarint(out, base.size());
    writeVarint(out, target.size());

    if (base.empty() || target.empty() || base.size() > kMaxIndexable) {
        if (!target.empty()) emitInsert(out, target.data(), target.size());
        return out;
    }

    // 建立 base 的窗口哈希表：每个字节位置一个桶
    std::unordered_map<uint32_t, std::vector<uint32_t>> index;
    index.reserve(base.size() / 2 + 1);
    if (base.size() >= kWin) {
        for (size_t i = 0; i + kWin <= base.size(); ++i) {
            uint32_t h = windowHash(base.data() + i);
            auto& bucket = index[h];
            if (bucket.size() < kBucketCap) bucket.push_back(uint32_t(i));
        }
    }

    size_t anchor = 0; // 已处理到的 target 位置（下一段 insert 的起点）
    size_t i = 0;
    size_t tn = target.size();
    bool canMatch = target.size() >= kWin && base.size() >= kWin;

    while (i + kWin <= tn) {
        if (!canMatch) break;
        uint32_t h = windowHash(target.data() + i);
        auto it = index.find(h);
        if (it == index.end()) { ++i; continue; }

        // 在候选 bucket 中找最长的、经 memcmp 确认窗口后再扩展的匹配
        size_t bestLen = 0, bestCopyT = 0, bestCopyB = 0;
        for (uint32_t bpos : it->second) {
            if (bpos + kWin > base.size()) continue;
            if (std::memcmp(target.data() + i, base.data() + bpos, kWin) != 0)
                continue; // 哈希碰撞，跳过
            size_t ct, cb, len;
            growMatch(base, target, i, bpos, anchor, ct, cb, len);
            if (len > bestLen) {
                bestLen = len; bestCopyT = ct; bestCopyB = cb;
            }
        }

        if (bestLen < kWin) { ++i; continue; }

        if (bestCopyT > anchor) {
            emitInsert(out, target.data() + anchor, bestCopyT - anchor);
        }
        emitCopy(out, bestCopyB, bestLen);

        i = bestCopyT + bestLen;
        anchor = i;
    }

    // 末尾剩余 insert
    if (anchor < tn) {
        emitInsert(out, target.data() + anchor, tn - anchor);
    }
    return out;
}

bool applyDelta(const std::vector<uint8_t>& base,
                const std::vector<uint8_t>& delta,
                std::vector<uint8_t>& out) {
    if (delta.empty()) return false;
    const uint8_t* p = delta.data();
    const uint8_t* end = p + delta.size();

    size_t len;
    uint64_t baseSize = readVarint(p, end, len); p += len;
    if (len == 0) return false;
    uint64_t targetSize = readVarint(p, end, len); p += len;
    if (len == 0) return false;
    if (baseSize != base.size()) return false;
    if (targetSize > (uint64_t(1) << 33)) return false; // 上限防溢出

    out.clear();
    out.reserve(size_t(targetSize));

    while (p < end) {
        uint8_t cmd = *p++;
        if (cmd & 0x80) {
            // copy
            size_t offset = 0;
            size_t size = 0;
            if (cmd & 0x01) { if (p >= end) return false; offset |= size_t(*p++); }
            if (cmd & 0x02) { if (p >= end) return false; offset |= size_t(*p++) << 8; }
            if (cmd & 0x04) { if (p >= end) return false; offset |= size_t(*p++) << 16; }
            if (cmd & 0x08) { if (p >= end) return false; offset |= size_t(*p++) << 24; }
            if (cmd & 0x10) { if (p >= end) return false; size |= size_t(*p++); }
            if (cmd & 0x20) { if (p >= end) return false; size |= size_t(*p++) << 8; }
            if (cmd & 0x40) { if (p >= end) return false; size |= size_t(*p++) << 16; }
            if (size == 0) size = 0x10000;
            if (offset + size > base.size()) return false;
            out.insert(out.end(), base.begin() + offset, base.begin() + offset + size);
        } else if (cmd != 0) {
            // insert
            size_t n = cmd;
            if (size_t(end - p) < n) return false;
            out.insert(out.end(), p, p + n);
            p += n;
        } else {
            return false; // cmd == 0 非法
        }
    }
    return out.size() == targetSize;
}

} // namespace co