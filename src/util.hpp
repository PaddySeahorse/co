// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <array>

namespace co {

// SHA1 — 使用 OpenSSL
std::array<uint8_t,20> sha1(const uint8_t* data, size_t len);
std::array<uint8_t,20> sha1(const std::vector<uint8_t>& data);

// Hex 编解码
std::string hexEncode(const uint8_t* data, size_t len);
std::string hexEncode(const std::vector<uint8_t>& data);
std::string hexEncode(const std::array<uint8_t,20>& data);
std::vector<uint8_t> hexDecode(const std::string& s);

// Zlib 压缩/解压（用 zlib 库，行为对齐 Go compress/zlib）
// compressZlib: 对应 Go 的 zlib.NewWriter + Write + Close
std::vector<uint8_t> compressZlib(const uint8_t* data, size_t len);
std::vector<uint8_t> compressZlib(const std::vector<uint8_t>& data);
// decompressZlib: 对应 Go 的 zlib.NewReader + io.ReadAll
std::vector<uint8_t> decompressZlib(const uint8_t* data, size_t len);
std::vector<uint8_t> decompressZlib(const std::vector<uint8_t>& data);

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
