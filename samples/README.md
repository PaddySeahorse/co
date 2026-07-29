# Office 样本文件清单

> 用途：为 `co` 工具准备真实 Microsoft Office 生成的文档，用于端到端兼容性测试。
>
> **核心约束**：每份样本必须由 **Microsoft Office 原生另存生成**。
> 不要 WPS / LibreOffice / Google Docs 导出的文件——ZIP「指纹」不对，
> 验证不了 `co` 的 `preserveRaw` 机制（README L50 承诺：处理后的文件仍能被 Office 正常打开）。

---

## 一、必交样本（最小集，7 个文件）

放入 `/workspaces/co/samples/` 下：

| # | 文件名 | 格式 | 内容要求 | 验证目标 |
|---|--------|------|---------|---------|
| 1 | `simple.docx` | docx | 几段纯文字，无图片 | 最小 OOXML 结构 + `co init/commit/checkout` 往返 |
| 2 | `simple.xlsx` | xlsx | 一个工作表，几行数字 | 同上，针对 xlsx |
| 3 | `simple.pptx` | pptx | 2–3 张幻灯片，纯文本 | 同上，针对 pptx |
| 4 | `rich.docx` | docx | 嵌入图片、表格、多种字体/样式 | 验证 `co` 不破坏二进制部件、多部件 ZIP 写序 |
| 5 | `rich.xlsx` | xlsx | 带公式（`SUM` 等）、一张图表、多个 sheet | 公式/图表是 OOXML 重灾区 |
| 6 | `rich.pptx` | pptx | 带母版改动、图片、动画 | pptx 的 `_rels` 和 `ppt/slides/` 结构复杂 |
| 7 | `with spaces.docx` | docx | 文件名带空格，内容随便 | README L40 专门讲了引号转义 |

---

## 二、强烈建议（边界场景，4 个文件）

| # | 文件 | 用途 |
|---|------|------|
| 8  | 一个 **5MB+** 的大 `.docx` 或 `.xlsx` | 触发 `gc` packfile 路径，验证大文件 ZIP 偏移处理 |
| 9  | 一个 **多 sheet + 命名区域** 的 `.xlsx` | OOXML 部件交叉引用最多，最容易暴露 `co` 改写问题 |
| 10 | 同一份文件 **用 Office 2016 和 Office 365 各另存一份** | 不同 Office 版本 ZIP 写法略有差异，验证兼容广度 |

完整集合计：约 11–12 个文件。

---

## 三、明确不要的样本（避免白做工）

| 类型 | 原因 |
|------|------|
| ❌ `.doc` / `.xls` / `.ppt` 老二进制格式 | README L33–36 明确只支持 OOXML 三种 |
| ❌ 加密 / 密码保护的 Office 文件 | `co` 没设计这个场景 |
| ❌ 损坏文件 | `co` 的边界测试 `test_bugs.cpp` 已经在造损坏样本了 |
| ❌ WPS / LibreOffice / Google Docs 导出的文件 | ZIP 指纹不对，验证不了 `preserveRaw` |

---

## 四、放好之后

样本丢进 `/workspaces/co/samples/` 后，告诉我「齐了」，我会：

1. 用 `unzip -l` 列出每个样本的部件结构，作为基线
2. 跑 `co init → commit → checkout → gc` 全流程
3. 用 `unzip -l` 再次对比，确认 `co` 没破坏非 `.co` 条目
4. 必要时建议你在 Office 里手动打开处理后文件，确认渲染正常

---

## 附：放置示意

```
/workspaces/co/
├── samples/
│   ├── simple.docx
│   ├── simple.xlsx
│   ├── simple.pptx
│   ├── rich.docx
│   ├── rich.xlsx
│   ├── rich.pptx
│   ├── with spaces.docx
│   └── （可选：大文件、多 sheet、跨版本）
├── src/
└── ...
```
