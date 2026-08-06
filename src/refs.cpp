// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "refs.hpp"
#include "objectstore.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace co {

namespace {

// 去除首尾空白
std::string trimSpace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' ||
                                s[start] == '\n' || s[start] == '\r' ||
                                s[start] == '\v' || s[start] == '\f')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\n' || s[end - 1] == '\r' ||
                           s[end - 1] == '\v' || s[end - 1] == '\f')) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string branchPath(const std::string& name) {
    return std::string(kRefsHeadsDir) + "/" + name;
}

} // namespace

bool isValidBranchName(const std::string& name) {
    if (name.empty()) return false;
    if (name.size() > 255) return false;
    if (name == "HEAD") return false;
    if (name.front() == '.') return false;
    if (name.back() == '.') return false;
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".lock") == 0) return false;
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '~' || c == '^' || c == ':' || c == '?' ||
            c == '*' || c == '[' || c == '\\' || c == '/' ||
            c == '`' || c == '\0') {
            return false;
        }
        if (c == 0x7f) return false;
        if (static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

std::vector<std::string> listBranches(const Document& doc) {
    std::vector<std::string> branches;
    std::string prefix = std::string(kRefsHeadsDir) + "/";
    for (const auto& name : doc.list()) {
        if (name.size() < prefix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (!name.empty() && name.back() == '/') continue;
        std::string branchName = name.substr(prefix.size());
        if (branchName.empty()) continue;
        if (branchName.find('/') != std::string::npos) continue;
        branches.push_back(branchName);
    }
    std::sort(branches.begin(), branches.end());
    return branches;
}

std::string getBranchHash(const Document& doc, const std::string& name) {
    if (!isValidBranchName(name)) return "";
    std::vector<uint8_t> data;
    if (!doc.get(branchPath(name), data)) return "";
    std::string s(data.begin(), data.end());
    return trimSpace(s);
}

bool setBranch(Document& doc, const std::string& name, const std::string& hash) {
    if (!isValidBranchName(name)) return false;
    if (hash.empty()) return false;
    std::string s = hash + "\n";
    std::vector<uint8_t> data(s.begin(), s.end());
    doc.set(branchPath(name), data);
    return true;
}

bool deleteBranch(Document& doc, const std::string& name) {
    if (!isValidBranchName(name)) return false;
    std::vector<uint8_t> tmp;
    if (!doc.get(branchPath(name), tmp)) return false;
    doc.remove(branchPath(name));
    return true;
}

HeadRef resolveHead(const Document& doc) {
    HeadRef r;
    std::vector<uint8_t> data;
    if (!doc.get(kHeadFile, data)) {
        r.hashPresent = false;
        return r;
    }
    std::string s(data.begin(), data.end());
    std::string content = trimSpace(s);
    if (content.empty()) {
        r.hashPresent = false;
        return r;
    }
    r.hashPresent = true;

    const size_t prefixLen = std::string(kRefPrefix).size();
    if (content.compare(0, prefixLen, kRefPrefix) == 0) {
        std::string ref = trimSpace(content.substr(prefixLen));
        const std::string headPrefix = std::string(kRefsHeadsDir) + "/";
        if (ref.compare(0, headPrefix.size(), headPrefix) == 0) {
            std::string branch = ref.substr(headPrefix.size());
            if (isValidBranchName(branch)) {
                r.symbolic = true;
                r.branch = branch;
                r.hash = getBranchHash(doc, branch);
                return r;
            }
        }
        // 非 refs/heads/ 的符号引用：悬空，按分离处理
        r.hash = content;
        return r;
    }

    r.hash = content;
    return r;
}

std::string headCommitHash(const Document& doc) {
    return resolveHead(doc).hash;
}

std::string currentBranch(const Document& doc) {
    HeadRef r = resolveHead(doc);
    return r.symbolic ? r.branch : "";
}

bool attachHead(Document& doc, const std::string& branch) {
    if (!isValidBranchName(branch)) return false;
    std::vector<uint8_t> tmp;
    if (!doc.get(branchPath(branch), tmp)) return false;
    std::string s = std::string(kRefPrefix) + std::string(kRefsHeadsDir) + "/" + branch + "\n";
    std::vector<uint8_t> data(s.begin(), s.end());
    doc.set(kHeadFile, data);
    return true;
}

void detachHead(Document& doc, const std::string& hash) {
    Store store(doc);
    store.setHead(hash);
}

std::string advanceHead(Document& doc, const std::string& newHash) {
    HeadRef r = resolveHead(doc);
    if (r.symbolic && !r.branch.empty()) {
        if (setBranch(doc, r.branch, newHash)) {
            return newHash;
        }
    }
    // 无分支仓库的首次提交：自动创建默认分支并符号指向
    if (!r.symbolic && !r.hashPresent && listBranches(doc).empty()) {
        if (setBranch(doc, kDefaultBranch, newHash) && attachHead(doc, kDefaultBranch)) {
            return newHash;
        }
    }
    Store store(doc);
    store.setHead(newHash);
    return newHash;
}

} // namespace co
