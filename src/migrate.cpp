// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "migrate.hpp"
#include "bundle.hpp"
#include "objectstore.hpp"
#include "packfile.hpp"
#include "commit.hpp"
#include "index.hpp"
#include "refs.hpp"
#include "util.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace co {

namespace {

// 目标算法哈希长度
size_t targetHashLen() {
    return (kHashLen == 20) ? 32 : 20;
}
size_t sourceHashLen() {
    return kHashLen;
}
size_t sourceHexLen() { return sourceHashLen() * 2; }

// 用目标算法计算对象 payload 的 hex 哈希
std::string targetObjectHex(const std::string& objType, const std::vector<uint8_t>& content) {
    std::string header = objType + " " + std::to_string(content.size()) + '\0';
    std::vector<uint8_t> payload;
    payload.reserve(header.size() + content.size());
    payload.insert(payload.end(), header.begin(), header.end());
    payload.insert(payload.end(), content.begin(), content.end());
    std::vector<uint8_t> h = hashDigestByLen(payload.data(), payload.size(), targetHashLen());
    return hexEncode(h);
}

// 按目标算法写一个 loose 对象，返回新 hex 哈希
std::string writeTargetLoose(Document& doc, const std::string& objType,
                             const std::vector<uint8_t>& content) {
    std::string hex = targetObjectHex(objType, content);
    std::string header = objType + " " + std::to_string(content.size()) + '\0';
    std::vector<uint8_t> payload;
    payload.reserve(header.size() + content.size());
    payload.insert(payload.end(), header.begin(), header.end());
    payload.insert(payload.end(), content.begin(), content.end());
    std::vector<uint8_t> compressed = compressZstd(payload);
    doc.set(objectPath(hex), compressed);
    return hex;
}

struct Migrator {
    Document& doc;
    Store store;
    std::map<std::string, std::string> blobMap;
    std::map<std::string, std::string> treeMap;
    std::map<std::string, std::string> commitMap;
    int64_t blobs = 0, trees = 0, commits = 0;

    explicit Migrator(Document& d) : doc(d), store(d) {}

    std::string migrateBlob(const std::string& oldHex) {
        auto it = blobMap.find(oldHex);
        if (it != blobMap.end()) return it->second;
        auto obj = store.readObject(oldHex);
        if (!obj || obj->first != "blob") return "";
        std::string nh = writeTargetLoose(doc, "blob", obj->second);
        blobMap[oldHex] = nh;
        ++blobs;
        return nh;
    }

    std::string migrateTree(const std::string& oldHex) {
        auto it = treeMap.find(oldHex);
        if (it != treeMap.end()) return it->second;
        auto obj = store.readObject(oldHex);
        if (!obj || obj->first != "tree") return "";
        std::vector<TreeEntry> entries;
        try { entries = parseTree(obj->second); } catch (...) { return ""; }

        std::vector<uint8_t> newContent;
        // 按原顺序重建（parseTree 已按出现顺序；为稳妥再排序）
        std::sort(entries.begin(), entries.end(),
                  [](const TreeEntry& a, const TreeEntry& b) { return a.path < b.path; });
        for (const auto& e : entries) {
            std::string nb = migrateBlob(e.hash);
            if (nb.empty()) return "";
            std::string entry = "100644 " + e.path;
            newContent.insert(newContent.end(), entry.begin(), entry.end());
            newContent.push_back(0);
            std::vector<uint8_t> bin = hexDecode(nb);  // 目标长度字节
            newContent.insert(newContent.end(), bin.begin(), bin.end());
        }
        std::string nh = writeTargetLoose(doc, "tree", newContent);
        treeMap[oldHex] = nh;
        ++trees;
        return nh;
    }

