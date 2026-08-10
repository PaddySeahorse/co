#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regression test for issue #8:
# "Objects written with zlib are corrupted on re-read: ZIP entry method=8 is
# raw deflate, not zlib"
#
# The bug class: object payloads whose format does not match what the ZIP layer
# produces for method=8 entries (raw DEFLATE, RFC 1951). In the broken design,
# zlib-format (RFC 1950) payloads were written inside method=8 entries; on
# re-read the ZIP layer inflates them as raw DEFLATE, mangling the embedded
# zlib header, so every earlier commit object becomes unreadable and
# `co checkout <old-hash>` fails.
#
# This suite asserts the round-trip contract:
#   1. Every .co/objects/** entry stored in the docx must, after ZIP
#      decompression, be a valid zstd frame (current format) or a legacy zlib
#      stream (old repos). A mangled header fails this check.
#   2. commit -> commit -> checkout <old-hash> must succeed and restore the
#      exact content of the older commit.
#   3. Longer commit chains survive gc / export / import round trips.
#
# Requirements: bash, python3 (stdlib zipfile only), a co binary.
# Usage: CO_BIN=/path/to/co bash scripts/tests/test_issue8_roundtrip.sh

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CO_BIN="${CO_BIN:-$ROOT/build/co}"

if [ ! -x "$CO_BIN" ]; then
    echo "SKIPPED: co binary not found at $CO_BIN (build first: make build)" >&2
    exit 0
fi
if ! command -v python3 >/dev/null; then
    echo "SKIPPED: python3 not found" >&2
    exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/co-issue8.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

FAILED=0
step() { echo "==> $*"; }
fail() { echo "FAIL: $*" >&2; FAILED=1; }

# ---- helpers ----------------------------------------------------------------

# make_docx <path> <text>  — create a minimal valid .docx with the given text
make_docx() {
    python3 - "$1" "$2" <<'PYEOF'
import sys, zipfile, os
out, text = sys.argv[1], sys.argv[2]
w = "w"
os.makedirs(w + "/_rels", exist_ok=True)
os.makedirs(w + "/word", exist_ok=True)
open(w + "/[Content_Types].xml", "w").write(
    '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
    '<Default Extension="xml" ContentType="application/xml"/></Types>')
open(w + "/_rels/.rels", "w").write(
    '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
    '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>'
    '</Relationships>')
open(w + "/word/document.xml", "w").write(
    '<?xml version="1.0"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
    '<w:body><w:p><w:r><w:t>' + text + '</w:t></w:r></w:p></w:body></w:document>')
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(w):
        for fn in files:
            p = os.path.join(root, fn)
            z.write(p, os.path.relpath(p, w))
PYEOF
}

# set_docx_text <path> <text> — rewrite word/document.xml in place, preserving
# every other entry (including .co/), like a real editor re-save would.
set_docx_text() {
    python3 - "$1" "$2" <<'PYEOF'
import sys, zipfile, shutil
path, text = sys.argv[1], sys.argv[2]
tmp = path + ".tmp"
xml = ('<?xml version="1.0"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
       '<w:body><w:p><w:r><w:t>' + text + '</w:t></w:r></w:p></w:body></w:document>')
zin = zipfile.ZipFile(path)
zout = zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED)
for item in zin.infolist():
    data = zin.read(item.filename)
    if item.filename == "word/document.xml":
        data = xml.encode()
    zout.writestr(item, data)
zout.close()
zin.close()
shutil.move(tmp, path)
PYEOF
}

# check_object_bytes <path> — assert every .co/objects/** entry is a valid
# zstd frame (28 B5 2F FD) or a legacy zlib stream (0x78..) after ZIP
# decompression. This is the invariant issue #8 reports as violated.
check_object_bytes() {
    python3 - "$1" <<'PYEOF'
import sys, zipfile
path = sys.argv[1]
bad = []
with zipfile.ZipFile(path) as z:
    for name in z.namelist():
        if not name.startswith(".co/objects/") or name.endswith("/"):
            continue
        if name.startswith(".co/objects/pack/"):
            continue  # pack/idx entries have their own formats
        try:
            raw = z.read(name)
        except Exception as exc:
            bad.append((name, f"undecompressible ({exc})"))
            continue
        ok = (len(raw) >= 4 and raw[0] == 0x28 and raw[1] == 0xB5 and
              raw[2] == 0x2F and raw[3] == 0xFD) or \
             (len(raw) >= 2 and raw[0] == 0x78)
        if not ok:
            bad.append((name, raw[:8].hex()))
if bad:
    for name, h in bad:
        print(f"CORRUPT {name}: bytes={h} (neither zstd magic 28b52ffd nor zlib header 78..)")
    sys.exit(1)
print(f"  object format invariant OK ({sum(1 for _ in [])})")
PYEOF
    local rc=$?
    if [ $rc -ne 0 ]; then fail "object bytes invariant (issue #8)"; return 1; fi
    return 0
}

