// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "index.hpp"

#include <cstdlib>
#include <sstream>
#include <string>

namespace co {

namespace {

constexpr const char* kIndexMagic = "COINDEX1\n";

// 按行分割（保留空行结构，仅以 '\n' 切分）
std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            lines.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < s.size()) lines.push_back(s.substr(start));
    return lines;
}

} // namespace

CommitIndex loadIndex(const Document& doc) {
    CommitIndex idx;
    std::vector<uint8_t> data;
    if (!doc.get(kIndexFile, data) || data.empty()) return idx;

    std::string s(data.begin(), data.end());
    if (s.rfind(kIndexMagic, 0) != 0) return idx;  // 格式不兼容

    auto lines = splitLines(s.substr(std::string(kIndexMagic).size()));
    for (const auto& line : lines) {
        if (line.empty()) continue;
        // path\tsize\tcrc\tblobhash
        std::vector<std::string> fields;
        size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == '\t') {
                fields.push_back(line.substr(start, i - start));
                start = i + 1;
                if (fields.size() == 4) break;
            }
        }
        if (fields.size() != 4) continue;
        IndexEntry e;
        e.path = fields[0];
        e.size = static_cast<uint64_t>(strtoull(fields[1].c_str(), nullptr, 10));
        e.crc = static_cast<uint32_t>(strtoul(fields[2].c_str(), nullptr, 10));
        e.blobHash = fields[3];
        if (!e.path.empty() && !e.blobHash.empty()) {
            idx.entries.push_back(std::move(e));
        }
    }
    idx.valid = true;
    return idx;
}

bool saveIndex(Document& doc, const CommitIndex& idx) {
    std::ostringstream os;
    os << kIndexMagic;
    for (const auto& e : idx.entries) {
        os << e.path << '\t' << e.size << '\t' << e.crc << '\t' << e.blobHash << '\n';
    }
    std::string s = os.str();
    std::vector<uint8_t> data(s.begin(), s.end());
    doc.set(kIndexFile, data);
    return true;
}

void removeIndex(Document& doc) {
    doc.remove(kIndexFile);
}

const IndexEntry* findEntry(const CommitIndex& idx, const std::string& path) {
    for (const auto& e : idx.entries) {
        if (e.path == path) return &e;
    }
    return nullptr;
}

} // namespace co
