// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// co status（第三章）与 co diff（第四章示例引用）。
//
// status：报告当前文件的版本控制状态。
// diff：比较两个 commit 的 tree，列出新增/删除/修改的文件。

#pragma once
#include "zip.hpp"
#include "commit.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace co {

struct StatusInfo {
    bool hasHistory = false;
    std::string headHash;
    std::string headShort;      // 前 7 位
    std::string headMessage;
    int64_t headTimestamp = 0;
    int changedFiles = 0;       // 自上次 commit 以来变化的文件数（增/删/改）
    int64_t fileSize = 0;       // 目标文件大小（bundle 或 office 文件）
    int64_t commitCount = 0;
    int64_t objectCount = 0;
    std::string hashAlgo;
    std::string branch;         // 当前分支名（分离 HEAD 或未初始化为空）
};

// historyDoc: 存 .co/ 的 Document；contentDoc: Office 文件内容。
// 内嵌模式下两者为同一个 Document。filePath 用于报告文件大小。
StatusInfo computeStatus(const Document& historyDoc, const Document& contentDoc,
                         const std::string& filePath);

struct DiffEntry {
    std::string path;
    char status = 0;   // 'A' 新增 / 'D' 删除 / 'M' 修改
};

// 比较两个 commit 的 tree。返回按路径排序的变化列表。
std::vector<DiffEntry> diffCommits(const Document& doc,
                                   const std::string& aCommit, const std::string& bCommit);

// 解析引用：HEAD、HEAD~N、完整 hash、前缀。失败返回空。
std::string resolveRef(const Document& doc, const std::string& ref);

} // namespace co
