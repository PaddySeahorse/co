// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "zip.hpp"
#include <string>
#include <vector>

namespace co {

// 分支引用目录与符号 HEAD 前缀
inline constexpr const char* kRefsHeadsDir = ".co/refs/heads";
inline constexpr const char* kRefPrefix = "ref: ";

// 空仓库首次提交时自动创建的默认分支名
inline constexpr const char* kDefaultBranch = "main";

// 分支名校验：非空、无空白与特殊字符、不以 . 开头、不以 .lock 结尾
bool isValidBranchName(const std::string& name);

// 列出所有分支名（按字母序）
std::vector<std::string> listBranches(const Document& doc);

// 读取分支指向的 commit hash（条目不存在返回空串）
std::string getBranchHash(const Document& doc, const std::string& name);

// 写/更新分支引用（name 非法则返回 false）
bool setBranch(Document& doc, const std::string& name, const std::string& hash);

// 删除分支引用（name 非法或条目不存在返回 false）
bool deleteBranch(Document& doc, const std::string& name);

// HEAD 结构：符号指向分支，或分离 hash
struct HeadRef {
    bool symbolic = false;        // true=符号指向分支，false=分离
    std::string branch;           // symbolic 时的分支名
    std::string hash;             // 解析出的 commit hash（可能为空）
    bool hashPresent = false;     // HEAD 文件是否存在且有内容
};

// 解析 HEAD：读 .co/HEAD，识别 ref: 前缀，返回解析结构（含分支解析后的 commit hash）
HeadRef resolveHead(const Document& doc);

// 读取 HEAD 的实际 commit hash（等价于 Git rev-parse HEAD，含符号解析）
std::string headCommitHash(const Document& doc);

// 当前分支名：符号 HEAD 返回分支名，分离返回空串
std::string currentBranch(const Document& doc);

// 将 HEAD 设为符号指向分支（分支不存在返回 false）
bool attachHead(Document& doc, const std::string& branch);

// 将 HEAD 设为分离 hash（写纯 hash）
void detachHead(Document& doc, const std::string& hash);

// 提交时推进当前分支：符号 HEAD 推进分支引用；分离直接写 HEAD。返回推进后指向的 hash。
std::string advanceHead(Document& doc, const std::string& newHash);

} // namespace co
