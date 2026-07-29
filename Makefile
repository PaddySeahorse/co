BINARY=co
DIST_DIR=dist
VERSION?=$(shell git describe --tags --always --dirty)

.PHONY: build release clean

build:
	cmake -B build -DVERSION=$(VERSION)
	cmake --build build

release:
	VERSION=$(VERSION) ./scripts/release.sh

clean:
	rm -rf build $(DIST_DIR)
