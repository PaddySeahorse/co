// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// .co-bundle 格式与命令（对应改造清单第一、二、十章与第十二章格式规范）。
//
// 设计说明（与第十二章 12.1 的取舍）：
//   12.1 示意 bundle 内部为 objects/、refs/、manifest.json。但本仓库的 Store/
//   packfile/commit/gc 全部基于「.co/ 前缀」路径常量（.co/objects、.co/HEAD、
//   .co/objects/pack…）。为最大化复用、避免维护两套并行路径，bundle 内部直接
//   保留 .co/ 前缀布局，即 bundle ZIP 条目为：
//       .co/HEAD
//       .co/objects/xx/yyyy…
//       .co/objects/pack/pack-*.pack|.idx
//       .co/index        （若存在）
//       manifest.json    （元数据，结构遵循 12.2）
//   这样 Store(bundleDoc) 与 Store(officeDoc) 行为完全一致，export/import/
//   external 模式/verify-bundle/bundle-merge 都能直接复用现有对象存储代码。
//   manifest.json 的字段语义严格遵循 12.2。

#pragma once
#include "zip.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace co {

#ifndef CO_VERSION_STR
#define CO_VERSION_STR "dev"
#endif

inline constexpr const char* kManifestPath = "manifest.json";
inline constexpr const char* kBundleExt = ".co-bundle";

struct Manifest {
    std::string version = "1.0";     // 格式版本
    std::string sourceFilename;
    std::string sourceSha256;        // 源文件 SHA256（hex）
    std::string createdAt;           // ISO 8601 UTC，如 2025-01-15T10:30:00Z
    int64_t commitCount = 0;
    int64_t bundleSizeBytes = 0;
    std::string coVersion;           // CO 工具版本
    std::string hashAlgo;            // "sha1" 或 "sha256"（构建期决定）
};

// 序列化为 manifest.json 文本
std::string serializeManifest(const Manifest& m);
// 解析 manifest.json。失败返回 false。
bool parseManifest(const std::string& json, Manifest& out);

// 计算文件 SHA256（hex）。失败返回空串。
std::string fileSha256(const std::string& path);
// 文件大小。失败返回 -1。
int64_t fileSizeOf(const std::string& path);

struct StoreStats {
    int64_t commits = 0;
    int64_t objects = 0;   // loose + packed
};
// 统计 doc（含 .co/ 的 Document）的 commit 数与 object 数
StoreStats computeStoreStats(const Document& doc);

// 当前构建的哈希算法名
std::string currentHashAlgoName();

// ============ export（第一章） ============

struct ExportOptions {
    std::string outputPath;          // bundle 输出路径
    bool clean = false;              // 同时生成干净副本
    std::string cleanOutputPath;     // 干净副本输出路径（clean=true 时用）
    bool redact = false;             // 只保留 commit/tree，不含 blob 内容
};
// 返回 true 成功。永不改写 sourcePath。
bool exportBundle(const std::string& sourcePath, const ExportOptions& opts,
                  std::string& error);

// ============ import（第二章） ============

struct ImportOutcome {
    bool ok = false;
    bool hashMatched = false;        // manifest 源 SHA256 是否与当前文件一致
    bool injected = false;           // 是否真正写入
    std::string manifestSourceSha256;
    std::string currentSha256;
    std::string sourceFilename;
    std::string error;
};
// force: hash 不匹配仍注入。verifyOnly: 只校验不写入。
ImportOutcome importBundle(const std::string& targetPath, const std::string& bundlePath,
                           bool force, bool verifyOnly);

// ============ verify-bundle（第十章） ============

struct VerifyReport {
    bool ok = false;
    bool manifestValid = false;
    bool headValid = false;
    bool allObjectsReadable = false;
    bool packsIntact = false;
    int64_t commitCount = 0;
    int64_t objectCount = 0;
    std::string manifestVersion;
    std::string hashAlgo;
    std::string error;
};
VerifyReport verifyBundle(const std::string& bundlePath);

} // namespace co
