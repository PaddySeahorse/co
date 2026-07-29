cmake_minimum_required(VERSION 3.16)
project(co LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 版本号注入：默认 dev，可通过 -DVERSION=xxx 覆盖
set(VERSION "dev" CACHE STRING "Version")

# 收集源文件
file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(co ${SOURCES})

target_include_directories(co PRIVATE src)

# 版本号作为编译期宏传给代码
target_compile_definitions(co PRIVATE VERSION="${VERSION}")

# 警告开关
target_compile_options(co PRIVATE -Wall -Wextra)

# 依赖：zlib（DEFLATE / CRC32）
find_package(ZLIB REQUIRED)
target_link_libraries(co PRIVATE ZLIB::ZLIB)

# 依赖：OpenSSL（SHA1）。当前环境有 OpenSSL，直接链接；
# 若环境无 OpenSSL，可在此处改为内置 SHA1 实现以保持可编译。
find_package(OpenSSL REQUIRED)
target_link_libraries(co PRIVATE OpenSSL::Crypto)
