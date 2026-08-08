# co (Office Object Storage MVP)

This repository contains the MVP skeleton of a CLI tool called `co`. It stores Git-style objects (blob/tree/commit) inside a `.co` directory embedded within `.docx`/`.xlsx`/`.pptx`/`.odt` files. All commands require an Office file path to prevent accidental modifications.

## Quickstart

```bash
co init "./example doc.docx"
co commit -m "Initial import" "./example doc.docx"
co log "./example doc.docx"
co status "./example doc.docx"
co diff HEAD~1 HEAD "./example doc.docx"
co gc "./example doc.docx"
co checkout <commit> "./example doc.docx"
```

## Commands

| Command | Description |
|---------|-------------|
| `init` | Initialize a `.co/` repository inside an Office file |
| `commit -m <msg>` | Create a blob/tree/commit for every file in the archive |
| `log` | Traverse the commit parent chain from `.co/HEAD` |
| `status` | Report the version-control status of the Office file |
| `diff <ref-a> <ref-b>` | Compare two commits and show content changes per entry (`--status` for the A/D/M file list) |
| `gc` | Pack reachable objects and prune unreachable ones |
| `checkout <commit>` | Restore the Office file to the given commit |
| `migrate` | Convert the repository's hash algorithm between SHA1 and SHA256 |
| `export` | Extract `.co/` history into a standalone `.co-bundle` |
| `import` | Inject a `.co-bundle`'s history back into an Office file |
| `verify-bundle` | Verify the integrity of a `.co-bundle` |
| `bundle-merge` | Three-way merge two `.co-bundle` files into a new bundle |

Refs for `diff`: `HEAD`, `HEAD~N`, a full hash, or a hash prefix.

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
- `.odt`

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

- `.docx`/`.xlsx`/`.pptx`/`.odt` files are treated as ZIP archives.
- Loose Git-style objects are written to `.co/objects/xx/yyyy...` using zlib compression.
- `commit` creates a blob for each file in the archive (excluding `.co`) and builds a flat tree.
- `log` traverses the commit parent chain starting from `.co/HEAD`.
- `gc` packs reachable objects into `.co/objects/pack/pack-<timestamp>.pack`, removing loose objects and old packfiles to reduce file size.
- Author identity is derived from `CO_AUTHOR_NAME` and `CO_AUTHOR_EMAIL` environment variables (with defaults if unset).

## Hash Algorithm

`co` stores objects under a content hash. The default is **SHA-1** for backward compatibility with existing repositories; **SHA-256** is available for stronger collision resistance.

Pick the algorithm at build time:

```bash
# SHA-1 (default)
cmake -B build -DVERSION=$(git describe --tags --always --dirty)

# SHA-256
cmake -B build -DCO_HASH=sha256 -DVERSION=$(git describe --tags --always --dirty)
cmake --build build
```

Or with the Makefile: `make CO_HASH=sha256 build`.

A single repository uses one algorithm. To switch an existing repository's algorithm, run `co migrate <path>` — it rewrites every object's hash. After migrating you **must rebuild the binary with the matching `-DCO_HASH=<target>`** to keep operating on that repository.
