// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "diff_content.hpp"
#include "objectstore.hpp"
#include "commit.hpp"
#include "status.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace co {

namespace {

using Bytes = std::vector<uint8_t>;
using Lines = std::vector<std::string>;

// LCS 兜底阈值：DP 表元素数超过此值则降级为整块删除 + 整块新增。
constexpr size_t kLcsLimit = 4 * 1024 * 1024;

// ============ 文本判定 ============

// 简单嗅探：前 64KB 无 NUL 视为文本（OOXML/JSON/文本均不含 NUL，图片等二进制必含）。
bool isLikelyText(const Bytes& data) {
    size_t n = std::min<size_t>(data.size(), 65536);
    for (size_t i = 0; i < n; ++i) {
        if (data[i] == 0) return false;
    }
    return true;
}

// ============ UTF-8 编解码 ============

// 解码为 Unicode 码点序列；非法字节按单字节码点保留（避免把多字节字符切坏）。
std::vector<uint32_t> decodeUtf8(const std::string& s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            cps.push_back(c);
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
            cps.push_back(((c & 0x1F) << 6) |
                          (static_cast<unsigned char>(s[i + 1]) & 0x3F));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80) {
            cps.push_back(((c & 0x0F) << 12) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 2]) & 0x3F));
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 3]) & 0xC0) == 0x80) {
            cps.push_back(((c & 0x07) << 18) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                          ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 3]) & 0x3F));
            i += 4;
        } else {
            cps.push_back(c);
            ++i;
        }
    }
    return cps;
}

void appendCp(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ============ 行级 LCS ============

struct LineHunk {
    size_t bStart = 0;
    size_t bEnd = 0;
    Lines replacement;
};

// 返回 base/other 的匹配下标对（递增）
std::vector<std::pair<size_t, size_t>> lcsPairs(const Lines& a, const Lines& b) {
    size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            dp[i][j] = (a[i] == b[j]) ? dp[i + 1][j + 1] + 1
                                      : std::max(dp[i + 1][j], dp[i][j + 1]);
        }
    }
    std::vector<std::pair<size_t, size_t>> pairs;
    size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            pairs.push_back({i, j});
            ++i;
            ++j;
        } else if (dp[i + 1][j] >= dp[i][j + 1]) {
            ++i;
        } else {
            ++j;
        }
    }
    return pairs;
}

// base → other 的行级 diff hunk 列表（语义同 merge.cpp 的 diffTo）
std::vector<LineHunk> lineHunks(const Lines& a, const Lines& b) {
    if ((a.size() + 1) * (b.size() + 1) > kLcsLimit) {
        return {{0, a.size(), b}};
    }
    std::vector<LineHunk> hunks;
    auto pairs = lcsPairs(a, b);
    size_t bi = 0, oj = 0;
    for (const auto& p : pairs) {
        if (bi < p.first || oj < p.second) {
            LineHunk h;
            h.bStart = bi;
            h.bEnd = p.first;
            h.replacement.assign(b.begin() + oj, b.begin() + p.second);
            hunks.push_back(std::move(h));
        }
        bi = p.first + 1;
        oj = p.second + 1;
    }
    if (bi < a.size() || oj < b.size()) {
        LineHunk h;
        h.bStart = bi;
        h.bEnd = a.size();
        h.replacement.assign(b.begin() + oj, b.end());
        hunks.push_back(std::move(h));
    }
    return hunks;
}

// ============ 字符级 LCS ============

struct Op {
    char kind;       // '=' 相同 / '-' 删除 / '+' 新增
    uint32_t cp;
};

// 对码点序列段 [as, as+an) 与 [bs, bs+bn) 做 LCS，把编辑操作追加到 ops。
void lcsOps(const std::vector<uint32_t>& a, size_t as, size_t an,
            const std::vector<uint32_t>& b, size_t bs, size_t bn,
            std::vector<Op>& ops) {
    if (an == 0) {
        for (size_t k = 0; k < bn; ++k) ops.push_back({'+', b[bs + k]});
        return;
    }
    if (bn == 0) {
        for (size_t k = 0; k < an; ++k) ops.push_back({'-', a[as + k]});
        return;
    }
    if ((an + 1) * (bn + 1) > kLcsLimit) {
        for (size_t k = 0; k < an; ++k) ops.push_back({'-', a[as + k]});
        for (size_t k = 0; k < bn; ++k) ops.push_back({'+', b[bs + k]});
        return;
    }
    std::vector<std::vector<int>> dp(an + 1, std::vector<int>(bn + 1, 0));
    for (size_t i = an; i-- > 0;) {
        for (size_t j = bn; j-- > 0;) {
            dp[i][j] = (a[as + i] == b[bs + j]) ? dp[i + 1][j + 1] + 1
                                                : std::max(dp[i + 1][j], dp[i][j + 1]);
        }
    }
    size_t i = 0, j = 0;
    while (i < an && j < bn) {
        if (a[as + i] == b[bs + j]) {
            ops.push_back({'=', a[as + i]});
            ++i;
            ++j;
        } else if (dp[i + 1][j] >= dp[i][j + 1]) {
            ops.push_back({'-', a[as + i]});
            ++i;
        } else {
            ops.push_back({'+', b[bs + j]});
            ++j;
        }
    }
    while (i < an) { ops.push_back({'-', a[as + i]}); ++i; }
    while (j < bn) { ops.push_back({'+', b[bs + j]}); ++j; }
}

