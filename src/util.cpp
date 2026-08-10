// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "util.hpp"
#include "zstd_dict.hpp"

#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>
#include <openssl/evp.h>

#include <stdexcept>
#include <cstring>

namespace co {

// ============ SHA1 ============

std::array<uint8_t,20> sha1(const uint8_t* data, size_t len) {
    std::array<uint8_t,20> out{};
    unsigned int outLen = 0;
    // EVP_Digest 是 OpenSSL 推荐的一次性摘要接口，避免使用已废弃的 SHA1()
    if (EVP_Digest(data, len, out.data(), &outLen, EVP_sha1(), nullptr) != 1) {
        throw std::runtime_error("EVP_Digest(SHA1) failed");
    }
    return out;
}

std::array<uint8_t,20> sha1(const std::vector<uint8_t>& data) {
    return sha1(data.data(), data.size());
}

// ============ SHA256 ============

std::array<uint8_t,32> sha256(const uint8_t* data, size_t len) {
    std::array<uint8_t,32> out{};
    unsigned int outLen = 0;
    if (EVP_Digest(data, len, out.data(), &outLen, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("EVP_Digest(SHA256) failed");
    }
    return out;
}

std::array<uint8_t,32> sha256(const std::vector<uint8_t>& data) {
    return sha256(data.data(), data.size());
}

// ============ 默认摘要 + 按长度摘要 ============

std::vector<uint8_t> hashDigestByLen(const uint8_t* data, size_t len, size_t hashLen) {
    if (hashLen == 32) {
        auto h = sha256(data, len);
        return std::vector<uint8_t>(h.begin(), h.end());
    }
    auto h = sha1(data, len);
    return std::vector<uint8_t>(h.begin(), h.end());
}

std::vector<uint8_t> hashDigest(const uint8_t* data, size_t len) {
    return hashDigestByLen(data, len, kHashLen);
}

std::vector<uint8_t> hashDigest(const std::vector<uint8_t>& data) {
    return hashDigest(data.data(), data.size());
}

// ============ Hex ============

std::string hexEncode(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[(data[i] >> 4) & 0x0F]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

std::string hexEncode(const std::vector<uint8_t>& data) {
    return hexEncode(data.data(), data.size());
}

std::string hexEncode(const std::array<uint8_t,20>& data) {
    return hexEncode(data.data(), data.size());
}

std::string hexEncode(const std::array<uint8_t,32>& data) {
    return hexEncode(data.data(), data.size());
}

std::vector<uint8_t> hexDecode(const std::string& s) {
    if (s.size() % 2 != 0) {
        throw std::invalid_argument("hexDecode: odd-length string");
    }
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nibble(s[i]);
        int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::invalid_argument("hexDecode: invalid hex char");
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

// ============ Zlib 解压（zlib 格式，带 2 字节头 + Adler32 尾） ============
//
// 仅用于兼容旧仓库：对象存储已改用 zstd（见下方 compressZstd），但旧仓库
// 的 loose 对象仍是 zlib 格式，decompressAuto 对非 zstd magic 的数据走这里。
// 压缩方向不再提供 compressZlib——zlib 格式（RFC 1950）与 ZIP method=8 的
// raw DEFLATE（RFC 1951）不兼容，写入会破坏对象（issue #8）。

std::vector<uint8_t> decompressZlib(const uint8_t* data, size_t len) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    // inflateInit 默认 windowBits=15 即 zlib 格式
    if (inflateInit(&strm) != Z_OK) {
        throw std::runtime_error("inflateInit(zlib) failed");
    }
    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = static_cast<uInt>(len);

    std::vector<uint8_t> out;
    out.resize(4096);
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());

    int ret;
    do {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) {
            inflateEnd(&strm);
            throw std::runtime_error("decompressZlib: inflate error");
        }
        if (strm.avail_out == 0) {
            size_t oldSize = out.size();
            out.resize(oldSize * 2);
            strm.next_out = out.data() + oldSize;
            strm.avail_out = static_cast<uInt>(out.size() - oldSize);
        }
    } while (true);

    inflateEnd(&strm);
    out.resize(strm.total_out);
    return out;
}

std::vector<uint8_t> decompressZlib(const std::vector<uint8_t>& data) {
    return decompressZlib(data.data(), data.size());
}

// ============ Zstd 压缩/解压（带内置 OOXML 字典） ============
//
// 对象存储改用 zstd：相比 zlib 在结构化 XML 上压缩率更高，且字典对小对象
// （commit/tree、小 blob）增益显著。zstd frame 的 magic（28 B5 2F FD）与 zlib
// 头（0x78..）可区分，故 decompressAuto 按语言分流以兼容旧的 zlib 数据。

namespace {

// 字典上下文单例（懒构造、线程不安全——co 单线程 CLI 可接受）
struct ZstdDictCtx {
    ZSTD_CDict* cdict = nullptr;
    ZSTD_DDict* ddict = nullptr;
    ZSTD_CCtx* cctx = nullptr;
    ZSTD_DCtx* dctx = nullptr;
    ZstdDictCtx() {
        cdict = ZSTD_createCDict(kZstdDict, kZstdDictSize, 19);
        ddict = ZSTD_createDDict(kZstdDict, kZstdDictSize);
        cctx = ZSTD_createCCtx();
        dctx = ZSTD_createDCtx();
        if (!cdict || !ddict || !cctx || !dctx) {
            throw std::runtime_error("zstd dict context init failed");
        }
    }
    ~ZstdDictCtx() {
        ZSTD_freeCDict(cdict);
        ZSTD_freeDDict(ddict);
        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
    }
};

ZstdDictCtx& zstdCtx() {
    static ZstdDictCtx inst;
    return inst;
}

} // namespace

std::vector<uint8_t> compressZstd(const uint8_t* data, size_t len) {
    ZstdDictCtx& c = zstdCtx();
    // 字典压缩对小数据最优；对大数据先估压缩界，按需扩容。
    size_t bound = ZSTD_compressBound(len);
    std::vector<uint8_t> out(bound);
    size_t n = ZSTD_compress_usingCDict(c.cctx, out.data(), out.size(),
                                         data, len, c.cdict);
    if (ZSTD_isError(n)) {
        throw std::runtime_error(std::string("compressZstd failed: ") +
                                 ZSTD_getErrorName(n));
    }
    out.resize(n);
    return out;
}

std::vector<uint8_t> compressZstd(const std::vector<uint8_t>& data) {
    return compressZstd(data.data(), data.size());
}

std::vector<uint8_t> decompressZstd(const uint8_t* data, size_t len) {
    // 一律采用流式解压：packfile 中多个 zstd frame 顺序拼合，紧随其后可能
    // 是下一个对象记录或 trailer。单次 ZSTD_decompress_usingDDict 要求 srcSize
    // 精确等于一个 frame，余下数据会引发失败。ZSTD_decompressStream 在 frame
    // 末尾自然停止，可正确处理拼接场景。
    ZstdDictCtx& c = zstdCtx();
    size_t r = ZSTD_DCtx_refDDict(c.dctx, c.ddict);
    if (ZSTD_isError(r)) {
        throw std::runtime_error(std::string("ZSTD_DCtx_refDDict failed: ") +
                                 ZSTD_getErrorName(r));
    }
    std::vector<uint8_t> out;
    out.resize(1 << 16);
    ZSTD_inBuffer in{data, len, 0};
    ZSTD_outBuffer outb{out.data(), out.size(), 0};
    while (in.pos < in.size) {
        size_t n = ZSTD_decompressStream(c.dctx, &outb, &in);
        if (ZSTD_isError(n)) {
            ZSTD_DCtx_reset(c.dctx, ZSTD_reset_session_only);
            throw std::runtime_error(std::string("decompressZstd failed: ") +
                                     ZSTD_getErrorName(n));
        }
        // n==0 表示 frame 完成；n>0 表示还期望更多输入（多 frame 拼接）
        if (n == 0) break;
        if (outb.pos == outb.size) {
            size_t old = out.size();
            out.resize(old * 2);
            outb.dst = out.data();
            outb.size = out.size();
        }
    }
    out.resize(outb.pos);
    ZSTD_DCtx_reset(c.dctx, ZSTD_reset_session_only);
    return out;
}

std::vector<uint8_t> decompressZstd(const std::vector<uint8_t>& data) {
    return decompressZstd(data.data(), data.size());
}

// ============ 自动分流解压：zstd 抑或 zlib ============
//
// 读取对象时无法提前知道是新增（zstd）还是遗留（zlib）数据。按 frame magic 判断：
//   - zstd frame: 首四字节为 0x28 0xB5 0x2F 0xFD
//   - 其余：当作 zlib 处理（zlib 头首字节 0x78 子集）
std::vector<uint8_t> decompressAuto(const uint8_t* data, size_t len) {
    if (len >= 4 && data[0] == 0x28 && data[1] == 0xB5 &&
        data[2] == 0x2F && data[3] == 0xFD) {
        return decompressZstd(data, len);
    }
    return decompressZlib(data, len);
}

std::vector<uint8_t> decompressAuto(const std::vector<uint8_t>& data) {
    return decompressAuto(data.data(), data.size());
}

// ============ Raw DEFLATE（无 zlib 头，ZIP method=8 用） ============

std::vector<uint8_t> compressDeflateRaw(const uint8_t* data, size_t len) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    // windowBits 为负数表示 raw deflate（RFC 1951），无 zlib 头/尾
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2(raw) failed");
    }
    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = static_cast<uInt>(len);

    std::vector<uint8_t> out;
    out.resize(4096);
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());

    int ret;
    do {
        ret = deflate(&strm, Z_FINISH);
        if (strm.avail_out == 0) {
            size_t oldSize = out.size();
            out.resize(oldSize * 2);
            strm.next_out = out.data() + oldSize;
            strm.avail_out = static_cast<uInt>(out.size() - oldSize);
        }
    } while (ret == Z_OK);

    deflateEnd(&strm);
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("compressDeflateRaw: deflate did not finish");
    }
    out.resize(strm.total_out);
    return out;
}

