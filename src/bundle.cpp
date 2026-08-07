// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bundle.hpp"
#include "objectstore.hpp"
#include "packfile.hpp"
#include "commit.hpp"
#include "refs.hpp"
#include "util.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>

namespace co {

// ============ 小工具 ============

std::string currentHashAlgoName() {
#ifdef CO_HASH_SHA256
    return "sha256";
#else
    return "sha1";
#endif
}

std::string repoHashAlgoName(const Document& doc) {
    // 优先：bundle 文件中 manifest.json 的 hash_algo 字段
    std::vector<uint8_t> md;
    if (doc.get(kManifestPath, md)) {
        Manifest m;
        std::string s(md.begin(), md.end());
        if (parseManifest(s, m) && !m.hashAlgo.empty()) return m.hashAlgo;
    }
    // 兜底：根据 HEAD hex 长度推断（嵌入式仓库无 manifest.json）
    std::string h = headCommitHash(doc);
    if (h.empty()) return "";
    if (h.size() == 2 * 20) return "sha1";
    if (h.size() == 2 * 32) return "sha256";
    return "";
}

int64_t fileSizeOf(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return -1;
    return static_cast<int64_t>(st.st_size);
}

std::string fileSha256(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    // 增量计算 SHA256
    std::array<uint8_t, 32> out{};
    unsigned int outLen = 0;
    // 用一次性读取简化（Office 文件通常不大）
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    if (EVP_Digest(buf.data(), buf.size(), out.data(), &outLen,
                   EVP_sha256(), nullptr) != 1) {
        return "";
    }
    return hexEncode(out);
}

namespace {

// JSON 字符串转义
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// 在 json 中查找 "key" 后的值（字符串或数字）。失败返回空。
std::string findJsonString(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t k = json.find(pat);
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k + pat.size());
    if (colon == std::string::npos) return "";
    size_t i = colon + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size()) return "";
    if (json[i] != '"') return "";
    ++i;
    std::string out;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < json.size()) {
            char n = json[i + 1];
            switch (n) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: out += n; break;
            }
            i += 2;
        } else {
            out += json[i++];
        }
    }
    return out;
}

int64_t findJsonInt(const std::string& json, const std::string& key, int64_t def) {
    std::string pat = "\"" + key + "\"";
    size_t k = json.find(pat);
    if (k == std::string::npos) return def;
    size_t colon = json.find(':', k + pat.size());
    if (colon == std::string::npos) return def;
    size_t i = colon + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] == '"') return def;
    try {
        return static_cast<int64_t>(std::strtoll(json.c_str() + i, nullptr, 10));
    } catch (...) {
        return def;
    }
}

std::string isoNowUtc() {
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    if (gmtime_s(&tmv, &t) != 0) return "";
#else
    if (gmtime_r(&t, &tmv) == nullptr) return "";
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

} // namespace

// ============ manifest ============

std::string serializeManifest(const Manifest& m) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"version\": \"" << jsonEscape(m.version) << "\",\n";
    os << "  \"source_filename\": \"" << jsonEscape(m.sourceFilename) << "\",\n";
    os << "  \"source_sha256\": \"" << jsonEscape(m.sourceSha256) << "\",\n";
    os << "  \"created_at\": \"" << jsonEscape(m.createdAt) << "\",\n";
    os << "  \"commit_count\": " << m.commitCount << ",\n";
    os << "  \"bundle_size_bytes\": " << m.bundleSizeBytes << ",\n";
    os << "  \"co_version\": \"" << jsonEscape(m.coVersion) << "\",\n";
    os << "  \"hash_algo\": \"" << jsonEscape(m.hashAlgo) << "\"\n";
    os << "}\n";
    return os.str();
}

bool parseManifest(const std::string& json, Manifest& out) {
    if (json.find('{') == std::string::npos) return false;
    out.version = findJsonString(json, "version");
    if (out.version.empty()) out.version = "1.0";
    out.sourceFilename = findJsonString(json, "source_filename");
    out.sourceSha256 = findJsonString(json, "source_sha256");
    out.createdAt = findJsonString(json, "created_at");
    out.commitCount = findJsonInt(json, "commit_count", 0);
    out.bundleSizeBytes = findJsonInt(json, "bundle_size_bytes", 0);
    out.coVersion = findJsonString(json, "co_version");
    out.hashAlgo = findJsonString(json, "hash_algo");
    return true;
}

// ============ computeStoreStats ============

StoreStats computeStoreStats(const Document& doc) {
    StoreStats s;
    // commit 数：沿 HEAD 链
    auto commits = logCommits(doc);
    s.commits = static_cast<int64_t>(commits.size());
    // object 数：loose + 每个 pack 的 idx objCount
    Store store(const_cast<Document&>(doc));
    s.objects = static_cast<int64_t>(store.listLooseObjects().size());
    for (const auto& pack : store.listPacks()) {
        PackIndex idx;
        if (readPackIndex(doc, pack, idx)) {
            s.objects += static_cast<int64_t>(idx.hashes.size());
        }
    }
    return s;
}

