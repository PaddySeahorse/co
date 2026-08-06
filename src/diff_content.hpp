// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// co diff 内容级差异：对两个 commit 之间发生变化的归档条目做文本内容 diff，
// 输出带 +/- 前缀的差异行；二进制条目输出 @ 占位行。
// 文件级 A/D/M 列表（--status 模式）仍由 status.hpp 的 diffCommits 提供。

#pragma once
#include "zip.hpp"
#include <string>

namespace co {

// 渲染 ref-a → ref-b 的内容差异，追加到 out。
// 无变化或 ref 解析失败时 out 保持为空。
void renderCommitDiff(Document& doc, const std::string& refA,
                      const std::string& refB, std::string& out);

} // namespace co