std::vector<uint8_t> compressDeflateRaw(const std::vector<uint8_t>& data) {
    return compressDeflateRaw(data.data(), data.size());
}

std::vector<uint8_t> decompressDeflateRaw(const uint8_t* data, size_t len, size_t outSizeHint) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    // windowBits 为负数表示 raw deflate
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("inflateInit2(raw) failed");
    }
    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = static_cast<uInt>(len);

    std::vector<uint8_t> out;
    out.resize(outSizeHint > 0 ? outSizeHint : 4096);
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());

    int ret;
    do {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) {
            inflateEnd(&strm);
            throw std::runtime_error("decompressDeflateRaw: inflate error");
        }
        if (strm.avail_out == 0) {
            size_t oldSize = out.size();
            out.resize(oldSize * 2);
            strm.next_out = out.data() + oldSize;
            strm.avail_out = static_cast<uInt>(out.size() - oldSize);
        }
    } while (true);

    inflateEnd(&strm);
    out.resize(strm.total_out);
    return out;
}

// ============ CRC32 (IEEE) ============

uint32_t crc32IEEE(const uint8_t* data, size_t len) {
    return static_cast<uint32_t>(crc32(0L, data, static_cast<uInt>(len)));
}

uint32_t crc32IEEE(const std::vector<uint8_t>& data) {
    return crc32IEEE(data.data(), data.size());
}

// ============ 大端读写 ============

uint16_t readBE16(const uint8_t* p) {
    return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

uint32_t readBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t readBE64(const uint8_t* p) {
    return (uint64_t(readBE32(p)) << 32) | uint64_t(readBE32(p + 4));
}

void writeBE16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeBE32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeBE64(std::vector<uint8_t>& buf, uint64_t v) {
    writeBE32(buf, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
    writeBE32(buf, static_cast<uint32_t>(v & 0xFFFFFFFFu));
}

// ============ 小端读写 ============

uint16_t readLE16(const uint8_t* p) {
    return static_cast<uint16_t>((uint16_t(p[1]) << 8) | uint16_t(p[0]));
}

uint32_t readLE32(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[1]) << 8) | uint32_t(p[0]);
}

void writeLE16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void writeLE32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

} // namespace co
