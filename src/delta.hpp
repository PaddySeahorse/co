// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Git 风格的二进制 delta 编解码。delta 描述如何由 base 重建 target：
//
//   <base-size varint><target-size varint><instr...>
//
// 指令：
//   - 若 cmd 最高位为 1：copy。低 3 位指示偏移字节数（1~4），第 4~6 位指示
//     长度字节数（0~3）。size==0 表示 0x10000。
//   - 若 cmd 最高位为 0 且 cmd != 0：insert，cmd 为随后的字面量字节数（1~127）。
//   - cmd == 0 非法。
//
// 用于 packfile 的 REF deltification：相邻版本的同一文件高度相似，delta 体
// 远小于全量副本，配合 zlib 二次压缩可显著降低 .co 体积。

#pragma once
#include <cstdint>
#include <vector>

namespace co {

// 由 base 和 target 生成 delta。base 与 target 任一为空时退化为纯 insert。
std::vector<uint8_t> createDelta(const std::vector<uint8_t>& base,
                                 const std::vector<uint8_t>& target);

// 将 delta 应用到 base，重建 target。失败（格式错误、越界、长度不符）返回 false。
bool applyDelta(const std::vector<uint8_t>& base,
                const std::vector<uint8_t>& delta,
                std::vector<uint8_t>& out);

} // namespace co