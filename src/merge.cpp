// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "merge.hpp"
#include "bundle.hpp"
#include "objectstore.hpp"
#include "commit.hpp"
#include "util.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace co {

namespace {

// ============ 行级 diff3 ============

using Lines = std::vector<std::string>;

// 把字节按 '\n' 切成行（保留行内容，不含分隔符）。
Lines toLines(const std::vector<uint8_t>& data) {
    Lines lines;
    std::string cur;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += static_cast<char>(data[i]);
        }
    }
    if (!cur.empty()) lines.push_back(cur);  // 末尾无换行的残行
    return lines;
}

std::vector<uint8_t> fromLines(const Lines& lines, bool trailingNewline) {
    std::string s;
    for (size_t i = 0; i < lines.size(); ++i) {
        s += lines[i];
        s += '\n';
    }
    if (!trailingNewline && !lines.empty()) {
        s.pop_back();  // 去掉最后那个 '\n'
    }
    return std::vector<uint8_t>(s.begin(), s.end());
}

bool hasTrailingNewline(const std::vector<uint8_t>& data) {
    return !data.empty() && data.back() == '\n';
}

struct Hunk {
    size_t bStart = 0;
    size_t bEnd = 0;            // base 范围 [bStart, bEnd)
    Lines replacement;          // 对应 other 的行
};

// LCS 动态规划，返回 base/other 的匹配下标对（递增）
std::vector<std::pair<size_t,size_t>> lcsPairs(const Lines& a, const Lines& b) {
    size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            if (a[i] == b[j]) dp[i][j] = dp[i + 1][j + 1] + 1;
            else dp[i][j] = std::max(dp[i + 1][j], dp[i][j + 1]);
        }
    }
    std::vector<std::pair<size_t,size_t>> pairs;
    size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) { pairs.push_back({i, j}); ++i; ++j; }
        else if (dp[i + 1][j] >= dp[i][j + 1]) ++i;
        else ++j;
    }
    return pairs;
}

// 计算 base → other 的差异 hunk 列表
std::vector<Hunk> diffTo(const Lines& base, const Lines& other) {
    std::vector<Hunk> hunks;
    auto pairs = lcsPairs(base, other);
    size_t bi = 0, oj = 0;
    for (auto& p : pairs) {
        if (bi < p.first || oj < p.second) {
            Hunk h;
            h.bStart = bi;
            h.bEnd = p.first;
            h.replacement.assign(other.begin() + oj, other.begin() + p.second);
            hunks.push_back(std::move(h));
        }
        bi = p.first + 1;
        oj = p.second + 1;
    }
    if (bi < base.size() || oj < other.size()) {
        Hunk h;
        h.bStart = bi;
        h.bEnd = base.size();
        h.replacement.assign(other.begin() + oj, other.end());
        hunks.push_back(std::move(h));
    }
    return hunks;
}

// 两个区间是否重叠（含纯插入 [k,k) 与另一区间相邻视为可能冲突：此处仅判严格重叠/包含）
bool rangesOverlap(size_t as, size_t ae, size_t bs, size_t be) {
    // [as,ae) 与 [bs,be) 相交：as < be && bs < ae；对空区间特判同点插入
    if (as == ae && bs == be) return as == bs;       // 两处同点插入
    if (as == ae) return as > bs && as < be;          // 插入落在对方删除/修改区间内
    if (bs == be) return bs > as && bs < ae;
    return as < be && bs < ae;
}