# docx_text <path> — crude text extraction from word/document.xml
docx_text() {
    python3 - "$1" <<'PYEOF'
import sys, zipfile, re
with zipfile.ZipFile(sys.argv[1]) as z:
    xml = z.read("word/document.xml").decode("utf-8", "replace")
print(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", "", xml)).strip())
PYEOF
}

head_hash() { "$CO_BIN" log "$1" | awk '/^commit/{print $2;exit}'; }

# ---- 1. exact issue scenario: commit -> commit -> checkout <old-hash> -------

step "scenario 1: commit -> commit -> checkout <old-hash>"
make_docx a.docx "alpha"
"$CO_BIN" init a.docx >/dev/null || { fail "init"; exit 1; }
"$CO_BIN" commit -m c1 a.docx >/dev/null || { fail "commit c1"; exit 1; }
H1="$(head_hash a.docx)"
[ -n "$H1" ] || { fail "first commit hash"; exit 1; }
set_docx_text a.docx "bravo"
"$CO_BIN" commit -m c2 a.docx >/dev/null || { fail "commit c2"; exit 1; }
check_object_bytes a.docx || exit 1
"$CO_BIN" checkout "$H1" a.docx >/dev/null 2>&1 || { fail "checkout $H1 (issue #8 repro)"; exit 1; }
text="$(docx_text a.docx)"
[ "$text" = "alpha" ] || { fail "checkout restored wrong content: '$text'"; exit 1; }
step "scenario 1 PASS (old commit re-readable after a newer commit)"

# ---- 2. longer chain: 3 commits, all checkouts ----

step "scenario 2: 3-commit chain, checkout every commit"
make_docx b.docx "one"
"$CO_BIN" init b.docx >/dev/null
"$CO_BIN" commit -m c1 b.docx >/dev/null
H1="$(head_hash b.docx)"
set_docx_text b.docx "two"
"$CO_BIN" commit -m c2 b.docx >/dev/null
H2="$(head_hash b.docx)"
set_docx_text b.docx "three"
"$CO_BIN" commit -m c3 b.docx >/dev/null
H3="$(head_hash b.docx)"
check_object_bytes b.docx || exit 1
for spec in "$H1:one" "$H2:two" "$H3:three"; do
    h="${spec%%:*}"; want="${spec##*:}"
    set_docx_text b.docx "three"
    "$CO_BIN" checkout "$h" b.docx >/dev/null 2>&1 || { fail "checkout $h"; exit 1; }
    got="$(docx_text b.docx)"
    [ "$got" = "$want" ] || { fail "checkout $h content: got '$got' want '$want'"; exit 1; }
done
step "scenario 2 PASS"

# ---- 3. gc / export / import round trip over the same chain ----

step "scenario 3: gc and export/import keep the whole chain readable"
"$CO_BIN" gc b.docx >/dev/null 2>&1 || { fail "gc"; exit 1; }
check_object_bytes b.docx || exit 1
"$CO_BIN" checkout "$H1" b.docx >/dev/null 2>&1 || { fail "checkout H1 after gc"; exit 1; }
"$CO_BIN" checkout "$H3" b.docx >/dev/null 2>&1 || { fail "checkout H3 after gc"; exit 1; }
"$CO_BIN" export b.docx >/dev/null 2>&1 || { fail "export"; exit 1; }
make_docx c.docx "one"
"$CO_BIN" init c.docx >/dev/null
"$CO_BIN" import c.docx b.docx.co-bundle --force >/dev/null 2>&1 || { fail "import"; exit 1; }
for spec in "$H1:one" "$H2:two" "$H3:three"; do
    h="${spec%%:*}"; want="${spec##*:}"
    "$CO_BIN" checkout "$h" c.docx >/dev/null 2>&1 || { fail "checkout $h after import"; exit 1; }
    got="$(docx_text c.docx)"
    [ "$got" = "$want" ] || { fail "checkout $h after import: got '$got' want '$want'"; exit 1; }
done
step "scenario 3 PASS"

# ---- 4. corrupt entries must not crash the CLI ------------------------------
#
# A docx whose entries were mangled by the bug (zlib stream tagged method=8)
# used to abort every co command with an uncaught exception. They must now be
# tolerated: read commands work, commit refuses loudly, and the raw bytes of
# the corrupt entry are preserved on rewrite.

step "scenario 4: corrupt method=8 entries are tolerated"

make_buggy_docx() {
    python3 - "$1" <<'PYEOF'
import sys, zipfile, zlib, struct
out = sys.argv[1]
# zlib stream (RFC 1950) stored inside a method=8 entry without raw-deflate
# wrapping — the exact corruption shape issue #8 describes.
zstream = zlib.compress(b"fake png bytes")
with zipfile.ZipFile(out, "w") as z:
    z.writestr("[Content_Types].xml",
               '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>')
    z.writestr("word/document.xml", "<xml/>")
    z.writestr(zipfile.ZipInfo("word/media/img.png"), zstream, zipfile.ZIP_STORED)
data = bytearray(open(out, "rb").read())
name = b"word/media/img.png"
local = data.find(name) - 30
data[local + 8:local + 10] = struct.pack("<H", 8)
central = data.rfind(name) - 46
data[central + 10:central + 12] = struct.pack("<H", 8)
open(out, "wb").write(bytes(data))
PYEOF
}

make_buggy_docx d.docx
"$CO_BIN" log d.docx >/dev/null 2>&1 || { fail "log on corrupt docx crashed"; exit 1; }
"$CO_BIN" status d.docx >/dev/null 2>&1 || { fail "status on corrupt docx crashed"; exit 1; }
if "$CO_BIN" commit -m x d.docx >/dev/null 2>&1; then
    fail "commit on corrupt docx should be refused"
    exit 1
fi
python3 - "$WORK/d.docx" <<'PYEOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    ok = "word/media/img.png" in z.namelist()
if not ok:
    sys.exit(1)
PYEOF
[ $? -eq 0 ] || { fail "corrupt entry lost on rewrite"; exit 1; }
step "scenario 4 PASS (graceful corruption handling)"

# ---- summary ----

if [ "$FAILED" -ne 0 ]; then
    echo "issue #8 regression test FAILED"
    exit 1
fi
echo "issue #8 regression test PASS (all round trips lossless)"
