// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "commit.hpp"
#include "objectstore.hpp"
#include "index.hpp"
#include "refs.hpp"
#include "util.hpp"
#include "lfs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <clocale>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace co {

namespace {

// 判断 ASCII 空白（对齐 Go strings.TrimSpace，仅处理 ASCII）
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

// 按空白分割（对齐 Go strings.Fields）
std::vector<std::string> splitFields(const std::string& s) {
    std::vector<std::string> result;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isAsciiSpace(s[i])) ++i;
        if (i >= s.size()) break;
        size_t start = i;
        while (i < s.size() && !isAsciiSpace(s[i])) ++i;
        result.push_back(s.substr(start, i - start));
    }
    return result;
}

// 解析 int64（对齐 Go fmt.Sscan：解析前导整数，无数字则失败）
bool parseInt64(const std::string& s, int64_t& out) {
    std::string trimmed = trimSpace(s);
    if (trimmed.empty()) return false;
    char* end = nullptr;
    long long v = strtoll(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str()) return false;  // 没有数字
    out = static_cast<int64_t>(v);
    return true;
}

// 解析 author 行 "Name <email> timestamp +0000"
struct ParsedAuthor {
    std::string name;
    std::string email;
    int64_t timestamp = 0;
};

ParsedAuthor parseAuthor(const std::string& value) {
    ParsedAuthor result;
    std::string namePart = value;

    // 找最后一个 "> " 分离 namePart 和时间部分
    size_t idx = value.rfind("> ");
    if (idx != std::string::npos) {
        namePart = value.substr(0, idx + 1);  // 包含 '>'
        std::string timePart = trimSpace(value.substr(idx + 2));
        std::vector<std::string> fs = splitFields(timePart);
        if (!fs.empty()) {
            int64_t ts = 0;
            if (parseInt64(fs[0], ts)) {
                result.timestamp = ts;
            }
        }
    }

    // 从 namePart 找 '<' 分离 name 和 email
    size_t ltIdx = namePart.rfind('<');
    if (ltIdx != std::string::npos) {
        result.name = trimSpace(namePart.substr(0, ltIdx));
        std::string email = namePart.substr(ltIdx + 1);
        // 去掉末尾 '>'
        if (!email.empty() && email.back() == '>') email.pop_back();
        result.email = email;
    } else {
        result.name = trimSpace(namePart);
    }
    return result;
}

// 从 docProps/core.xml 提取 creator 元素文本
// 简单 XML 解析：找 Local 名为 "creator" 的起始标签，读取其文本内容
std::string authorFromMetadata(const Document& doc) {
    std::vector<uint8_t> data;
    if (!doc.get("docProps/core.xml", data) || data.empty()) return "";
    std::string xml(data.begin(), data.end());

    size_t pos = 0;
    while (pos < xml.size()) {
        size_t lt = xml.find('<', pos);
        if (lt == std::string::npos) break;
        size_t gt = xml.find('>', lt);
        if (gt == std::string::npos) break;

        // 跳过注释 <!-- -->
        if (gt - lt >= 3 && xml[lt + 1] == '!' && xml[lt + 2] == '-' && xml[lt + 3] == '-') {
            size_t endComment = xml.find("-->", lt);
            if (endComment == std::string::npos) break;
            pos = endComment + 3;
            continue;
        }
        // 跳过 CDATA <![CDATA[ ]]>
        if (gt - lt >= 8 && xml.compare(lt, 9, "<![CDATA[") == 0) {
            size_t endCdata = xml.find("]]>", lt);
            if (endCdata == std::string::npos) break;
            pos = endCdata + 3;
            continue;
        }
        // 跳过处理指令 <? ?>
        if (xml[lt + 1] == '?') {
            pos = gt + 1;
            continue;
        }

        bool isEnd = (xml[lt + 1] == '/');
        size_t nameStart = isEnd ? lt + 2 : lt + 1;
        // 标签名到空格/制表/换行/'/'/'>' 为止
        size_t nameEnd = nameStart;
        while (nameEnd < gt && xml[nameEnd] != ' ' && xml[nameEnd] != '\t' &&
               xml[nameEnd] != '\n' && xml[nameEnd] != '\r' &&
               xml[nameEnd] != '/' && xml[nameEnd] != '>') {
            ++nameEnd;
        }
        std::string tagName = xml.substr(nameStart, nameEnd - nameStart);
        // 取 Local 名（去掉命名空间前缀，对齐 Go xml.Name.Local）
        std::string local = tagName;
        size_t colon = tagName.rfind(':');
        if (colon != std::string::npos) local = tagName.substr(colon + 1);

        bool selfClosing = (gt > 0 && xml[gt - 1] == '/');

        if (!isEnd && local == "creator" && !selfClosing) {
            size_t textStart = gt + 1;
            size_t closeTag = xml.find("</", textStart);
            if (closeTag == std::string::npos) return "";
            return trimSpace(xml.substr(textStart, closeTag - textStart));
        }

        pos = gt + 1;
    }
    return "";
}