// 行级三方合并。返回合并行；若产生冲突置 conflict=true。
Lines merge3Lines(const Lines& base, const Lines& ours, const Lines& theirs,
                  bool& conflict) {
    conflict = false;
    if (ours == base) return theirs;
    if (theirs == base) return ours;
    if (ours == theirs) return ours;

    auto oh = diffTo(base, ours);
    auto th = diffTo(base, theirs);

    // 检查是否存在重叠 hunk
    bool overlap = false;
    for (const auto& o : oh) {
        for (const auto& t : th) {
            if (rangesOverlap(o.bStart, o.bEnd, t.bStart, t.bEnd)) {
                overlap = true; break;
            }
        }
        if (overlap) break;
    }

    if (!overlap) {
        // 非重叠：按 base 顺序同时套用两边的 hunk
        Lines result;
        size_t i = 0;
        size_t oi = 0, ti = 0;
        while (i <= base.size()) {
            const Hunk* o = (oi < oh.size() && oh[oi].bStart == i) ? &oh[oi] : nullptr;
            const Hunk* t = (ti < th.size() && th[ti].bStart == i) ? &th[ti] : nullptr;
            if (o && t) {
                // 同点：先插 ours 再插 theirs（两者 bStart 相同且不重叠）
                result.insert(result.end(), o->replacement.begin(), o->replacement.end());
                result.insert(result.end(), t->replacement.begin(), t->replacement.end());
                if (o->bEnd > i) i = o->bEnd;
                if (t->bEnd > i) i = t->bEnd;
                ++oi; ++ti;
            } else if (o) {
                result.insert(result.end(), o->replacement.begin(), o->replacement.end());
                i = o->bEnd; ++oi;
            } else if (t) {
                result.insert(result.end(), t->replacement.begin(), t->replacement.end());
                i = t->bEnd; ++ti;
            } else {
                if (i < base.size()) result.push_back(base[i]);
                ++i;
            }
        }
        return result;
    }

    // 重叠：整体冲突块（blob 级标记）
    conflict = true;
    Lines result;
    result.push_back("<<<<<<< ours");
    result.insert(result.end(), ours.begin(), ours.end());
    result.push_back("=======");
    result.insert(result.end(), theirs.begin(), theirs.end());
    result.push_back(">>>>>>> theirs");
    return result;
}

// 判断 blob 是否为文本（无 NUL 字节）
bool isText(const std::vector<uint8_t>& data) {
    for (uint8_t c : data) if (c == 0) return false;
    return true;
}

} // namespace

// ============ bundleMerge ============

