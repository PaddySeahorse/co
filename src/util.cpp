// SPDX-License-Identifier: GPL-3.0-or-later

#include "util.hpp"

#include <zlib.h>
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

// ============ Zlib 压缩/解压（zlib 格式，带 2 字节头 + Adler32 尾） ============

std::vector<uint8_t> compressZlib(const uint8_t* data, size_t len) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    // windowBits=MAX_WBITS(15) 表示 zlib 格式（RFC 1950）
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2(zlib) failed");
    }
    // zlib 的 next_in 是非 const 指针，这里 const_cast 是标准做法
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
            // 输出缓冲区满，扩容后继续
            size_t oldSize = out.size();
            out.resize(oldSize * 2);
            strm.next_out = out.data() + oldSize;
            strm.avail_out = static_cast<uInt>(out.size() - oldSize);
        }
    } while (ret == Z_OK);

    deflateEnd(&strm);
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("compressZlib: deflate did not finish");
    }
    out.resize(strm.total_out);
    return out;
}

std::vector<uint8_t> compressZlib(const std::vector<uint8_t>& data) {
    return compressZlib(data.data(), data.size());
}

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
