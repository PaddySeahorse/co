// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <array>

namespace co {

// 哈希长度（编译期由 CO_HASH 决定）。SHA1=20，SHA256=32。
// 旧仓库默认 SHA1（向后兼容）；用 -DCO_HASH=sha256 构建可启用 SHA256。
#ifdef CO_HASH_SHA256
inline constexpr size_t kHashLen = 32;
#else
inline constexpr size_t kHashLen = 20;
#endif

// SHA1 — 使用 OpenSSL（始终编译，供 migrate 跨算法转换使用）
std::array<uint8_t,20> sha1(const uint8_t* data, size_t len);
std::array<uint8_t,20> sha1(const std::vector<uint8_t>& data);

// SHA256 — 使用 OpenSSL（始终编译，供 migrate 跨算法转换使用）
std::array<uint8_t,32> sha256(const uint8_t* data, size_t len);
std::array<uint8_t,32> sha256(const std::vector<uint8_t>& data);

// 当前构建默认摘要（kHashLen 字节）。SHA1 构建返回 SHA1，SHA256 构建返回 SHA256。
std::vector<uint8_t> hashDigest(const uint8_t* data, size_t len);
std::vector<uint8_t> hashDigest(const std::vector<uint8_t>& data);

// 按指定算法计算摘要（供 migrate 显式指定目标算法）。len 必须为 20 或 32。
std::vector<uint8_t> hashDigestByLen(const uint8_t* data, size_t len, size_t hashLen);

// Hex 编解码
std::string hexEncode(const uint8_t* data, size_t len);
std::string hexEncode(const std::vector<uint8_t>& data);
std::string hexEncode(const std::array<uint8_t,20>& data);
std::string hexEncode(const std::array<uint8_t,32>& data);
std::vector<uint8_t> hexDecode(const std::string& s);

// 对象存储压缩契约（issue #8）：
//   - loose/pack 对象一律用 zstd frame 压缩（decompressAuto 按 magic 识别）；
//   - ZIP 层对 method=8 条目使用 raw DEFLATE（RFC 1951，无 zlib 头）；
//   - 禁止把 zlib 格式（RFC 1950，0x78 头 + Adler32 尾）写入 method=8 条目：
//     ZIP 按 raw DEFLATE 解压会破坏 zlib 头，导致对象不可读。
// 仅保留解压方向以兼容旧仓库遗留的 zlib 数据；压缩方向已被 zstd 取代，
// 不再提供 compressZlib，防止重新引入上述格式不匹配。
// decompressZlib: 对应 Go 的 zlib.NewReader + io.ReadAll（旧数据兼容）
std::vector<uint8_t> decompressZlib(const uint8_t* data, size_t len);
std::vector<uint8_t> decompressZlib(const std::vector<uint8_t>& data);

// Zstd 压缩/解压（带内置 OOXML 字典）。用于对象存储（loose/pack）压缩，
// 替代 zlib 以提升结构化 XML 压缩率。ZIP 本身的 raw DEFLATE 仍由 zlib 负责。
std::vector<uint8_t> compressZstd(const uint8_t* data, size_t len);
std::vector<uint8_t> compressZstd(const std::vector<uint8_t>& data);
std::vector<uint8_t> decompressZstd(const uint8_t* data, size_t len);
std::vector<uint8_t> decompressZstd(const std::vector<uint8_t>& data);

// 自动分流解压：按 frame magic 判定 zstd 抑或 zlib，兼容旧数据。
std::vector<uint8_t> decompressAuto(const uint8_t* data, size_t len);
std::vector<uint8_t> decompressAuto(const std::vector<uint8_t>& data);

// Raw DEFLATE 压缩（无 zlib 头，用于 ZIP method=8）
std::vector<uint8_t> compressDeflateRaw(const uint8_t* data, size_t len);
std::vector<uint8_t> compressDeflateRaw(const std::vector<uint8_t>& data);
// Raw DEFLATE 解压
std::vector<uint8_t> decompressDeflateRaw(const uint8_t* data, size_t len, size_t outSizeHint = 0);

// CRC32 (IEEE, 与 Go hash/crc32.ChecksumIEEE 一致)
uint32_t crc32IEEE(const uint8_t* data, size_t len);
uint32_t crc32IEEE(const std::vector<uint8_t>& data);

// 大端读写（ZIP 中央目录、packfile 用大端）
uint16_t readBE16(const uint8_t* p);
uint32_t readBE32(const uint8_t* p);
uint64_t readBE64(const uint8_t* p);
void writeBE16(std::vector<uint8_t>& buf, uint16_t v);
void writeBE32(std::vector<uint8_t>& buf, uint32_t v);
void writeBE64(std::vector<uint8_t>& buf, uint64_t v);

// 小端读写（ZIP 本地文件头用小端）
uint16_t readLE16(const uint8_t* p);
uint32_t readLE32(const uint8_t* p);
void writeLE16(std::vector<uint8_t>& buf, uint16_t v);
void writeLE32(std::vector<uint8_t>& buf, uint32_t v);

} // namespace co
