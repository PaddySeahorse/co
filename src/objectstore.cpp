#include "objectstore.hpp"
#include "packfile.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace co {

namespace {

// 去除首尾空白（对齐 Go strings.TrimSpace，仅处理 ASCII 空白）
std::string trimSpace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() &&
           (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' ||
            s[start] == '\r' || s[start] == '\v' || s[start] == '\f')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start &&
           (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' ||
            s[end - 1] == '\r' || s[end - 1] == '\v' || s[end - 1] == '\f')) {
        --end;
    }
    return s.substr(start, end - start);
}

} // namespace

// ============ 构造 ============

Store::Store(Document& doc) : doc_(doc) {}

// ============ objectPath ============

std::string objectPath(const std::string& hash) {
    if (hash.size() < 2) {
        return std::string(kObjectsDir) + "/" + hash;
    }
    return std::string(kObjectsDir) + "/" + hash.substr(0, 2) + "/" + hash.substr(2);
}

// ============ HEAD 读写 ============

std::string Store::head() const {
    std::vector<uint8_t> data;
    if (!doc_.get(kHeadFile, data)) return "";
    std::string s(data.begin(), data.end());
    return trimSpace(s);
}

void Store::setHead(const std::string& hash) {
    if (hash.empty()) {
        doc_.set(kHeadFile, std::vector<uint8_t>{});
        return;
    }
    std::string s = hash + "\n";
    std::vector<uint8_t> data(s.begin(), s.end());
    doc_.set(kHeadFile, data);
}

// ============ writeObject ============

std::string Store::writeObject(const std::string& objType, const std::vector<uint8_t>& content) {
    // 拼接 header: "type size\0"
    std::string header = objType + " " + std::to_string(content.size()) + '\0';
    std::vector<uint8_t> payload;
    payload.reserve(header.size() + content.size());
    payload.insert(payload.end(), header.begin(), header.end());
    payload.insert(payload.end(), content.begin(), content.end());

    // SHA1 取哈希
    std::array<uint8_t,20> hashBytes = sha1(payload);
    std::string hash = hexEncode(hashBytes);

    // 去重
    if (hasObject(hash)) return hash;

    // zlib 压缩
    std::vector<uint8_t> compressed = compressZlib(payload);
    doc_.set(objectPath(hash), compressed);
    return hash;
}

// ============ readObject ============

std::optional<std::pair<std::string, std::vector<uint8_t>>>
Store::readObject(const std::string& hash) const {
    // 先查 loose
    std::vector<uint8_t> raw;
    if (doc_.get(objectPath(hash), raw)) {
        try {
            std::vector<uint8_t> decompressed = decompressZlib(raw);

            // 找 null 分隔符
            auto it = std::find(decompressed.begin(), decompressed.end(), uint8_t(0));
            if (it == decompressed.end()) return std::nullopt;
            size_t nullIndex = static_cast<size_t>(it - decompressed.begin());

            // 解析 header: "type size"，取第一个空格之前的部分作为 type
            std::string header(decompressed.begin(),
                               decompressed.begin() + nullIndex);
            size_t spacePos = header.find(' ');
            std::string type;
            if (spacePos != std::string::npos) {
                type = header.substr(0, spacePos);
            } else {
                type = header;
            }

            std::vector<uint8_t> content(decompressed.begin() + nullIndex + 1,
                                         decompressed.end());
            return std::make_pair(std::move(type), std::move(content));
        } catch (...) {
            // 解压失败，继续尝试 pack
        }
    }

    // 查 packfile
    for (const auto& pack : listPackFiles(doc_)) {
        std::string type;
        std::vector<uint8_t> content;
        if (readPackedObject(doc_, pack, hash, type, content)) {
            return std::make_pair(std::move(type), std::move(content));
        }
    }

    return std::nullopt;
}

// ============ hasObject ============

bool Store::hasObject(const std::string& hash) const {
    std::vector<uint8_t> tmp;
    if (doc_.get(objectPath(hash), tmp)) return true;

    for (const auto& pack : listPackFiles(doc_)) {
        std::string type;
        std::vector<uint8_t> data;
        if (readPackedObject(doc_, pack, hash, type, data)) return true;
    }
    return false;
}

// ============ listLooseObjects ============

std::vector<std::string> Store::listLooseObjects() const {
    std::vector<std::string> objects;
    std::string prefix = std::string(kObjectsDir) + "/";
    std::string packPrefix = std::string(kObjectsDir) + "/pack/";

    for (const auto& name : doc_.list()) {
        // 必须以 prefix 开头
        if (name.size() < prefix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        // 排除 pack 目录下的条目
        if (name.size() >= packPrefix.size() &&
            name.compare(0, packPrefix.size(), packPrefix) == 0) continue;

        // 去掉前缀后按 '/' 分割
        std::string rest = name.substr(prefix.size());
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= rest.size()) {
            size_t slashPos = rest.find('/', start);
            if (slashPos == std::string::npos) {
                parts.push_back(rest.substr(start));
                break;
            }
            parts.push_back(rest.substr(start, slashPos - start));
            start = slashPos + 1;
        }

        // len(parts)==2 且 len(parts[0])==2 时拼成 hash
        if (parts.size() == 2 && parts[0].size() == 2) {
            objects.push_back(parts[0] + parts[1]);
        }
    }
    return objects;
}

} // namespace co
