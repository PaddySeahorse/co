# co (Office Object Storage MVP)

This repository contains the MVP skeleton of a CLI tool called `co`. It stores Git-style objects (blob/tree/commit) inside a `.co` directory embedded within `.docx`/`.xlsx`/`.pptx` files. All commands require an Office file path to prevent accidental modifications.

## Quickstart

```bash
co init "./example doc.docx"
co commit -m "Initial import" "./example doc.docx"
co log "./example doc.docx"
co gc "./example doc.docx"
co checkout <commit> "./example doc.docx"
```

## Building

```bash
cmake -B build -DVERSION=$(git describe --tags --always --dirty)
cmake --build build
```

The binary will be at `build/co`. Dependencies: CMake 3.16+, a C++17 compiler, zlib, and OpenSSL.

## Usage

```bash
co --help
co --version
co <command> --help
```

## Supported File Types

- `.docx`
- `.xlsx`
- `.pptx`

## Quoting Paths With Spaces

If your path contains spaces, use quotes or backslash escaping:

```bash
co init "My File.docx"
co init My\ File.docx
```

## Common Pitfalls

- `commit` requires a commit message (`-m`).
- `co` preserves the original ZIP structure of Office files, so processed files should remain compatible with Microsoft Office. If you encounter any issues, please report them.
- Back up your files before using `co` to avoid history loss if an editor automatically cleans up the `.co` directory.

## How It Works

- `.docx`/`.xlsx`/`.pptx` files are treated as ZIP archives.
- Loose Git-style objects are written to `.co/objects/xx/yyyy...` using zlib compression.
- `commit` creates a blob for each file in the archive (excluding `.co`) and builds a flat tree.
- `log` traverses the commit parent chain starting from `.co/HEAD`.
- `gc` packs reachable objects into `.co/objects/pack/pack-<timestamp>.pack`, removing loose objects and old packfiles to reduce file size.
- Author identity is derived from `CO_AUTHOR_NAME` and `CO_AUTHOR_EMAIL` environment variables (with defaults if unset).