// 环境变量取值，空则返回默认值
std::string envOrDefault(const char* key, const char* def) {
    const char* v = std::getenv(key);
    if (v == nullptr) return def;
    std::string s = trimSpace(v);
    if (s.empty()) return def;
    return s;
}

} // namespace

// ============ isCoEntry（公开） ============

bool isCoEntry(const std::string& name) {
    return name == ".co" || name == ".co/" ||
           (name.size() >= 4 && name.compare(0, 4, ".co/") == 0);
}

// ============ formatTimestampRFC1123Z ============

std::string formatTimestampRFC1123Z(int64_t unixTime) {
    time_t t = static_cast<time_t>(unixTime);
    struct tm tmv;
#ifdef _WIN32
    if (gmtime_s(&tmv, &t) != 0) return "";
#else
    if (gmtime_r(&t, &tmv) == nullptr) return "";
#endif

    // 切换到 C locale 确保英文月份/星期名（对齐 Go time.Format 行为）
    std::string saved = setlocale(LC_TIME, nullptr);
    setlocale(LC_TIME, "C");

    char buf[64];
    size_t n = strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", &tmv);

    setlocale(LC_TIME, saved.c_str());

    if (n == 0) return "";
    return std::string(buf, n);
}

// ============ parseCommit ============

Commit parseCommit(const std::string& hash, const std::vector<uint8_t>& content) {
    std::string s(content.begin(), content.end());

    // 按 "\n\n" 分割为 header 和 message（最多 2 段）
    size_t sep = s.find("\n\n");
    std::string header;
    std::string message;
    if (sep == std::string::npos) {
        header = s;
    } else {
        header = s.substr(0, sep);
        message = trimSpace(s.substr(sep + 2));
    }

    Commit commit;
    commit.hash = hash;
    commit.message = message;

    // 逐行解析 header
    size_t pos = 0;
    while (pos < header.size()) {
        size_t nl = header.find('\n', pos);
        std::string line;
        if (nl == std::string::npos) {
            line = header.substr(pos);
            pos = header.size();
        } else {
            line = header.substr(pos, nl - pos);
            pos = nl + 1;
        }
        if (line.empty()) continue;

        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string key = line.substr(0, sp);
        std::string val = line.substr(sp + 1);

        if (key == "tree") {
            commit.tree = val;
        } else if (key == "parent") {
            commit.parents.push_back(val);
        } else if (key == "author") {
            ParsedAuthor pa = parseAuthor(val);
            commit.authorName = pa.name;
            commit.authorEmail = pa.email;
            commit.timestamp = pa.timestamp;
        }
    }

    // 兼容字段：首父
    commit.parent = commit.parents.empty() ? std::string() : commit.parents[0];
    return commit;
}

// ============ parseTree ============

std::vector<TreeEntry> parseTree(const std::vector<uint8_t>& content) {
    std::vector<TreeEntry> entries;
    size_t pos = 0;
    while (pos < content.size()) {
        // 找空格（"100644 " 中的空格）
        size_t space = pos;
        while (space < content.size() && content[space] != ' ') ++space;
        if (space >= content.size()) {
            throw std::runtime_error("invalid tree entry: missing space");
        }
        // 找 null 字节
        size_t nullIdx = space + 1;
        while (nullIdx < content.size() && content[nullIdx] != 0) ++nullIdx;
        if (nullIdx >= content.size()) {
            throw std::runtime_error("invalid tree entry: missing null terminator");
        }
        std::string path(content.begin() + space + 1, content.begin() + nullIdx);
        size_t hashStart = nullIdx + 1;
        size_t hashEnd = hashStart + kHashLen;
        if (hashEnd > content.size()) {
            throw std::runtime_error("invalid tree entry: short hash");
        }
        std::string hash = hexEncode(content.data() + hashStart, kHashLen);
        entries.push_back(TreeEntry{path, hash});
        pos = hashEnd;
    }
    return entries;
}

// ============ 读取辅助 ============

std::optional<Commit> readCommit(const Document& doc, const std::string& hash) {
    Store store(const_cast<Document&>(doc));
    auto obj = store.readObject(hash);
    if (!obj || obj->first != "commit") return std::nullopt;
    return parseCommit(hash, obj->second);
}

std::vector<TreeEntry> readTree(const Document& doc, const std::string& hash) {
    Store store(const_cast<Document&>(doc));
    auto obj = store.readObject(hash);
    if (!obj || obj->first != "tree") return {};
    try {
        return parseTree(obj->second);
    } catch (...) {
        return {};
    }
}

