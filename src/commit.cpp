// SPDX-License-Identifier: GPL-3.0-or-later

#include "commit.hpp"
#include "objectstore.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <clocale>
#include <cstdlib>
#include <ctime>
#include <map>
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

// 判断是否为 .co 条目
bool isCoEntry(const std::string& name) {
    return name == ".co" || name == ".co/" ||
           (name.size() >= 4 && name.compare(0, 4, ".co/") == 0);
}

} // namespace

// ============ formatTimestampRFC1123Z ============

std::string formatTimestampRFC1123Z(int64_t unixTime) {
    time_t t = static_cast<time_t>(unixTime);
    struct tm tmv;
    if (gmtime_r(&t, &tmv) == nullptr) return "";

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
            commit.parent = val;
        } else if (key == "author") {
            ParsedAuthor pa = parseAuthor(val);
            commit.authorName = pa.name;
            commit.authorEmail = pa.email;
            commit.timestamp = pa.timestamp;
        }
    }

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
        size_t hashEnd = hashStart + 20;
        if (hashEnd > content.size()) {
            throw std::runtime_error("invalid tree entry: short hash");
        }
        std::string hash = hexEncode(content.data() + hashStart, 20);
        entries.push_back(TreeEntry{path, hash});
        pos = hashEnd;
    }
    return entries;
}

// ============ createCommit ============

std::string createCommit(Document& doc, const std::string& message, int64_t nowUnix) {
    Store store(doc);

    // 1. 遍历非 .co 条目写 blob
    std::map<std::string, std::string> blobMap;
    for (const auto& name : doc.list()) {
        if (name.rfind(".co/", 0) == 0) continue;          // .co/ 前缀
        if (!name.empty() && name.back() == '/') continue;  // 目录条目

        std::vector<uint8_t> data;
        doc.get(name, data);  // 忽略失败（对齐 Go：失败时 data 为空）
        std::string blobHash = store.writeObject("blob", data);
        blobMap[name] = blobHash;
    }

    // 2. 按字母排序路径构建 tree
    std::vector<std::string> paths;
    paths.reserve(blobMap.size());
    for (const auto& kv : blobMap) paths.push_back(kv.first);
    std::sort(paths.begin(), paths.end());

    std::vector<uint8_t> treeContent;
    for (const auto& path : paths) {
        std::string entry = "100644 " + path;
        treeContent.insert(treeContent.end(), entry.begin(), entry.end());
        treeContent.push_back(0);  // null 分隔
        std::vector<uint8_t> binaryHash = hexDecode(blobMap[path]);
        treeContent.insert(treeContent.end(), binaryHash.begin(), binaryHash.end());
    }

    std::string treeHash = store.writeObject("tree", treeContent);

    // 3. 构建作者信息
    std::string parent = store.head();
    std::string name = envOrDefault("CO_AUTHOR_NAME", "");
    if (name.empty()) name = authorFromMetadata(doc);
    if (name.empty()) name = "Unknown";
    std::string email = envOrDefault("CO_AUTHOR_EMAIL", "unknown@example.com");

    // 4. 构建 commit 内容
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
    store.setHead(commitHash);
    return commitHash;
}

// ============ logCommits ============

std::vector<Commit> logCommits(const Document& doc) {
    // Store 构造需要非 const 引用，但此处仅调用 const 方法（head/readObject），
    // 不会修改 doc，故 const_cast 安全
    Store store(const_cast<Document&>(doc));

    std::vector<Commit> commits;
    std::string current = store.head();
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

// ============ checkoutCommit ============

bool checkoutCommit(Document& doc, const std::string& commitHash) {
    try {
        Store store(doc);

        // 读 commit 对象
        auto obj = store.readObject(commitHash);
        if (!obj) return false;
        if (obj->first != "commit") return false;

        Commit parsed = parseCommit(commitHash, obj->second);
        if (parsed.tree.empty()) return false;

        // 读 tree 对象
        auto treeObj = store.readObject(parsed.tree);
        if (!treeObj) return false;
        if (treeObj->first != "tree") return false;

        std::vector<TreeEntry> entries = parseTree(treeObj->second);

        // 删除非 .co 条目
        auto names = doc.list();  // 拷贝，避免迭代时修改
        for (const auto& name : names) {
            if (isCoEntry(name)) continue;
            doc.remove(name);
        }

        // 按 tree 恢复 blob
        for (const auto& entry : entries) {
            auto blob = store.readObject(entry.hash);
            if (!blob) return false;
            if (blob->first != "blob") return false;
            doc.set(entry.path, blob->second);
        }

        store.setHead(commitHash);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace co
