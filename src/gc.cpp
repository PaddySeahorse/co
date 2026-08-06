// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gc.hpp"
#include "objectstore.hpp"
#include "packfile.hpp"
#include "commit.hpp"
#include "refs.hpp"
#include "util.hpp"
#include "lfs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <set>
#include <string>
#include <vector>

namespace co {

namespace {

// 判断 ASCII 空白
bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

// 去除首尾 ASCII 空白
std::string trimSpace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && isAsciiSpace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && isAsciiSpace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

// 前向声明：递归标记从 hash 可达的所有对象
bool markReachable(const Store& store, const std::string& hash,
                   std::set<std::string>& reachable,
                   std::set<std::string>& lfsReachable);

// 标记 commit 对象引用的 tree 和 parent
bool markReachableCommit(const Store& store, const std::vector<uint8_t>& content,
                         std::set<std::string>& reachable,
                         std::set<std::string>& lfsReachable) {
    std::string s(content.begin(), content.end());
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line;
        if (nl == std::string::npos) {
            line = s.substr(pos);
            pos = s.size();
        } else {
            line = s.substr(pos, nl - pos);
            pos = nl + 1;
        }
        if (line.rfind("tree ", 0) == 0) {
            std::string treeHash = trimSpace(line.substr(5));
            if (!markReachable(store, treeHash, reachable, lfsReachable)) return false;
        } else if (line.rfind("parent ", 0) == 0) {
            std::string parentHash = trimSpace(line.substr(7));
            if (!markReachable(store, parentHash, reachable, lfsReachable)) return false;
        }
    }
    return true;
}

// 标记 tree 对象引用的所有 blob
bool markReachableTree(const Store& store, const std::vector<uint8_t>& content,
                       std::set<std::string>& reachable,
                       std::set<std::string>& lfsReachable) {
    try {
        std::vector<TreeEntry> entries = parseTree(content);
        for (const auto& e : entries) {
            if (!markReachable(store, e.hash, reachable, lfsReachable)) return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool markReachable(const Store& store, const std::string& hash,
                   std::set<std::string>& reachable,
                   std::set<std::string>& lfsReachable) {
    if (reachable.count(hash)) return true;
    reachable.insert(hash);
    auto obj = store.readObject(hash);
    if (!obj) return false;
    const std::string& objType = obj->first;
    const std::vector<uint8_t>& content = obj->second;
    if (objType == "commit") {
        return markReachableCommit(store, content, reachable, lfsReachable);
    }
    if (objType == "tree") {
        return markReachableTree(store, content, reachable, lfsReachable);
    }
    if (objType == "blob") {
        // 指针 blob 引用的 LFS 对象视为可达
        LfsPointer p;
        if (parseLfsPointer(content, p)) lfsReachable.insert(p.oid);
        return true;
    }
    return false;  // 未知类型
}

// 删除所有 loose 对象和 pack 文件（无可达对象时调用）
void removeAllObjects(Document& doc) {
    Store store(doc);
    for (const auto& hash : store.listLooseObjects()) {
        doc.remove(objectPath(hash));
    }
    for (const auto& pack : listPackFiles(doc)) {
        doc.remove(std::string(kPackDir) + "/" + pack + ".pack");
        doc.remove(std::string(kPackDir) + "/" + pack + ".idx");
    }
    for (const auto& oid : listLfsObjects(doc)) {
        removeLfsObject(doc, oid);
    }
}

} // namespace

// ============ garbageCollect ============

bool garbageCollect(Document& doc, GCStats& stats) {
    stats = GCStats{};
    Store store(doc);

    // 1. 标记从 HEAD 与所有分支引用可达的所有对象
    std::set<std::string> reachable;
    std::set<std::string> lfsReachable;
    std::string head = headCommitHash(doc);
    if (!head.empty()) {
        if (!markReachable(store, head, reachable, lfsReachable)) {
            return false;
        }
    }
    for (const auto& branch : listBranches(doc)) {
        std::string bh = getBranchHash(doc, branch);
        if (bh.empty()) continue;
        if (!markReachable(store, bh, reachable, lfsReachable)) {
            return false;
        }
    }
    stats.reachableObjects = static_cast<int>(reachable.size());

    // 2. 无可达对象：删除所有
    if (reachable.empty()) {
        removeAllObjects(doc);
        return true;
    }

    // 3. 打包可达对象
    std::vector<PackedObject> objects;
    objects.reserve(reachable.size());
    for (const auto& hash : reachable) {
        auto obj = store.readObject(hash);
        if (!obj) return false;
        PackedObject po;
        po.hash = hash;
        po.type = obj->first;
        po.data = obj->second;
        objects.push_back(std::move(po));
    }

    std::string packName = "pack-" + std::to_string(static_cast<long long>(time(nullptr)));
    if (!writePack(doc, objects, packName)) {
        return false;
    }
    stats.packedObjects = static_cast<int>(objects.size());

    // 4. 删除 loose 对象
    for (const auto& hash : store.listLooseObjects()) {
        doc.remove(objectPath(hash));
        stats.removedLoose++;
    }

    // 5. 删除旧 pack（保留新 pack）
    for (const auto& pack : listPackFiles(doc)) {
        if (pack != packName) {
            doc.remove(std::string(kPackDir) + "/" + pack + ".pack");
            doc.remove(std::string(kPackDir) + "/" + pack + ".idx");
            stats.removedPacks++;
        }
    }

    // 6. 清理不可达的 LFS 对象（LFS 对象不进 pack，仅做引用清理）
    for (const auto& oid : listLfsObjects(doc)) {
        if (!lfsReachable.count(oid)) {
            removeLfsObject(doc, oid);
            stats.removedLfs++;
        }
    }

    return true;
}

} // namespace co
