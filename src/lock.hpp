// SPDX-License-Identifier: GPL-3.0-or-later
//
// 文件锁（并发安全，对应改造清单第八章）。
//
// 说明：Office 文件的 .co/ 目录是嵌在 ZIP 内部的虚拟目录，无法对 ZIP 内部
// 的条目做 flock。因此采用「伴随锁文件」方案：在目标文件同目录下创建
// <targetPath>.colock 这个真实文件，对其 flock(LOCK_EX)。
//
//   - 内嵌模式：锁文件 = <office文件>.colock
//   - 外部/bundle 模式：锁文件 = <bundle文件>.colock
//
// 5 秒超时返回失败（避免死等），RAII 析构自动释放。

#pragma once
#include <string>

namespace co {

class FileLock {
public:
    FileLock() = default;
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&&) noexcept;
    FileLock& operator=(FileLock&&) noexcept;

    // 获取 targetPath 的排他锁。超时（默认 5s）返回 false。
    // 成功后由 RAII 在析构时释放。
    bool acquire(const std::string& targetPath, unsigned timeoutMs = 5000);

    // 显式释放（析构也会调用）。
    void release();

    bool locked() const { return fd_ >= 0; }
    const std::string& path() const { return lockPath_; }

private:
    int fd_ = -1;
    std::string lockPath_;
};

} // namespace co
