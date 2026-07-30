// SPDX-License-Identifier: GPL-3.0-or-later
//
// co bundle-merge（第五章）：两个 .co-bundle 三方合并。
//
// 算法：
//   1. 取两 bundle 的 HEAD，沿首父链找共同祖先作为 base。
//   2. 对 base/ours/theirs 三个 tree 做逐文件三方合并：
//      - 一方未改 → 取另一方；
//      - 两方改得相同 → 取任一；
//      - 两方都改且改得不同：
//          * 文本文件（无 NUL 字节）做行级 diff3——非重叠改动自动合并，
//            重叠改动用 <<<<<<< / ======= / >>>>>>> 标记冲突；
//          * 二进制文件标记为二进制冲突，取 ours 并报告。
//   3. 合并结果写入新 bundle：复制两份历史对象，再写入合并 tree/blobs/merge commit。

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace co {

struct MergeResult {
    bool ok = false;
    bool hadConflicts = false;
    std::string outputPath;
    std::string mergeCommitHash;
    std::vector<std::string> conflicts;   // 冲突文件路径
    std::string commonAncestor;
    std::string error;
};

// 合并 bundleA 与 bundleB，输出到 outputPath。
// message 为 merge commit message；nowUnix 为时间戳。
MergeResult bundleMerge(const std::string& bundleA, const std::string& bundleB,
                        const std::string& outputPath,
                        const std::string& message, int64_t nowUnix);

} // namespace co