    std::string migrateCommit(const std::string& oldHex) {
        auto it = commitMap.find(oldHex);
        if (it != commitMap.end()) return it->second;
        auto obj = store.readObject(oldHex);
        if (!obj || obj->first != "commit") return "";

        // 解析 commit：tree + parents（按源算法）
        Commit c = parseCommit(oldHex, obj->second);

        std::string newTree = migrateTree(c.tree);
        if (newTree.empty() && !c.tree.empty()) return "";

        std::vector<std::string> newParents;
        for (const auto& p : c.parents) {
            if (p.empty()) continue;
            std::string np = migrateCommit(p);
            if (np.empty()) return "";
            newParents.push_back(np);
        }

        // 重建 commit 文本：保留原 header 中非 tree/parent 行 + 原 message
        std::string s(obj->second.begin(), obj->second.end());
        size_t sep = s.find("\n\n");
        std::string header = (sep == std::string::npos) ? s : s.substr(0, sep);
        std::string message = (sep == std::string::npos) ? "" : s.substr(sep + 2);

        std::string newHeader;
        size_t pos = 0;
        size_t parentIdx = 0;
        while (pos < header.size()) {
            size_t nl = header.find('\n', pos);
            std::string line = (nl == std::string::npos) ? header.substr(pos) : header.substr(pos, nl - pos);
            if (line.rfind("tree ", 0) == 0) {
                newHeader += "tree " + newTree + "\n";
            } else if (line.rfind("parent ", 0) == 0) {
                if (parentIdx < newParents.size()) {
                    newHeader += "parent " + newParents[parentIdx++] + "\n";
                }
                // 多余的 parent 行（不应出现）丢弃
            } else {
                newHeader += line + "\n";
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        std::string newContent = newHeader + "\n" + message;
        std::vector<uint8_t> nc(newContent.begin(), newContent.end());
        std::string nh = writeTargetLoose(doc, "commit", nc);
        commitMap[oldHex] = nh;
        ++commits;
        return nh;
    }
};

} // namespace

MigrateResult migrateStore(const std::string& path) {
    MigrateResult r;
    r.fromAlgo = currentHashAlgoName();
    r.toAlgo = (kHashLen == 20) ? "sha256" : "sha1";

    auto doc = Document::load(path);
    if (!doc) { r.error = "failed to load: " + path; return r; }

    Store store(*doc);
    HeadRef hr = resolveHead(*doc);
    std::string head = hr.hash;
    if (head.empty()) {
        // 无历史，无需迁移（可能仍有悬空分支引用，一并清理）
        r.ok = true;
        return r;
    }

    Migrator mig(*doc);
    std::string newHead = mig.migrateCommit(head);
    if (newHead.empty()) { r.error = "migration failed (cannot rebuild commit graph)"; return r; }
    r.newHead = newHead;
    r.objectsRewritten = mig.blobs;
    r.treesRewritten = mig.trees;
    r.commitsRewritten = mig.commits;

    // 迁移所有分支引用（含符号 HEAD 指向的分支）
    for (const auto& branch : listBranches(*doc)) {
        std::string oldBh = getBranchHash(*doc, branch);
        if (oldBh.empty()) continue;
        std::string newBh = mig.migrateCommit(oldBh);
        if (newBh.empty()) { r.error = "migration failed (cannot rewrite branch " + branch + ")"; return r; }
        if (!setBranch(*doc, branch, newBh)) { r.error = "migration failed (cannot rewrite branch " + branch + ")"; return r; }
    }

    // 恢复 HEAD：符号形式保持符号（分支已在循环中重写，仅需重新指向）；分离形式写 hash
    if (hr.symbolic) {
        if (!attachHead(*doc, hr.branch)) {
            r.error = "migration failed (cannot attach HEAD to branch " + hr.branch + ")";
            return r;
        }
    } else {
        store.setHead(newHead);
    }

    // 删除旧算法 loose 对象（hex 长度 == 源长度的）与新算法无关的 pack
    for (const auto& h : store.listLooseObjects()) {
        if (h.size() == sourceHexLen()) {
            doc->remove(objectPath(h));
        }
    }
    for (const auto& pack : store.listPacks()) {
        doc->remove(std::string(kPackDir) + "/" + pack + ".pack");
        doc->remove(std::string(kPackDir) + "/" + pack + ".idx");
    }
    // index 引用的是旧 blob hash，删除以走全量
    removeIndex(*doc);

    if (!doc->write(path)) {
        r.error = "failed to write: " + path;
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace co
