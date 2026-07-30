// SPDX-License-Identifier: GPL-3.0-or-later

#include "status.hpp"
#include "objectstore.hpp"
#include "bundle.hpp"
#include "util.hpp"

#include <algorithm>
#include <map>
#include <string>

namespace co {

// ============ resolveRef ============

std::string resolveRef(const Document& doc, const std::string& ref) {
    Store store(const_cast<Document&>(doc));

    if (ref == "HEAD" || ref.empty()) {
        return store.head();
    }

    // HEAD~N
    if (ref.rfind("HEAD", 0) == 0) {
        std::string rest = ref.substr(4);
        if (!rest.empty() && rest[0] == '~') {
            int n = 0;
            bool ok = true;
            for (size_t i = 1; i < rest.size(); ++i) {
                if (rest[i] < '0' || rest[i] > '9') { ok = false; break; }
                n = n * 10 + (rest[i] - '0');
            }
            if (ok && rest.size() > 1) {
                std::string cur = store.head();
                for (int i = 0; i < n && !cur.empty(); ++i) {
                    auto c = readCommit(doc, cur);
                    if (!c) return "";
                    cur = c->parent;
                }
                return cur;
            }
        }
    }

    // 完整 hash
    if (store.hasObject(ref)) return ref;

    // 前缀匹配（在 HEAD 链中找）
    std::string cur = store.head();
    while (!cur.empty()) {
        if (cur.rfind(ref, 0) == 0) return cur;
        auto c = readCommit(doc, cur);
        if (!c) break;
        cur = c->parent;
    }
    return "";
}

// ============ computeStatus ============

StatusInfo computeStatus(const Document& historyDoc, const Document& contentDoc,
                         const std::string& filePath) {
    StatusInfo info;
    info.fileSize = fileSizeOf(filePath);
    info.hashAlgo = currentHashAlgoName();

    Store store(const_cast<Document&>(historyDoc));
    std::string head = store.head();
    StoreStats stats = computeStoreStats(historyDoc);
    info.commitCount = stats.commits;
    info.objectCount = stats.objects;

    if (head.empty()) {
        info.hasHistory = false;
        return info;
    }
    info.hasHistory = true;
    info.headHash = head;
    info.headShort = head.substr(0, std::min<size_t>(head.size(), 7));

    auto c = readCommit(historyDoc, head);
    if (c) {
        info.headMessage = c->message;
        info.headTimestamp = c->timestamp;
    }

    // 比较 contentDoc 与 HEAD tree
    std::map<std::string, std::string> treeMap;  // path -> blob hash
    if (c && !c->tree.empty()) {
        auto entries = readTree(historyDoc, c->tree);
        for (const auto& e : entries) treeMap[e.path] = e.hash;
    }

    std::map<std::string, std::string> contentMap;  // path -> blob hash
    for (const auto& name : contentDoc.list()) {
        if (isCoEntry(name)) continue;
        if (!name.empty() && name.back() == '/') continue;
        std::vector<uint8_t> data;
        contentDoc.get(name, data);
        contentMap[name] = store.hashObject("blob", data);
    }

    // 新增/修改
    for (const auto& kv : contentMap) {
        auto it = treeMap.find(kv.first);
        if (it == treeMap.end() || it->second != kv.second) {
            info.changedFiles++;
        }
    }
    // 删除
    for (const auto& kv : treeMap) {
        if (contentMap.find(kv.first) == contentMap.end()) {
            info.changedFiles++;
        }
    }

    return info;
}

// ============ diffCommits ============

std::vector<DiffEntry> diffCommits(const Document& doc,
                                   const std::string& aCommit, const std::string& bCommit) {
    std::vector<DiffEntry> result;
    std::string a = resolveRef(doc, aCommit);
    std::string b = resolveRef(doc, bCommit);
    if (a.empty() || b.empty()) return result;

    auto ca = readCommit(doc, a);
    auto cb = readCommit(doc, b);
    if (!ca || !cb) return result;

    auto ea = readTree(doc, ca->tree);
    auto eb = readTree(doc, cb->tree);

    std::map<std::string, std::string> ta, tb;
    for (const auto& e : ea) ta[e.path] = e.hash;
    for (const auto& e : eb) tb[e.path] = e.hash;

    // B 相对 A：A 有 B 无 → D；B 有 A 无 → A；都有但 hash 不同 → M
    for (const auto& kv : ta) {
        auto it = tb.find(kv.first);
        if (it == tb.end()) result.push_back({kv.first, 'D'});
        else if (it->second != kv.second) result.push_back({kv.first, 'M'});
    }
    for (const auto& kv : tb) {
        if (ta.find(kv.first) == ta.end()) result.push_back({kv.first, 'A'});
    }
    std::sort(result.begin(), result.end(),
              [](const DiffEntry& x, const DiffEntry& y) { return x.path < y.path; });
    return result;
}

} // namespace co