MergeResult bundleMerge(const std::string& bundleA, const std::string& bundleB,
                        const std::string& outputPath,
                        const std::string& message, int64_t nowUnix) {
    MergeResult r;
    r.outputPath = outputPath;

    auto docA = Document::load(bundleA);
    auto docB = Document::load(bundleB);
    if (!docA) { r.error = "failed to load bundle A: " + bundleA; return r; }
    if (!docB) { r.error = "failed to load bundle B: " + bundleB; return r; }

    std::string ha = Store(*docA).head();
    std::string hb = Store(*docB).head();
    if (ha.empty()) { r.error = "bundle A has no history"; return r; }
    if (hb.empty()) { r.error = "bundle B has no history"; return r; }

    // 共同祖先：收集 A 的祖先集，在 B 链中找首个命中
    std::set<std::string> ancestorsA;
    {
        std::string cur = ha;
        while (!cur.empty()) {
            if (!ancestorsA.insert(cur).second) break;
            auto c = readCommit(*docA, cur);
            if (!c) break;
            cur = c->parent;
        }
    }
    std::string base;
    {
        std::string cur = hb;
        while (!cur.empty()) {
            if (ancestorsA.count(cur)) { base = cur; break; }
            auto c = readCommit(*docB, cur);
            if (!c) break;
            cur = c->parent;
        }
    }
    r.commonAncestor = base;

    // 三个 tree
    auto ca = readCommit(*docA, ha);
    auto cb = readCommit(*docB, hb);
    if (!ca || !cb) { r.error = "cannot read head commits"; return r; }
    auto ta = readTree(*docA, ca->tree);
    auto tb = readTree(*docB, cb->tree);
    std::map<std::string, std::string> treeBase;
    if (!base.empty()) {
        // base 同时在 A、B 的祖先链中，两边都能读到；优先 A，失败回退 B
        auto cc = readCommit(*docA, base);
        if (!cc) cc = readCommit(*docB, base);
        if (cc) {
            std::vector<TreeEntry> tbe = readTree(*docA, cc->tree);
            if (tbe.empty()) tbe = readTree(*docB, cc->tree);
            for (const auto& e : tbe) treeBase[e.path] = e.hash;
        }
    }
    std::map<std::string, std::string> treeA, treeB;
    for (const auto& e : ta) treeA[e.path] = e.hash;
    for (const auto& e : tb) treeB[e.path] = e.hash;

    // 收集所有路径
    std::set<std::string> paths;
    for (const auto& kv : treeA) paths.insert(kv.first);
    for (const auto& kv : treeB) paths.insert(kv.first);
    for (const auto& kv : treeBase) paths.insert(kv.first);

    // 构建合并 bundle：先复制 A 的全部 .co/ 对象，再覆盖 B 的（同 hash 同内容，无副作用）
    auto out = std::make_unique<Document>();
    auto copyCo = [](const Document& src, Document& dst) {
        for (const auto& name : src.list()) {
            if (isCoEntry(name)) {
                const ZipEntry* e = src.getEntry(name);
                if (e) dst.setEntry(name, *e);
            }
        }
    };
    copyCo(*docA, *out);
    copyCo(*docB, *out);
    Store outStore(*out);

    // 逐路径三方合并
    std::vector<TreeEntry> mergedTree;
    for (const auto& path : paths) {
        auto ia = treeA.find(path);
        auto ib = treeB.find(path);
        auto iz = treeBase.find(path);
        bool aHas = ia != treeA.end();
        bool bHas = ib != treeB.end();
        bool zHas = iz != treeBase.end();
        std::string ha2 = aHas ? ia->second : "";
        std::string hb2 = bHas ? ib->second : "";
        std::string hz = zHas ? iz->second : "";

        std::string chosenHash;
        bool conflictPath = false;

        if (aHas && bHas && ha2 == hb2) {
            chosenHash = ha2;  // 两方一致
        } else if (zHas && aHas && ha2 == hz) {
            chosenHash = hb2;  // A 未改，取 B
        } else if (zHas && bHas && hb2 == hz) {
            chosenHash = ha2;  // B 未改，取 A
        } else if (zHas && !aHas && !bHas) {
            // 两方都删除：不加入合并 tree
            continue;
        } else if (zHas && aHas && !bHas && ha2 == hz) {
            // B 删除，A 未改：删除
            continue;
        } else if (zHas && bHas && !aHas && hb2 == hz) {
            // A 删除，B 未改：删除
            continue;
        } else {
            // 两方都改了（或新增冲突），需要内容级合并
            std::vector<uint8_t> da, db, dz;
            if (aHas) { auto o = outStore.readObject(ha2); if (o) da = o->second; }
            if (bHas) { auto o = outStore.readObject(hb2); if (o) db = o->second; }
            if (zHas) { auto o = outStore.readObject(hz); if (o) dz = o->second; }

            if (!aHas) { da = dz; }      // A 删除视为与 base 相同以便三方合并
            if (!bHas) { db = dz; }

            std::vector<uint8_t> merged;
            if (isText(da) && isText(db) && isText(dz)) {
                bool nlA = hasTrailingNewline(da);
                bool nlB = hasTrailingNewline(db);
                bool nlZ = hasTrailingNewline(dz);
                bool conflict = false;
                Lines ml = merge3Lines(toLines(dz), toLines(da), toLines(db), conflict);
                bool trailingNL = nlA || nlB || nlZ;
                merged = fromLines(ml, trailingNL);
                if (conflict) {
                    conflictPath = true;
                    r.conflicts.push_back(path);
                }
            } else {
                // 二进制冲突：取 ours，报告
                merged = da.empty() ? db : da;
                conflictPath = true;
                r.conflicts.push_back(path + " (binary)");
            }
            chosenHash = outStore.writeBlob(merged);
        }

        if (!chosenHash.empty()) {
            mergedTree.push_back({path, chosenHash});
        }
        (void)conflictPath;
    }

    // 写合并 tree 与 merge commit（两父）
    std::string treeHash = writeTree(outStore, mergedTree);
    std::vector<std::string> parents{ha, hb};
    std::string mergeMsg = message.empty() ? ("Merge " + ha.substr(0,7) + " & " + hb.substr(0,7)) : message;
    std::string commitHash = createMergeCommit(*out, treeHash, parents, mergeMsg, nowUnix);
    r.mergeCommitHash = commitHash;
    r.hadConflicts = !r.conflicts.empty();

    // manifest
    Manifest m;
    m.version = "1.0";
    m.sourceFilename = "merged";
    m.sourceSha256 = "";
    m.createdAt = "";  // computeStatus/manifest 不强依赖
    m.commitCount = static_cast<int64_t>(logCommits(*out).size());
    m.bundleSizeBytes = 0;
    m.coVersion = CO_VERSION_STR;
    m.hashAlgo = currentHashAlgoName();
    std::string msj = serializeManifest(m);
    std::vector<uint8_t> md(msj.begin(), msj.end());
    out->set(kManifestPath, md);

    if (!out->write(outputPath)) {
        r.error = "failed to write merged bundle: " + outputPath;
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace co