// ============ tree 构建辅助 ============

// 把 entries 按字母序序列化为 tree 对象内容
static std::vector<uint8_t> serializeTree(const std::vector<TreeEntry>& entries) {
    std::vector<TreeEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const TreeEntry& a, const TreeEntry& b) { return a.path < b.path; });
    std::vector<uint8_t> treeContent;
    for (const auto& e : sorted) {
        std::string entry = "100644 " + e.path;
        treeContent.insert(treeContent.end(), entry.begin(), entry.end());
        treeContent.push_back(0);
        std::vector<uint8_t> binaryHash = hexDecode(e.hash);
        treeContent.insert(treeContent.end(), binaryHash.begin(), binaryHash.end());
    }
    return treeContent;
}

std::string buildTreeHash(Store& store, const std::vector<TreeEntry>& entries) {
    std::vector<uint8_t> treeContent = serializeTree(entries);
    return store.hashObject("tree", treeContent);
}

std::string writeTree(Store& store, const std::vector<TreeEntry>& entries) {
    std::vector<uint8_t> treeContent = serializeTree(entries);
    return store.writeObject("tree", treeContent);
}

// ============ createCommitExternal ============

std::string createCommitExternal(Document& historyDoc, const Document& contentDoc,
                                 const std::string& message, int64_t nowUnix) {
    Store store(historyDoc);

    // 加载增量索引（第九章）：缺失/损坏则全量
    CommitIndex idx = loadIndex(historyDoc);

    // 1. 遍历 contentDoc 非 .co 文件条目，写 blob（带增量去重）
    std::map<std::string, std::string> blobMap;
    CommitIndex newIndex;
    newIndex.valid = true;

    for (const auto& name : contentDoc.list()) {
        if (isCoEntry(name)) continue;
        if (!name.empty() && name.back() == '/') continue;  // 目录条目

        const ZipEntry* entry = contentDoc.getEntry(name);
        // 条目损坏（raw DEFLATE 解压失败，issue #8 的损坏形态）：
        // 拒绝提交，避免把空内容当 blob 写入造成静默数据丢失。
        if (entry && entry->corrupt) return "";
        // 指纹：优先用 ZIP 中央目录已有的 size+CRC（免读数据）
        bool haveStat = entry && (entry->crc != 0 || entry->uncompressedSize != 0);
        uint64_t fpSize = 0;
        uint32_t fpCrc = 0;
        std::vector<uint8_t> data;  // 惰性读取

        if (haveStat) {
            fpSize = entry->uncompressedSize;
            fpCrc = entry->crc;
        } else {
            contentDoc.get(name, data);
            fpSize = data.size();
            fpCrc = crc32IEEE(data);
        }

        // 增量命中：指纹相同 + 索引有 blob hash + 对象仍存在 → 复用
        std::string blobHash;
        bool reused = false;
        if (idx.valid) {
            const IndexEntry* ie = findEntry(idx, name);
            if (ie && ie->size == fpSize && ie->crc == fpCrc &&
                !ie->blobHash.empty() && store.hasObject(ie->blobHash)) {
                blobHash = ie->blobHash;
                reused = true;
            }
        }
        if (!reused) {
            if (data.empty()) contentDoc.get(name, data);
            if (shouldUseLfs(name)) {
                // LFS：内容原样写入独立对象库（不 zstd），blob 只存指针文本
                std::string oid = writeLfsObject(historyDoc, data);
                blobHash = store.writeBlob(makeLfsPointer(oid, data.size()));
            } else {
                blobHash = store.writeBlob(data);  // 内部再次对象级去重（第七章）
            }
        }
        blobMap[name] = blobHash;

        IndexEntry ne;
        ne.path = name;
        ne.size = fpSize;
        ne.crc = fpCrc;
        ne.blobHash = blobHash;
        newIndex.entries.push_back(std::move(ne));
    }

    // 2. 构建 tree
    std::vector<TreeEntry> treeEntries;
    for (const auto& kv : blobMap) treeEntries.push_back({kv.first, kv.second});
    std::string treeHash = writeTree(store, treeEntries);

    // 3. 作者信息
    std::string parent = headCommitHash(historyDoc);
    std::string name = envOrDefault("CO_AUTHOR_NAME", "");
    if (name.empty()) name = authorFromMetadata(contentDoc);
    if (name.empty()) name = "Unknown";
    std::string email = envOrDefault("CO_AUTHOR_EMAIL", "unknown@example.com");

    // 4. 构建 commit
    std::string commitContent;
    commitContent += "tree " + treeHash + "\n";
    if (!parent.empty()) {
        commitContent += "parent " + parent + "\n";
    }
    commitContent += "author " + name + " <" + email + "> " +
                     std::to_string(nowUnix) + " +0000\n";
    commitContent += "committer " + name + " <" + email + "> " +
                     std::to_string(nowUnix) + " +0000\n\n";
    commitContent += message;
    commitContent += "\n";

    std::vector<uint8_t> commitData(commitContent.begin(), commitContent.end());
    std::string commitHash = store.writeObject("commit", commitData);
    advanceHead(historyDoc, commitHash);

    // 5. 写回增量索引
    saveIndex(historyDoc, newIndex);
    return commitHash;
}