// ============ export ============

namespace {

// 把 source 中所有 .co/ 条目按字节复制到 dst（保留原始压缩字节）
void copyCoEntries(const Document& source, Document& dst) {
    for (const auto& name : source.list()) {
        if (!isCoEntry(name)) continue;
        const ZipEntry* e = source.getEntry(name);
        if (e) dst.setEntry(name, *e);
    }
}

// 把 source 中所有非 .co/ 条目按字节复制到 dst（用于 clean 副本）
void copyContentEntries(const Document& source, Document& dst) {
    for (const auto& name : source.list()) {
        if (isCoEntry(name)) continue;
        const ZipEntry* e = source.getEntry(name);
        if (e) dst.setEntry(name, *e);
    }
}

// redact 模式：只把可达的 commit/tree 对象以 loose 形式写入 dst，丢弃 blob 与 pack
bool writeRedactedHistory(const Document& source, Document& dst) {
    Store srcStore(const_cast<Document&>(source));
    Store dstStore(dst);
    std::string head = headCommitHash(source);
    if (head.empty()) return true;  // 无历史

    // BFS 收集可达对象（起点：HEAD + 所有分支引用）
    std::vector<std::string> queue{head};
    for (const auto& branch : listBranches(source)) {
        std::string bh = getBranchHash(source, branch);
        if (!bh.empty()) queue.push_back(bh);
    }
    std::set<std::string> seen;
    while (!queue.empty()) {
        std::string h = queue.back();
        queue.pop_back();
        if (!seen.insert(h).second) continue;
        auto obj = srcStore.readObject(h);
        if (!obj) continue;
        const std::string& type = obj->first;
        if (type == "commit") {
            auto c = parseCommit(h, obj->second);
            dstStore.writeObject("commit", obj->second);  // 重写为 loose（hash 不变）
            if (!c.tree.empty()) queue.push_back(c.tree);
            for (const auto& p : c.parents) if (!p.empty()) queue.push_back(p);
        } else if (type == "tree") {
            dstStore.writeObject("tree", obj->second);
            auto entries = parseTree(obj->second);
            for (const auto& e : entries) queue.push_back(e.hash);
        }
        // blob：跳过（redact）
    }
    // 保留 HEAD 原始内容（符号或分离形式）与分支引用
    std::vector<uint8_t> headData;
    if (source.get(kHeadFile, headData)) {
        dst.set(kHeadFile, headData);
    } else {
        dstStore.setHead(head);
    }
    for (const auto& branch : listBranches(source)) {
        std::string bh = getBranchHash(source, branch);
        if (!bh.empty()) setBranch(dst, branch, bh);
    }
    return true;
}

} // namespace

bool exportBundle(const std::string& sourcePath, const ExportOptions& opts,
                  std::string& error) {
    auto src = Document::load(sourcePath);
    if (!src) { error = "failed to load source: " + sourcePath; return false; }

    std::string sha = fileSha256(sourcePath);
    if (sha.empty()) { error = "failed to hash source: " + sourcePath; return false; }

    // 源仓库实际算法：优先 manifest（bundle→bundle 二次 export 场景），否则按 HEAD 推断；
    // 空仓库退化为当前构建默认。manifest 不可再用 currentHashAlgoName()，否则 sha256
    // 仓库被 sha1 二进制 export 会写出错误标记。
    std::string srcAlgo = repoHashAlgoName(*src);
    if (srcAlgo.empty()) srcAlgo = currentHashAlgoName();

    StoreStats stats = computeStoreStats(*src);

    auto buildBundleDoc = [&](int64_t reportedSize) -> std::unique_ptr<Document> {
        auto b = std::make_unique<Document>();
        if (opts.redact) {
            writeRedactedHistory(*src, *b);
        } else {
            copyCoEntries(*src, *b);
        }
        Manifest m;
        m.version = "1.0";
        m.sourceFilename = sourcePath;
        m.sourceSha256 = sha;
        m.createdAt = isoNowUtc();
        m.commitCount = stats.commits;
        m.bundleSizeBytes = reportedSize;
        m.coVersion = CO_VERSION_STR;
        m.hashAlgo = srcAlgo;
        std::string msj = serializeManifest(m);
        std::vector<uint8_t> md(msj.begin(), msj.end());
        b->set(kManifestPath, md);
        return b;
    };

    // 收敛写入：让 manifest.bundle_size_bytes 等于最终文件大小（差几位数字则迭代）
    int64_t reported = 0;
    for (int iter = 0; iter < 6; ++iter) {
        auto b = buildBundleDoc(reported);
        if (!b->write(opts.outputPath)) {
            error = "failed to write bundle: " + opts.outputPath;
            return false;
        }
        int64_t sz = fileSizeOf(opts.outputPath);
        if (sz < 0) { error = "failed to stat bundle"; return false; }
        if (sz == reported) break;
        reported = sz;
    }

    // 干净副本
    if (opts.clean) {
        auto clean = std::make_unique<Document>();
        copyContentEntries(*src, *clean);
        if (!clean->write(opts.cleanOutputPath)) {
            error = "failed to write clean copy: " + opts.cleanOutputPath;
            return false;
        }
    }
    return true;
}

