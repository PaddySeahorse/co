// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// co migrate（第六章）：把仓库的哈希算法从「当前构建算法」转换为「另一种算法」。
//
// 机制（务实方案，对应第六章「加编译选项 CO_HASH=sha256」+ migrate 命令）：
//   - 当前二进制按构建期算法 D（SHA1 或 SHA256）读取对象（路径/tree 解析均用 D）。
//   - migrate 用 D 读取全部对象，按「另一种算法 T」重新计算哈希并重写：
//       blob  → 重算 hash，按 T 长度重写 loose 对象；
//       tree  → 递归迁移子 blob，用 T 长度二进制哈希重建 tree，重算 tree hash；
//       commit→ 递归迁移 tree/parent，替换 tree 与 parent 行为 T 哈希，重算 commit hash。
//   - 重设 HEAD，删除旧算法 loose 对象与 pack，删除 index。
//   - 转换后仓库为 T 算法，与当前 D 二进制不互通——需用 -DCO_HASH=<T> 重新构建后使用。
//
// 这样一个 SHA1 二进制可以把 SHA1 仓库迁移为 SHA256（反之亦然），满足「短期用 SHA1，
// 长期迁移 SHA256」的目标，且新旧格式互不混淆。

#pragma once
#include <cstdint>
#include <string>

namespace co {

struct MigrateResult {
    bool ok = false;
    std::string fromAlgo;      // 源算法（当前构建算法）
    std::string toAlgo;        // 目标算法
    int64_t objectsRewritten = 0;
    int64_t commitsRewritten = 0;
    int64_t treesRewritten = 0;
    std::string newHead;
    std::string error;
};

// 迁移 path（Office 文件，内嵌 .co/）的哈希算法。直接改写该文件。
MigrateResult migrateStore(const std::string& path);

} // namespace co