// 两段码点序列的字符级 diff：先剥离共同前缀/后缀，再 LCS，
// 连续删除/新增块各聚合为一行输出（unified 风格：先 - 后 +）。
void appendCharDiff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                    std::string& out) {
    size_t as = 0, ae = a.size(), bs = 0, be = b.size();
    while (as < ae && bs < be && a[as] == b[bs]) { ++as; ++bs; }
    while (ae > as && be > bs && a[ae - 1] == b[be - 1]) { --ae; --be; }
    std::vector<Op> ops;
    lcsOps(a, as, ae - as, b, bs, be - bs, ops);
    size_t k = 0;
    while (k < ops.size()) {
        if (ops[k].kind == '-') {
            std::string line;
            while (k < ops.size() && ops[k].kind == '-') { appendCp(line, ops[k].cp); ++k; }
            out += "-" + line + "\n";
        } else if (ops[k].kind == '+') {
            std::string line;
            while (k < ops.size() && ops[k].kind == '+') { appendCp(line, ops[k].cp); ++k; }
            out += "+" + line + "\n";
        } else {
            ++k;
        }
    }
}

// ============ 渲染 ============

Lines toLines(const Bytes& data) {
    Lines lines;
    std::string cur;
    for (uint8_t c : data) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += static_cast<char>(c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// 文本条目：行级 hunk，单行替换时升级为字符级 diff。
void appendTextDiff(const Bytes& oldData, const Bytes& newData, std::string& out) {
    Lines a = toLines(oldData);
    Lines b = toLines(newData);
    auto hunks = lineHunks(a, b);
    for (const auto& h : hunks) {
        size_t del = h.bEnd - h.bStart;
        size_t add = h.replacement.size();
        if (del == 1 && add == 1 && a[h.bStart] != h.replacement[0]) {
            appendCharDiff(decodeUtf8(a[h.bStart]), decodeUtf8(h.replacement[0]), out);
        } else {
            for (size_t i = h.bStart; i < h.bEnd; ++i) out += "-" + a[i] + "\n";
            for (const auto& l : h.replacement) out += "+" + l + "\n";
        }
    }
}

// 整文件新增/删除：文本逐行加前缀，二进制/空输出单行 @ 占位。
void appendWholeFile(const Bytes& data, bool added, const std::string& path, std::string& out) {
    char mark = added ? '+' : '-';
    if (!isLikelyText(data)) {
        out += std::string(1, mark) + " @" + path + "\n";
        return;
    }
    Lines lines = toLines(data);
    if (lines.empty()) {
        out += std::string(1, mark) + " @" + path + "\n";
        return;
    }
    for (const auto& l : lines) out += std::string(1, mark) + l + "\n";
}

} // namespace

// ============ 主入口 ============

void renderCommitDiff(Document& doc, const std::string& refA,
                      const std::string& refB, std::string& out) {
    std::string a = resolveRef(doc, refA);
    std::string b = resolveRef(doc, refB);
    if (a.empty() || b.empty()) return;

    auto ca = readCommit(doc, a);
    auto cb = readCommit(doc, b);
    if (!ca || !cb) return;

    std::map<std::string, std::string> ta, tb;
    for (const auto& e : readTree(doc, ca->tree)) ta[e.path] = e.hash;
    for (const auto& e : readTree(doc, cb->tree)) tb[e.path] = e.hash;

    Store store(doc);

    std::set<std::string> paths;
    for (const auto& kv : ta) paths.insert(kv.first);
    for (const auto& kv : tb) paths.insert(kv.first);

    for (const auto& p : paths) {
        auto ia = ta.find(p);
        auto ib = tb.find(p);
        const bool inA = ia != ta.end();
        const bool inB = ib != tb.end();
        if (inA && inB && ia->second == ib->second) continue;

        out += "=== " + p + " ===\n";

        if (inA && !inB) {
            auto oa = store.readObject(ia->second);
            if (oa) appendWholeFile(oa->second, false, p, out);
            else out += "- @" + p + "\n";
        } else if (!inA && inB) {
            auto ob = store.readObject(ib->second);
            if (ob) appendWholeFile(ob->second, true, p, out);
            else out += "+ @" + p + "\n";
        } else {
            auto oa = store.readObject(ia->second);
            auto ob = store.readObject(ib->second);
            if (!oa || !ob) continue;
            const bool oldText = isLikelyText(oa->second);
            const bool newText = isLikelyText(ob->second);
            if (oldText && newText) {
                appendTextDiff(oa->second, ob->second, out);
            } else if (!oldText && !newText) {
                out += "M @" + p + "\n";
            } else {
                appendWholeFile(oa->second, false, p, out);
                appendWholeFile(ob->second, true, p, out);
            }
        }
    }
}

} // namespace co