// ============ import ============

ImportOutcome importBundle(const std::string& targetPath, const std::string& bundlePath,
                           bool force, bool verifyOnly) {
    ImportOutcome r;

    auto bundle = Document::load(bundlePath);
    if (!bundle) { r.error = "failed to load bundle: " + bundlePath; return r; }

    // 解析 manifest
    std::vector<uint8_t> md;
    Manifest m;
    bool hasManifest = bundle->get(kManifestPath, md);
    if (hasManifest) {
        std::string ms(md.begin(), md.end());
        parseManifest(ms, m);
        r.sourceFilename = m.sourceFilename;
        r.manifestSourceSha256 = m.sourceSha256;
    }

    // 当前文件 SHA256
    r.currentSha256 = fileSha256(targetPath);
    if (r.currentSha256.empty()) {
        r.error = "failed to hash target: " + targetPath;
        return r;
    }
    r.hashMatched = !m.sourceSha256.empty() && m.sourceSha256 == r.currentSha256;

    if (verifyOnly) {
        r.ok = true;
        return r;
    }

    if (!r.hashMatched && !force) {
        r.error = "source SHA256 mismatch (file may have been modified); use --force to import anyway";
        return r;
    }

    auto target = Document::load(targetPath);
    if (!target) { r.error = "failed to load target: " + targetPath; return r; }

    // 先清掉目标里既有的 .co/ 条目，再从 bundle 复制
    auto names = target->list();
    for (const auto& name : names) {
        if (isCoEntry(name)) target->remove(name);
    }
    copyCoEntries(*bundle, *target);

    if (!target->write(targetPath)) {
        r.error = "failed to write target: " + targetPath;
        return r;
    }
    r.ok = true;
    r.injected = true;
    return r;
}

// ============ verify-bundle ============

VerifyReport verifyBundle(const std::string& bundlePath) {
    VerifyReport rep;
    auto bundle = Document::load(bundlePath);
    if (!bundle) { rep.error = "failed to load bundle"; return rep; }

    // manifest
    std::vector<uint8_t> md;
    if (bundle->get(kManifestPath, md)) {
        std::string ms(md.begin(), md.end());
        Manifest m;
        if (parseManifest(ms, m)) {
            rep.manifestValid = true;
            rep.manifestVersion = m.version;
            rep.hashAlgo = m.hashAlgo;
            rep.commitCount = m.commitCount;
        }
    }

    Store store(*bundle);

    // HEAD → commit 链
    std::string head = headCommitHash(*bundle);
    rep.headValid = false;
    if (!head.empty()) {
        auto c = readCommit(*bundle, head);
        if (c) rep.headValid = true;
    }

    // 所有 loose 对象可读 + 类型合法
    rep.allObjectsReadable = true;
    auto loose = store.listLooseObjects();
    for (const auto& h : loose) {
        auto obj = store.readObject(h);
        if (!obj) { rep.allObjectsReadable = false; break; }
        const std::string& t = obj->first;
        if (t != "commit" && t != "tree" && t != "blob") {
            rep.allObjectsReadable = false; break;
        }
    }

    // pack 完整性：idx 可读 + 每个对象区 CRC32 与 idx 记录一致
    rep.packsIntact = true;
    int64_t packedCount = 0;
    for (const auto& pack : store.listPacks()) {
        PackIndex idx;
        if (!readPackIndex(*bundle, pack, idx)) { rep.packsIntact = false; break; }
        std::string packPath = std::string(kPackDir) + "/" + pack + ".pack";
        std::vector<uint8_t> packData;
        if (!bundle->get(packPath, packData)) { rep.packsIntact = false; break; }

        // 逐对象校验 CRC：对象区 = 从 offset 起，到下一个对象 offset（或 trailer 前）止
        // trailer 长度 = kHashLen
        size_t trailer = kHashLen;
        for (size_t i = 0; i < idx.hashes.size(); ++i) {
            uint32_t off = idx.offsets[i];
            size_t end;
            if (i + 1 < idx.hashes.size()) {
                end = idx.offsets[i + 1];
            } else {
                if (packData.size() >= trailer) {
                    end = packData.size() - trailer;
                } else {
                    rep.packsIntact = false; break;
                }
            }
            if (off >= packData.size() || end > packData.size() || end <= off) {
                rep.packsIntact = false; break;
            }
            uint32_t crc = crc32IEEE(packData.data() + off, end - off);
            if (crc != idx.crcs[i]) { rep.packsIntact = false; break; }
            ++packedCount;
        }
        if (!rep.packsIntact) break;
    }

    rep.objectCount = static_cast<int64_t>(loose.size()) + packedCount;

    // 综合判定：manifest 合法 + HEAD 有效 + 对象可读 + pack 完整
    rep.ok = rep.manifestValid && rep.headValid && rep.allObjectsReadable && rep.packsIntact;
    return rep;
}

} // namespace co
