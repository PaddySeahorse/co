// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lfs.hpp"
#include "util.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>

namespace co {

namespace {

// 白名单扩展名（小写）：已压缩的图片/视频/音频/pdf 等。
// 命中即走 LFS（原样存储、不 zstd、不参与 delta），避免压缩浪费。
const std::set<std::string>& lfsWhitelist() {
    static const std::set<std::string> whitelist = {
        // 图片
        "png", "jpg", "jpeg", "gif", "webp", "bmp", "tif", "tiff", "ico",
        "emf", "wmf",
        // 视频
        "mp4", "mov", "avi", "mkv", "m4v", "wmv", "flv",
        // 音频
        "mp3", "wav", "aac", "flac", "ogg", "wma",
        // 已压缩/其他大文件
        "pdf", "zip", "7z", "rar",
    };
    return whitelist;
}

} // namespace

// ============ 判定规则 ============

bool isLfsExtension(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lfsWhitelist().count(ext) != 0;
}

bool shouldUseLfs(const std::string& path) {
    return isLfsExtension(path);
}

// ============ 指针编解码 ============

bool isLfsPointer(const std::vector<uint8_t>& blobData) {
    const std::string prefix = "version " + std::string(kLfsPointerVersion) + "\n";
    if (blobData.size() < prefix.size()) return false;
    return std::memcmp(blobData.data(), prefix.data(), prefix.size()) == 0;
}

bool parseLfsPointer(const std::vector<uint8_t>& blobData, LfsPointer& out) {
    if (!isLfsPointer(blobData)) return false;
    std::string s(blobData.begin(), blobData.end());

    std::string oid;
    uint64_t size = 0;
    size_t pos = 0;
    int lineIdx = 0;  // 0=version, 1=oid, 2=size
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line = (nl == std::string::npos) ? s.substr(pos)
                                                     : s.substr(pos, nl - pos);
        if (lineIdx == 1) {
            const std::string oidPrefix = "oid sha256:";
            if (line.rfind(oidPrefix, 0) != 0 ||
                line.size() != oidPrefix.size() + 64) {
                return false;
            }
            std::string hex = line.substr(oidPrefix.size());
            for (char c : hex) {
                bool digit = (c >= '0' && c <= '9');
                bool lower = (c >= 'a' && c <= 'f');
                if (!digit && !lower) return false;
            }
            oid = hex;
        } else if (lineIdx == 2) {
            const std::string sizePrefix = "size ";
            if (line.rfind(sizePrefix, 0) != 0) return false;
            std::string sz = line.substr(sizePrefix.size());
            if (sz.empty()) return false;
            char* end = nullptr;
            errno = 0;
            unsigned long long v = std::strtoull(sz.c_str(), &end, 10);
            if (end == sz.c_str() || *end != '\0' || errno == ERANGE) return false;
            size = static_cast<uint64_t>(v);
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
        ++lineIdx;
    }
    if (oid.empty()) return false;
    out.oid = oid;
    out.size = size;
    return true;
}

std::vector<uint8_t> makeLfsPointer(const std::string& oid, uint64_t size) {
    std::string s = "version " + std::string(kLfsPointerVersion) + "\n"
                  + "oid sha256:" + oid + "\n"
                  + "size " + std::to_string(size) + "\n";
    return std::vector<uint8_t>(s.begin(), s.end());
}

// ============ LFS 对象读写 ============

std::string lfsObjectPath(const std::string& oid) {
    if (oid.size() < 2) {
        return std::string(kLfsObjectsDir) + "/" + oid;
    }
    return std::string(kLfsObjectsDir) + "/" + oid.substr(0, 2) + "/" + oid.substr(2);
}

std::string computeLfsOid(const std::vector<uint8_t>& content) {
    return hexEncode(sha256(content));
}

std::string writeLfsObject(Document& doc, const std::vector<uint8_t>& content) {
    std::string oid = computeLfsOid(content);
    if (!hasLfsObject(doc, oid)) {
        doc.set(lfsObjectPath(oid), content);
    }
    return oid;
}

bool readLfsObject(const Document& doc, const std::string& oid,
                   std::vector<uint8_t>& out) {
    return doc.get(lfsObjectPath(oid), out);
}

bool hasLfsObject(const Document& doc, const std::string& oid) {
    std::vector<uint8_t> tmp;
    return doc.get(lfsObjectPath(oid), tmp);
}

std::vector<std::string> listLfsObjects(const Document& doc) {
    std::vector<std::string> oids;
    std::string prefix = std::string(kLfsObjectsDir) + "/";

    for (const auto& name : doc.list()) {
        if (name.size() <= prefix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (!name.empty() && name.back() == '/') continue;  // 目录条目

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
        if (parts.size() == 2 && parts[0].size() == 2 && !parts[1].empty()) {
            oids.push_back(parts[0] + parts[1]);
        }
    }
    return oids;
}

void removeLfsObject(Document& doc, const std::string& oid) {
    doc.remove(lfsObjectPath(oid));
}

// ============ 统一 blob 内容解析 ============

bool resolveBlobContent(const Store& store, const std::string& hash,
                        std::vector<uint8_t>& out) {
    auto obj = store.readObject(hash);
    if (!obj || obj->first != "blob") return false;
    LfsPointer p;
    if (parseLfsPointer(obj->second, p)) {
        return readLfsObject(store.doc(), p.oid, out);
    }
    out = std::move(obj->second);
    return true;
}

} // namespace co
