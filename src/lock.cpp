// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lock.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <ctime>
#include <cerrno>
#include <cstring>

namespace co {

namespace {

// 睡眠 millis 毫秒
void sleepMs(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(ms / 1000);
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
#endif
}

} // namespace

FileLock::~FileLock() {
    release();
}

FileLock::FileLock(FileLock&& o) noexcept : fd_(o.fd_), lockPath_(std::move(o.lockPath_)) {
    o.fd_ = -1;
}

FileLock& FileLock::operator=(FileLock&& o) noexcept {
    if (this != &o) {
        release();
        fd_ = o.fd_;
        lockPath_ = std::move(o.lockPath_);
        o.fd_ = -1;
    }
    return *this;
}

bool FileLock::acquire(const std::string& targetPath, unsigned timeoutMs) {
    release();
    lockPath_ = targetPath + ".colock";

#ifdef _WIN32
    int fd = _open(lockPath_.c_str(), _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) return false;
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        _close(fd);
        return false;
    }

    unsigned waited = 0;
    const unsigned step = 100;
    OVERLAPPED ov = {};
    while (true) {
        if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
            fd_ = fd;
            return true;
        }
        if (GetLastError() != ERROR_LOCK_VIOLATION) {
            _close(fd);
            return false;
        }
        if (waited >= timeoutMs) {
            _close(fd);
            return false;  // 超时
        }
        sleepMs(step);
        waited += step;
    }
#else
    int fd = open(lockPath_.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) return false;

    unsigned waited = 0;
    const unsigned step = 100;
    while (true) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            fd_ = fd;
            return true;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            // 其他错误（如 EBADF）直接放弃
            close(fd);
            return false;
        }
        if (waited >= timeoutMs) {
            close(fd);
            return false;  // 超时
        }
        sleepMs(step);
        waited += step;
    }
#endif
}

void FileLock::release() {
    if (fd_ >= 0) {
#ifdef _WIN32
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd_));
        OVERLAPPED ov = {};
        UnlockFileEx(h, 0, 1, 0, &ov);
        _close(fd_);
#else
        flock(fd_, LOCK_UN);
        close(fd_);
#endif
        fd_ = -1;
    }
    // 保留锁文件（伴随文件，不 unlink 以避免与等待中的进程竞争 inode）。
    lockPath_.clear();
}

} // namespace co
