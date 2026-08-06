# Requirements Document: Branch 功能

## Introduction

`co` 是一个 Git 风格的对象存储工具，将 `.co/` 目录嵌入 docx/xlsx/pptx（ZIP 格式）文件中。当前仓库只有单一指针 `.co/HEAD`，直接存储 commit hash。`commit` 无条件更新 HEAD，`checkout` 无条件改写 HEAD，`log`/`gc`/`migrate`/`status`/`diff` 均从 HEAD 出发遍历，无法支持并行开发线或保存多个命名引用。

本功能引入 Git 风格的分支机制：`refs/heads/<name>` 命名引用 + 符号 HEAD（symbolic ref）。使多个分支可以共存、创建、删除、切换，并在切回分支时保留各自的历史指针。

## Glossary

- **分支 (Branch)**: 存储在 `.co/refs/heads/<name>` 下的命名引用，内容为一条 commit hash。
- **符号 HEAD (Symbolic HEAD)**: 内容为 `ref: refs/heads/<name>` 的 HEAD 文件，表示 HEAD 间接指向某个分支。
- **分离 HEAD (Detached HEAD)**: HEAD 直接存储 commit hash（无符号引用），表示不处于任何分支。
- **当前分支 (Current Branch)**: HEAD 符号引用指向的分支，commit 会推进其指针。
- **外部模式 (External Mode)**: 历史存储在与 Office 文件分离的 `.co-bundle` 文件中（`--external <bundle>`）。

## Requirements

### Requirement 1: 分支引用存储

**User Story:** AS 用户, I want 分支引用以独立命名空间持久化, SO THAT 多个开发线可以在同一仓库内共存并分别保存各自最新的 commit。

#### Acceptance Criteria

1. WHEN 系统创建分支，系统 SHALL 将分支 commit hash 写入 `.co/refs/heads/<name>` 条目。
2. WHEN 系统需要读取分支，系统 SHALL 从 `.co/refs/heads/<name>` 条目读取 commit hash。
3. WHEN 系统需要列出所有分支，系统 SHALL 枚举 `.co/refs/heads/` 下的全部命名引用。
4. IF 分支引用条目不存在，系统 SHALL 将该分支视为空引用。

### Requirement 2: 符号 HEAD 支持

**User Story:** AS 用户, I want HEAD 能够符号指向当前分支, SO THAT commit 自动推进当前分支指针，checkout 能切换当前分支。

#### Acceptance Criteria

1. WHEN HEAD 内容以 `ref: refs/heads/<name>` 开头，系统 SHALL 将 HEAD 解析为对应分支的 commit hash。
2. WHEN HEAD 内容为纯 commit hash，系统 SHALL 将其视为分离 HEAD 并直接返回该 hash。
3. WHEN commit 完成，系统 SHALL 将新 commit hash 写入 HEAD 指向的分支引用，同时保持 HEAD 的符号形式不变。
4. WHEN 仓库处于分离 HEAD 且 commit 完成，系统 SHALL 保持 HEAD 为直接 hash 形式。

### Requirement 3: 分支创建

**User Story:** AS 用户, I want 从当前提交点创建新分支, SO THAT 可以从该点开始独立开发。

#### Acceptance Criteria

1. WHEN 用户执行 `co branch <name>`，系统 SHALL 在 `.co/refs/heads/<name>` 创建指向当前 HEAD commit 的分支引用。
2. IF 分支名已存在，系统 SHALL 拒绝创建并报告错误。
3. IF 分支名非法（空、包含空白或 `~^:?*[` 等字符、以 `.` 开头、以 `.lock` 结尾、超过长度限制），系统 SHALL 拒绝创建并报告错误。
4. IF 仓库尚未有 HEAD commit，系统 SHALL 拒绝创建并报告错误。

### Requirement 4: 分支列出

**User Story:** AS 用户, I want 查看所有分支并识别当前分支, SO THAT 我能了解可切换的目标。

#### Acceptance Criteria

1. WHEN 用户执行 `co branch`（无参数），系统 SHALL 列出全部分支名。
2. WHILE 当前分支存在于分支列表中，系统 SHALL 以 `* ` 前缀标记当前分支。
3. WHILE HEAD 为分离状态，系统 SHALL 在列表中显示分离 HEAD 提示。

### Requirement 5: 分支删除

**User Story:** AS 用户, I want 删除不再需要的分支, SO THAT 引用空间保持整洁。

#### Acceptance Criteria

1. WHEN 用户执行 `co branch -d <name>`，系统 SHALL 删除 `.co/refs/heads/<name>` 引用条目。
2. IF 目标分支为当前分支，系统 SHALL 拒绝删除并报告错误。
3. IF 目标分支不存在，系统 SHALL 报告错误。

### Requirement 6: 分支切换

**User Story:** AS 用户, I want 切换到指定分支, SO THAT 内容与历史指针都切换到该开发线。

#### Acceptance Criteria

1. WHEN 用户执行 `co checkout <branch>`，系统 SHALL 将 Office 内容恢复到该分支指向 commit 的 tree。
2. WHEN 用户执行 `co checkout <branch>`，系统 SHALL 将 HEAD 更新为指向该分支的符号引用。
3. IF 目标分支引用为空或目标分支不存在，系统 SHALL 报告错误。

### Requirement 7: 既有命令适配

**User Story:** AS 用户, I want log/gc/migrate/status/diff 等命令继续正常工作, SO THAT 分支机制不破坏既有工作流。

#### Acceptance Criteria

1. WHEN 用户对含分支的仓库执行 `co log`，系统 SHALL 从当前 HEAD（含符号解析）出发遍历历史。
2. WHEN 用户执行 `co gc`，系统 SHALL 将所有分支引用指向的对象视为可达并保留。
3. WHEN 用户执行 `co migrate`，系统 SHALL 重写所有分支引用指向的 commit hash。
4. WHEN 用户执行 `co status`，系统 SHALL 在输出中显示当前分支名。
5. WHEN 用户对无任何分支的旧仓库执行任意命令，系统 SHALL 保持与旧版本一致的行为。

### Requirement 8: 外部模式兼容

**User Story:** AS 用户, I want 分支机制在 `--external` 模式下同样可用, SO THAT bundle 历史存储也能享受分支能力。

#### Acceptance Criteria

1. WHEN 用户在外部模式下执行分支命令，系统 SHALL 将分支引用写入历史 bundle 文档。
2. WHEN 用户在外部模式下执行 `co log`/`co checkout` 等命令，系统 SHALL 从历史 bundle 解析分支引用。
3. IF 历史 bundle 不存在分支引用条目，系统 SHALL 按分离 HEAD 处理。