std::string createCommit(Document& doc, const std::string& message, int64_t nowUnix) {
    return createCommitExternal(doc, doc, message, nowUnix);
}

// ============ createMergeCommit ============

std::string createMergeCommit(Document& historyDoc, const std::string& treeHash,
                              const std::vector<std::string>& parents,
                              const std::string& message, int64_t nowUnix) {
    Store store(historyDoc);
    std::string name = envOrDefault("CO_AUTHOR_NAME", "Merge");
    if (name.empty()) name = "Merge";
    std::string email = envOrDefault("CO_AUTHOR_EMAIL", "unknown@example.com");

    std::string commitContent;
    commitContent += "tree " + treeHash + "\n";
    for (const auto& p : parents) {
        if (!p.empty()) commitContent += "parent " + p + "\n";
    }
    commitContent += "author " + name + " <" + email + "> " +
                     std::to_string(nowUnix) + " +0000\n";
    commitContent += "committer " + name + " <" + email + "> " +
                     std::to_string(nowUnix) + " +0000\n\n";
    commitContent += message;
    commitContent += "\n";

    std::vector<uint8_t> commitData(commitContent.begin(), commitContent.end());
    std::string commitHash = store.writeObject("commit", commitData);
    advanceHead(historyDoc, commitHash);
    // merge 后索引失效，删除以走全量
    removeIndex(historyDoc);
    return commitHash;
}

// ============ logCommits ============

std::vector<Commit> logCommits(const Document& doc) {
    // Store 构造需要非 const 引用，但此处仅调用 const 方法（head/readObject），
    // 不会修改 doc，故 const_cast 安全
    Store store(const_cast<Document&>(doc));

    std::vector<Commit> commits;
    std::string current = headCommitHash(doc);
    while (!current.empty()) {
        auto obj = store.readObject(current);
        if (!obj) break;
        if (obj->first != "commit") break;
        Commit parsed = parseCommit(current, obj->second);
        commits.push_back(std::move(parsed));
        current = commits.back().parent;
    }
    return commits;
}

// ============ checkoutCommitExternal ============

bool checkoutCommitExternal(Document& historyDoc, Document& contentDoc,
                            const std::string& commitHash) {
    try {
        Store store(historyDoc);

        auto commit = readCommit(historyDoc, commitHash);
        if (!commit || commit->tree.empty()) return false;

        std::vector<TreeEntry> entries = readTree(historyDoc, commit->tree);
        if (entries.empty()) {
            // 区分「合法空树」与「tree 对象缺失/损坏」：后者必须失败以免清空内容。
            auto treeObj = store.readObject(commit->tree);
            if (!treeObj || treeObj->first != "tree") return false;
        }

        // 删除 contentDoc 非 .co 条目
        auto names = contentDoc.list();  // 拷贝，避免迭代时修改
        for (const auto& name : names) {
            if (isCoEntry(name)) continue;
            contentDoc.remove(name);
        }

        // 按 tree 恢复 blob 到 contentDoc，同时重建增量索引
        CommitIndex newIndex;
        newIndex.valid = true;
        for (const auto& entry : entries) {
            std::vector<uint8_t> content;
            // 指针 blob 自动从 LFS 库还原真实内容
            if (!resolveBlobContent(store, entry.hash, content)) return false;
            contentDoc.set(entry.path, content);
            // 写入 CRC/size 供下次 commit 增量命中（免读数据）
            ZipEntry* ze = contentDoc.getEntryMut(entry.path);
            if (ze) {
                ze->crc = crc32IEEE(content);
                ze->uncompressedSize = content.size();
            }
            IndexEntry ne;
            ne.path = entry.path;
            ne.size = content.size();
            ne.crc = crc32IEEE(content);
            ne.blobHash = entry.hash;
            newIndex.entries.push_back(std::move(ne));
        }

        saveIndex(historyDoc, newIndex);
        return true;
    } catch (...) {
        return false;
    }
}

bool checkoutCommit(Document& doc, const std::string& commitHash) {
    return checkoutCommitExternal(doc, doc, commitHash);
}

} // namespace co
