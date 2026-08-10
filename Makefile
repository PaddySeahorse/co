# SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
BINARY=co
DIST_DIR=dist
VERSION?=$(shell git describe --tags --always --dirty)
# 哈希算法：sha1（默认，向后兼容）或 sha256。`make CO_HASH=sha256 build`
CO_HASH?=sha1

.PHONY: build build-sha1 build-sha256 test release clean

build:
	cmake -B build -DVERSION=$(VERSION) -DCO_HASH=$(CO_HASH)
	cmake --build build

build-sha1:
	cmake -B build-sha1 -DVERSION=$(VERSION) -DCO_HASH=sha1
	cmake --build build-sha1

build-sha256:
	cmake -B build-sha256 -DVERSION=$(VERSION) -DCO_HASH=sha256
	cmake --build build-sha256

# 回归测试：对象压缩格式与 ZIP method=8 的往返一致性（issue #8）
test: build
	bash scripts/tests/test_issue8_roundtrip.sh

release:
	VERSION=$(VERSION) ./scripts/release.sh

clean:
	rm -rf build build-sha1 build-sha256 $(DIST_DIR)
