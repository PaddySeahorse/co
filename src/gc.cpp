#include "gc.hpp"
#include "objectstore.hpp"
#include "packfile.hpp"
#include "util.hpp"

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
                   std::set<std::string>& reachable);

// 标记 commit 对象引用的 tree 和 parent
bool markReachableCommit(const Store& store, const std::vector<uint8_t>& content,
                         std::set<std::string>& reachable) {
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
            if (!markReachable(store, treeHash, reachable)) return false;
        } else if (line.rfind("parent ", 0) == 0) {
            std::string parentHash = trimSpace(line.substr(7));
            if (!markReachable(store, parentHash, reachable)) return false;
        }
    }
    return true;
}

// 标记 tree 对象引用的所有 blob
// 按 null 字节 + 20 字节二进制哈希遍历
bool markReachableTree(const Store& store, const std::vector<uint8_t>& content,
                       std::set<std::string>& reachable) {
    size_t pos = 0;
    while (pos < content.size()) {
        // 找 null 字节
        size_t nullIdx = pos;
        while (nullIdx < content.size() && content[nullIdx] != 0) ++nullIdx;
        if (nullIdx >= content.size()) break;
        if (nullIdx + 21 > content.size()) break;
        // 哈希在 nullIdx+1 .. nullIdx+21
        std::string hash = hexEncode(content.data() + nullIdx + 1, 20);
        if (!markReachable(store, hash, reachable)) return false;
        pos = nullIdx + 21;
    }
    return true;
}

bool markReachable(const Store& store, const std::string& hash,
                   std::set<std::string>& reachable) {
    if (reachable.count(hash)) return true;
    reachable.insert(hash);
    auto obj = store.readObject(hash);
    if (!obj) return false;
    const std::string& objType = obj->first;
    const std::vector<uint8_t>& content = obj->second;
    if (objType == "commit") {
        return markReachableCommit(store, content, reachable);
    }
    if (objType == "tree") {
        return markReachableTree(store, content, reachable);
    }
    if (objType == "blob") {
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
}

} // namespace

// ============ garbageCollect ============

bool garbageCollect(Document& doc, GCStats& stats) {
    stats = GCStats{};
    Store store(doc);

    // 1. 标记从 HEAD 可达的所有对象
    std::set<std::string> reachable;
    std::string head = store.head();
    if (!head.empty()) {
        if (!markReachable(store, head, reachable)) {
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

    return true;
}

} // namespace co
