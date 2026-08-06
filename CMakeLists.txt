# SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(co LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 版本号注入：默认 dev，可通过 -DVERSION=xxx 覆盖
set(VERSION "dev" CACHE STRING "Version")

# 哈希算法选择（第六章）：默认 sha1（向后兼容旧仓库），可设为 sha256 启用 SHA256。
# 该选项决定 kHashLen（util.hpp）与对象路径/tree/commit 引用长度。
# 注意：SHA1 与 SHA256 构建的二进制互不互通，需用 `co migrate` 转换仓库。
set(CO_HASH "sha1" CACHE STRING "Hash algorithm (sha1|sha256)")
if(CO_HASH STREQUAL "sha256")
    set(CO_HASH_DEFINE CO_HASH_SHA256)
elseif(CO_HASH STREQUAL "sha1")
    set(CO_HASH_DEFINE "")
else()
    message(FATAL_ERROR "Unsupported CO_HASH='${CO_HASH}' (expected: sha1|sha256)")
endif()

# 收集源文件
file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(co ${SOURCES})

target_include_directories(co PRIVATE src)

# 版本号与哈希算法作为编译期宏传给代码
target_compile_definitions(co PRIVATE
    VERSION="${VERSION}"
    CO_VERSION_STR="${VERSION}"
)
if(CO_HASH_DEFINE)
    target_compile_definitions(co PRIVATE ${CO_HASH_DEFINE})
endif()

# 警告开关
target_compile_options(co PRIVATE -Wall -Wextra)

# 依赖：zlib（DEFLATE / CRC32）
find_package(ZLIB REQUIRED)
target_link_libraries(co PRIVATE ZLIB::ZLIB)

# 依赖：OpenSSL（SHA1）。当前环境有 OpenSSL，直接链接；
# 若环境无 OpenSSL，可在此处改为内置 SHA1 实现以保持可编译。
find_package(OpenSSL REQUIRED)
target_link_libraries(co PRIVATE OpenSSL::Crypto)

# 依赖：zstd（对象存储的 zstd + 内置字典压缩）。Debian 等发行版仅提供 .pc，
# 故通过 pkg-config 直接引入，避免依赖 shipped cmake config。
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
target_link_libraries(co PRIVATE PkgConfig::ZSTD)
