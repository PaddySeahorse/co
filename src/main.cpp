// SPDX-FileCopyrightText: 2026 Xu Qiyuan <paddyseahorse@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "zip.hpp"
#include "objectstore.hpp"
#include "commit.hpp"
#include "gc.hpp"
#include "bundle.hpp"
#include "status.hpp"
#include "merge.hpp"
#include "migrate.hpp"
#include "lock.hpp"
#include "diff_content.hpp"
#include "refs.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <ctime>
#include <sys/stat.h>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <utility>

#ifndef VERSION
#define VERSION "dev"
#endif

using namespace co;

static void fatal(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    exit(1);
}

// ============ 用法 ============

static void printUsage(FILE* out) {
    fprintf(out, "Usage: co <command> [args]\n");
    fprintf(out, "       co [--help] [--version]\n");
    fprintf(out, "Note: paths with spaces must be quoted or escaped.\n");
    fprintf(out, "Commands:\n");
    fprintf(out, "  init <path>                  Initialize .co metadata inside the Office file\n");
    fprintf(out, "  commit -m <msg> <path>       Create a commit of the Office file contents\n");
    fprintf(out, "  log <path>                   Show commit history stored inside the Office file\n");
    fprintf(out, "  status <path>                Show version control status of the Office file\n");
    fprintf(out, "  diff <a> <b> <path>          Compare two commits (refs: HEAD, HEAD~N, hash, prefix)\n");
    fprintf(out, "  checkout <commit> <path>     Restore the Office file contents from a specific commit\n");
    fprintf(out, "  gc <path>                    Pack objects and prune unreachable ones to reduce file size\n");
    fprintf(out, "  export <path>                Extract .co/ history into a standalone .co-bundle\n");
    fprintf(out, "  import <path> <bundle>       Inject a .co-bundle's history back into an Office file\n");
    fprintf(out, "  verify-bundle <bundle>       Verify the integrity of a .co-bundle\n");
    fprintf(out, "  bundle-merge <a> <b>         Three-way merge two .co-bundle files into one\n");
    fprintf(out, "  migrate <path>               Convert the repository's hash algorithm (sha1<->sha256)\n");
    fprintf(out, "  branch <path>                List branches (with '* ' marking the current one)\n");
    fprintf(out, "  branch <name> <path>         Create a branch pointing at the current HEAD\n");
    fprintf(out, "  branch -d <name> <path>      Delete a branch\n");
    fprintf(out, "  switch <branch> <path>       Switch to a branch (alias for checkout <branch>)\n");
    fprintf(out, "Common flag: --external <bundle>  Use an external .co-bundle as history store\n");
    fprintf(out, "Run 'co <command> --help' for command-specific help.\n");
}

static void printInitUsage(FILE* out) {
    fprintf(out, "Usage: co init [--help] <path>\n");
    fprintf(out, "Initialize .co metadata inside an Office file.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co init \"My File.docx\"\n");
}

static void printCommitUsage(FILE* out) {
    fprintf(out, "Usage: co commit -m <message> [--help] [--external <bundle>] <path>\n");
    fprintf(out, "Create a commit of the Office file contents.\n");
    fprintf(out, "With --external, history is read from / written to the .co-bundle instead of the Office file.\n");
    fprintf(out, "Examples:\n");
    fprintf(out, "  co commit -m \"Draft edits\" ./report.docx\n");
    fprintf(out, "  co commit -m \"Edit\" ./report.docx --external ./report.docx.co-bundle\n");
}

static void printLogUsage(FILE* out) {
    fprintf(out, "Usage: co log [--help] [--external <bundle>] <path>\n");
    fprintf(out, "Show commit history. By default reads from .co/ inside the Office file.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co log ./report.docx --external ./report.docx.co-bundle\n");
}

static void printStatusUsage(FILE* out) {
    fprintf(out, "Usage: co status [--help] [--external <bundle>] <path>\n");
    fprintf(out, "Show version control status of the Office file.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co status ./report.docx\n");
}

static void printDiffUsage(FILE* out) {
    fprintf(out, "Usage: co diff <ref-a> <ref-b> [--help] [--status] [--external <bundle>] <path>\n");
    fprintf(out, "Show content differences of changed entries between two commits.\n");
    fprintf(out, "  --status              File-level change list only (A/D/M), no content diff\n");
    fprintf(out, "Refs: HEAD, HEAD~N, full hash, or hash prefix.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co diff HEAD~1 HEAD ./report.docx\n");
}

static void printGCUsage(FILE* out) {
    fprintf(out, "Usage: co gc [--help] [--external <bundle>] <path>\n");
    fprintf(out, "Pack objects and prune unreachable ones to reduce file size.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co gc ./report.docx\n");
}

static void printCheckoutUsage(FILE* out) {
    fprintf(out, "Usage: co checkout <commit> [--help] [--external <bundle>] <path>\n");
    fprintf(out, "Restore the Office file contents from a specific commit.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co checkout a1b2c3d4 ./report.docx\n");
}

static void printExportUsage(FILE* out) {
    fprintf(out, "Usage: co export [--help] [--output <bundle>] [--clean [--clean-output <path>]] [--redact] <path>\n");
    fprintf(out, "Extract the .co/ history from an Office file into a standalone .co-bundle.\n");
    fprintf(out, "The original Office file is never modified.\n");
    fprintf(out, "  --output <bundle>    Write the bundle to this path (default: <path>.co-bundle)\n");
    fprintf(out, "  --clean              Also write a copy of the Office file with .co/ stripped\n");
    fprintf(out, "  --clean-output <p>   Path for the clean copy (default: <base>.clean<ext>)\n");
    fprintf(out, "  --redact             Omit blob contents from the bundle (keep commits/trees only)\n");
    fprintf(out, "Examples:\n");
    fprintf(out, "  co export report.docx\n");
    fprintf(out, "  co export report.docx --clean\n");
}

static void printImportUsage(FILE* out) {
    fprintf(out, "Usage: co import [--help] [--verify] [--force] <path> <bundle>\n");
    fprintf(out, "Inject a .co-bundle's history back into an Office file.\n");
    fprintf(out, "  --verify   Only check that the bundle's source SHA256 matches the file; do not write\n");
    fprintf(out, "  --force    Import even when the source SHA256 does not match\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co import report.docx report.docx.co-bundle --verify\n");
}

static void printVerifyBundleUsage(FILE* out) {
    fprintf(out, "Usage: co verify-bundle [--help] <bundle>\n");
    fprintf(out, "Verify the integrity of a .co-bundle (manifest, HEAD, objects, packs).\n");
    fprintf(out, "Exit code 0 = OK, non-zero = damaged.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co verify-bundle report.docx.co-bundle\n");
}

static void printBundleMergeUsage(FILE* out) {
    fprintf(out, "Usage: co bundle-merge [--help] -o <output> [-m <message>] <bundle-a> <bundle-b>\n");
    fprintf(out, "Three-way merge two .co-bundle files into a new bundle.\n");
    fprintf(out, "  -o, --output <bundle>   Output path for the merged bundle (required)\n");
    fprintf(out, "  -m, --message <msg>     Merge commit message\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co bundle-merge alice.docx.co-bundle bob.docx.co-bundle -o merged.co-bundle\n");
}

static void printMigrateUsage(FILE* out) {
    fprintf(out, "Usage: co migrate [--help] <path>\n");
    fprintf(out, "Convert the repository's hash algorithm between SHA1 and SHA256.\n");
    fprintf(out, "The current binary's algorithm is the source; the other is the target.\n");
    fprintf(out, "After migration the repository is incompatible with this binary — rebuild\n");
    fprintf(out, "with the matching -DCO_HASH=<target> to keep using it.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co migrate ./report.docx\n");
}

static void printBranchUsage(FILE* out) {
    fprintf(out, "Usage: co branch [--help] [--external <bundle>] [<name>] <path>\n");
    fprintf(out, "       co branch -d <name> [--help] [--external <bundle>] <path>\n");
    fprintf(out, "List, create, or delete branches.\n");
    fprintf(out, "  (no <name>)      List all branches; '* ' marks the current branch\n");
    fprintf(out, "  <name>           Create a branch pointing at the current HEAD\n");
    fprintf(out, "  -d <name>        Delete a branch\n");
    fprintf(out, "Examples:\n");
    fprintf(out, "  co branch ./report.docx\n");
    fprintf(out, "  co branch feature-x ./report.docx\n");
    fprintf(out, "  co branch -d feature-x ./report.docx\n");
}

static void printSwitchUsage(FILE* out) {
    fprintf(out, "Usage: co switch <branch> [--help] [--external <bundle>] <path>\n");
    fprintf(out, "Switch to a branch (alias for 'co checkout <branch>').\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co switch feature-x ./report.docx\n");
}

static void commandUsage(const std::string& command, FILE* out) {
    if (command == "init") printInitUsage(out);
    else if (command == "commit") printCommitUsage(out);
    else if (command == "log") printLogUsage(out);
    else if (command == "status") printStatusUsage(out);
    else if (command == "diff") printDiffUsage(out);
    else if (command == "gc") printGCUsage(out);
    else if (command == "checkout") printCheckoutUsage(out);
    else if (command == "export") printExportUsage(out);
    else if (command == "import") printImportUsage(out);
    else if (command == "verify-bundle") printVerifyBundleUsage(out);
    else if (command == "bundle-merge") printBundleMergeUsage(out);
    else if (command == "migrate") printMigrateUsage(out);
    else if (command == "branch") printBranchUsage(out);
    else if (command == "switch") printSwitchUsage(out);
}

static void printCommandUsage(const std::string& cmd) {
    if (cmd == "init") { printInitUsage(stdout); return; }
    if (cmd == "commit") { printCommitUsage(stdout); return; }
    if (cmd == "log") { printLogUsage(stdout); return; }
    if (cmd == "status") { printStatusUsage(stdout); return; }
    if (cmd == "diff") { printDiffUsage(stdout); return; }
    if (cmd == "gc") { printGCUsage(stdout); return; }
    if (cmd == "checkout") { printCheckoutUsage(stdout); return; }
    if (cmd == "export") { printExportUsage(stdout); return; }
    if (cmd == "import") { printImportUsage(stdout); return; }
    if (cmd == "verify-bundle") { printVerifyBundleUsage(stdout); return; }
    if (cmd == "bundle-merge") { printBundleMergeUsage(stdout); return; }
    if (cmd == "migrate") { printMigrateUsage(stdout); return; }
    if (cmd == "branch") { printBranchUsage(stdout); return; }
    if (cmd == "switch") { printSwitchUsage(stdout); return; }
    fprintf(stderr, "Unknown command: %s\n\n", cmd.c_str());
    printUsage(stderr);
    exit(1);
}

// ============ 路径/文件工具 ============

// 模拟 Go filepath.Ext：只取最后一个路径元素中最后一个 '.' 起的后缀
static std::string fileExtension(const std::string& path) {
    size_t dot = std::string::npos;
    for (size_t i = path.size(); i > 0; --i) {
        char c = path[i - 1];
        if (c == '/' || c == '\\') break;
        if (c == '.') dot = i - 1;
    }
    if (dot == std::string::npos) return "";
    return path.substr(dot);
}

// 文件名（含扩展）→ 去 ext 的主体
static std::string fileBase(const std::string& path) {
    std::string ext = fileExtension(path);
    if (ext.empty()) return path;
    return path.substr(0, path.size() - ext.size());
}

static void requireOfficeFile(const std::string& path) {
    std::string ext = fileExtension(path);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    if (ext != ".docx" && ext != ".xlsx" && ext != ".pptx") {
        fatal("unsupported file extension \"" + ext + "\" (supported: .docx, .xlsx, .pptx)");
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            fatal("file not found: " + path + " (if the path contains spaces, wrap it in quotes or escape the spaces)");
        }
        fatal(std::string("stat: ") + strerror(errno));
    }
}

static void requireExistingFile(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            fatal("file not found: " + path + " (if the path contains spaces, wrap it in quotes or escape the spaces)");
        }
        fatal(std::string("stat: ") + strerror(errno));
    }
}

// 校验：仓库实际哈希算法必须与当前二进制构建的算法一致；不一致则 fatal。
// 空 history 或无法识别时不阻塞（init 和 migrate 显式不调用此函数）。
static void requireHashCompat(const Document& doc) {
    std::string repo = repoHashAlgoName(doc);
    if (repo.empty()) return;
    std::string builtin = currentHashAlgoName();
    if (repo != builtin) {
        fatal("repository hash algorithm is \"" + repo +
              "\" but this binary was built for \"" + builtin +
              "\". Rebuild with -DCO_HASH=" + repo +
              " or run `co migrate` after rebuilding to switch the repository.");
    }
}

// ============ flag 解析（Go 风格：flag 必须在 positional 之前） ============

struct FlagSpec {
    bool allowMessage = false;       // -m / --m / -m= / --m=
    bool allowExternal = false;      // --external <path>
    bool allowOutput = false;        // --output / -o <path>
    bool allowClean = false;         // --clean (bool)
    bool allowCleanOutput = false;   // --clean-output <path>
    bool allowRedact = false;        // --redact (bool)
    bool allowVerify = false;        // --verify (bool)
    bool allowForce = false;         // --force (bool)
    bool allowStatus = false;        // --status (bool, diff 专用)
    bool allowDelete = false;        // -d / --delete (bool, branch 专用)
};

struct ParsedArgs {
    bool showHelp = false;
    std::string message;
    std::string external;
    std::string output;
    std::string cleanOutput;
    bool clean = false;
    bool redact = false;
    bool verify = false;
    bool force = false;
    bool status = false;
    bool del = false;
    std::vector<std::string> positional;
};

static const char* valueFlagName(const std::string& arg, const FlagSpec& spec) {
    if (spec.allowMessage && (arg == "-m" || arg == "--m")) return "m";
    if (spec.allowExternal && arg == "--external") return "external";
    if (spec.allowOutput && (arg == "--output" || arg == "-o")) return "output";
    if (spec.allowCleanOutput && arg == "--clean-output") return "clean-output";
    return nullptr;
}

static bool boolFlag(const std::string& arg, const FlagSpec& spec) {
    if (spec.allowClean && arg == "--clean") return true;
    if (spec.allowRedact && arg == "--redact") return true;
    if (spec.allowVerify && arg == "--verify") return true;
    if (spec.allowForce && arg == "--force") return true;
    if (spec.allowStatus && arg == "--status") return true;
    if (spec.allowDelete && (arg == "-d" || arg == "--delete")) return true;
    return false;
}

// 处理 -m=value / --m=value / --output=value 等带等号形式
static bool tryEqualForm(const std::string& arg, const FlagSpec& spec, ParsedArgs& pa) {
    size_t eq = arg.find('=');
    if (eq == std::string::npos) return false;
    std::string key = arg.substr(0, eq);
    std::string val = arg.substr(eq + 1);
    if (spec.allowMessage && (key == "-m" || key == "--m")) { pa.message = val; return true; }
    if (spec.allowExternal && key == "--external") { pa.external = val; return true; }
    if (spec.allowOutput && (key == "--output" || key == "-o")) { pa.output = val; return true; }
    if (spec.allowCleanOutput && key == "--clean-output") { pa.cleanOutput = val; return true; }
    return false;
}

static ParsedArgs parseArgs(const std::string& command, const FlagSpec& spec,
                            const std::vector<std::string>& args) {
    // git 风格：flag 与 positional 可交错出现（改造清单示例将 --external 放在 path 之后）。
    // 遇到 "--" 后，剩余全部视为 positional。以 '-' 开头的参数（除 "--" 外）按 flag 处理。
    ParsedArgs pa;
    bool noMoreFlags = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (!noMoreFlags && arg == "--") { noMoreFlags = true; continue; }
        if (!noMoreFlags && arg == "-h") { pa.showHelp = true; continue; }
        if (!noMoreFlags && arg == "--help") { pa.showHelp = true; continue; }
        if (!noMoreFlags && arg.size() > 1 && arg[0] == '-' && tryEqualForm(arg, spec, pa)) {
            continue;
        }
        if (!noMoreFlags && arg.size() > 1 && arg[0] == '-') {
            const char* vname = valueFlagName(arg, spec);
            if (vname) {
                if (i + 1 >= args.size()) {
                    fprintf(stderr, "flag needs an argument: %s\n", arg.c_str());
                    commandUsage(command, stderr);
                    exit(2);
                }
                std::string v = args[i + 1];
                std::string n = vname;
                if (n == "m") pa.message = v;
                else if (n == "external") pa.external = v;
                else if (n == "output") pa.output = v;
                else if (n == "clean-output") pa.cleanOutput = v;
                ++i;
                continue;
            }
            if (boolFlag(arg, spec)) {
                if (arg == "--clean") pa.clean = true;
                else if (arg == "--redact") pa.redact = true;
                else if (arg == "--verify") pa.verify = true;
                else if (arg == "--force") pa.force = true;
                else if (arg == "--status") pa.status = true;
                else if (arg == "-d" || arg == "--delete") pa.del = true;
                continue;
            }
            fprintf(stderr, "flag provided but not defined: %s\n", arg.c_str());
            commandUsage(command, stderr);
            exit(2);
        }
        pa.positional.push_back(arg);
    }
    return pa;
}

// ============ 锁辅助 ============

// 获取 targetPath 的排他锁（5s 超时）。失败 fatal。
static FileLock acquireLockOrDie(const std::string& targetPath) {
    FileLock lock;
    if (!lock.acquire(targetPath)) {
        fatal("could not acquire lock on " + targetPath + " (another co process may be holding it)");
    }
    return lock;
}

// ============ bundle 元数据刷新（外部模式写回时调用） ============

// 在写回 bundle 前，根据当前内容更新 manifest 的 commit_count 与 bundle_size_bytes。
// 无 manifest 则跳过（保持原状）。
static void refreshBundleManifest(Document& bundleDoc, int64_t reportedSize) {
    std::vector<uint8_t> md;
    if (!bundleDoc.get(kManifestPath, md)) return;
    std::string ms(md.begin(), md.end());
    Manifest m;
    if (!parseManifest(ms, m)) return;
    StoreStats stats = computeStoreStats(bundleDoc);
    m.commitCount = stats.commits;
    m.bundleSizeBytes = reportedSize;
    std::string msj = serializeManifest(m);
    std::vector<uint8_t> nmd(msj.begin(), msj.end());
    bundleDoc.set(kManifestPath, nmd);
}

// 写 bundle 并刷新 manifest 的 size 字段（一次迭代：先按 reportedSize 写，
// 再以实际大小重写 manifest，再写一次。两次足以让 size 字段反映最终大小）。
static bool writeBundleWithManifest(Document& bundleDoc, const std::string& bundlePath) {
    int64_t reported = 0;
    for (int iter = 0; iter < 3; ++iter) {
        refreshBundleManifest(bundleDoc, reported);
        if (!bundleDoc.write(bundlePath)) return false;
        int64_t sz = fileSizeOf(bundlePath);
        if (sz < 0) return false;
        if (sz == reported) break;
        reported = sz;
    }
    return true;
}

// ============ 现有命令：init / commit / log / gc / checkout ============

static void handleInit(const std::vector<std::string>& args) {
    FlagSpec spec;
    ParsedArgs pa = parseArgs("init", spec, args);
    if (pa.showHelp) { printInitUsage(stdout); return; }
    if (pa.positional.empty()) fatal("Office file path is required.");
    if (pa.positional.size() > 1) fatal("unexpected arguments after path");
    std::string path = pa.positional[0];
    requireOfficeFile(path);

    FileLock lock = acquireLockOrDie(path);
    auto doc = Document::load(path);
    if (!doc) fatal("failed to load " + path);
    Store store(*doc);
    if (store.head() == "") store.setHead("");
    if (!doc->write(path)) fatal("failed to write " + path);
    printf("Initialized .co metadata inside %s\n", path.c_str());
}

static void handleCommit(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowMessage = true;
    spec.allowExternal = true;
    ParsedArgs pa = parseArgs("commit", spec, args);
    if (pa.showHelp) { printCommitUsage(stdout); return; }
    if (pa.positional.empty()) fatal("Office file path is required.");
    if (pa.positional.size() > 1) fatal("unexpected arguments after path");
    std::string path = pa.positional[0];
    requireOfficeFile(path);
    if (pa.message.empty()) fatal("commit message required (-m)");

    int64_t nowUnix = (int64_t)time(nullptr);

    if (pa.external.empty()) {
        // 内嵌模式
        FileLock lock = acquireLockOrDie(path);
        auto doc = Document::load(path);
        if (!doc) fatal("failed to load " + path);
        requireHashCompat(*doc);
        std::string hash = createCommit(*doc, pa.message, nowUnix);
        if (!doc->write(path)) fatal("failed to write " + path);
        printf("Committed %s\n", hash.c_str());
    } else {
        // 外部模式：historyDoc = bundle，contentDoc = office 文件
        requireExistingFile(pa.external);
        FileLock lock = acquireLockOrDie(pa.external);
        auto historyDoc = Document::load(pa.external);
        if (!historyDoc) fatal("failed to load bundle: " + pa.external);
        requireHashCompat(*historyDoc);
        auto contentDoc = Document::load(path);
        if (!contentDoc) fatal("failed to load " + path);
        std::string hash = createCommitExternal(*historyDoc, *contentDoc, pa.message, nowUnix);
        if (!writeBundleWithManifest(*historyDoc, pa.external)) {
            fatal("failed to write bundle: " + pa.external);
        }
        printf("Committed %s (external: %s)\n", hash.c_str(), pa.external.c_str());
    }
}

static void handleLog(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowExternal = true;
    ParsedArgs pa = parseArgs("log", spec, args);
    if (pa.showHelp) { printLogUsage(stdout); return; }
    if (pa.positional.empty()) fatal("Office file path is required.");
    if (pa.positional.size() > 1) fatal("unexpected arguments after path");
    std::string path = pa.positional[0];
    requireOfficeFile(path);

    std::unique_ptr<Document> doc;
    if (pa.external.empty()) {
        doc = Document::load(path);
        if (!doc) fatal("failed to load " + path);
        requireHashCompat(*doc);
    } else {
        requireExistingFile(pa.external);
        doc = Document::load(pa.external);
        if (!doc) fatal("failed to load bundle: " + pa.external);
        requireHashCompat(*doc);
    }
    std::vector<Commit> commits = logCommits(*doc);
    for (const auto& entry : commits) {
        printf("commit %s\n", entry.hash.c_str());
        if (!entry.authorName.empty()) {
            printf("Author: %s <%s>\n", entry.authorName.c_str(), entry.authorEmail.c_str());
        }
        if (entry.timestamp != 0) {
            printf("Date:   %s\n", formatTimestampRFC1123Z(entry.timestamp).c_str());
        }
        if (!entry.message.empty()) {
            printf("\n    %s\n", entry.message.c_str());
        }
        printf("\n");
    }
}

static void handleGC(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowExternal = true;
    ParsedArgs pa = parseArgs("gc", spec, args);
    if (pa.showHelp) { printGCUsage(stdout); return; }
    if (pa.positional.empty()) fatal("Office file path is required.");
    if (pa.positional.size() > 1) fatal("unexpected arguments after path");
    std::string path = pa.positional[0];
    requireOfficeFile(path);

    if (pa.external.empty()) {
        FileLock lock = acquireLockOrDie(path);
        auto doc = Document::load(path);
        if (!doc) fatal("failed to load " + path);
        requireHashCompat(*doc);
        GCStats stats;
        if (!garbageCollect(*doc, stats)) fatal("garbage collection failed");
        if (!doc->write(path)) fatal("failed to write " + path);
        printf("Garbage collection completed:\n");
        printf("  Reachable objects: %d\n", stats.reachableObjects);
        printf("  Packed objects: %d\n", stats.packedObjects);
        printf("  Removed loose objects: %d\n", stats.removedLoose);
        printf("  Removed old packs: %d\n", stats.removedPacks);
    } else {
        requireExistingFile(pa.external);
        FileLock lock = acquireLockOrDie(pa.external);
        auto historyDoc = Document::load(pa.external);
        if (!historyDoc) fatal("failed to load bundle: " + pa.external);
        requireHashCompat(*historyDoc);
        GCStats stats;
        if (!garbageCollect(*historyDoc, stats)) fatal("garbage collection failed");
        if (!writeBundleWithManifest(*historyDoc, pa.external)) {
            fatal("failed to write bundle: " + pa.external);
        }
        printf("Garbage collection completed (external: %s):\n", pa.external.c_str());
        printf("  Reachable objects: %d\n", stats.reachableObjects);
        printf("  Packed objects: %d\n", stats.packedObjects);
        printf("  Removed loose objects: %d\n", stats.removedLoose);
        printf("  Removed old packs: %d\n", stats.removedPacks);
    }
}

static void handleCheckout(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowExternal = true;
    ParsedArgs pa = parseArgs("checkout", spec, args);
    if (pa.showHelp) { printCheckoutUsage(stdout); return; }
    if (pa.positional.size() < 2) fatal("commit hash and Office file path are required.");
    if (pa.positional.size() > 2) fatal("unexpected arguments after path");
    const std::string& commitRef = pa.positional[0];
    const std::string& path = pa.positional[1];
    requireOfficeFile(path);

    // 分支名优先（与 resolveRef 一致）；非分支则视为分离 checkout
    if (pa.external.empty()) {
        FileLock lock = acquireLockOrDie(path);
        auto doc = Document::load(path);
        if (!doc) fatal("failed to load " + path);
        requireHashCompat(*doc);
        bool isBranch = isValidBranchName(commitRef) && !getBranchHash(*doc, commitRef).empty();
        std::string commitHash = resolveRef(*doc, commitRef);
        if (commitHash.empty()) fatal("checkout failed: " + commitRef);
        if (!checkoutCommit(*doc, commitHash)) fatal("checkout failed: " + commitHash);
        if (isBranch) {
            if (!attachHead(*doc, commitRef)) fatal("checkout failed: cannot attach HEAD to " + commitRef);
            commitHash = commitRef;
        } else {
            detachHead(*doc, commitHash);
        }
        if (!doc->write(path)) fatal("failed to write " + path);
        printf("Checked out %s\n", commitHash.c_str());
    } else {
        requireExistingFile(pa.external);
        FileLock histLock = acquireLockOrDie(pa.external);
        FileLock contentLock = acquireLockOrDie(path);
        auto historyDoc = Document::load(pa.external);
        if (!historyDoc) fatal("failed to load bundle: " + pa.external);
        requireHashCompat(*historyDoc);
        auto contentDoc = Document::load(path);
        if (!contentDoc) fatal("failed to load " + path);
        bool isBranch = isValidBranchName(commitRef) && !getBranchHash(*historyDoc, commitRef).empty();
        std::string commitHash = resolveRef(*historyDoc, commitRef);
        if (commitHash.empty()) fatal("checkout failed: " + commitRef);
        if (!checkoutCommitExternal(*historyDoc, *contentDoc, commitHash)) {
            fatal("checkout failed: " + commitHash);
        }
        if (isBranch) {
            if (!attachHead(*historyDoc, commitRef)) fatal("checkout failed: cannot attach HEAD to " + commitRef);
            commitHash = commitRef;
        } else {
            detachHead(*historyDoc, commitHash);
        }
        if (!contentDoc->write(path)) fatal("failed to write " + path);
        if (!writeBundleWithManifest(*historyDoc, pa.external)) {
            fatal("failed to write bundle: " + pa.external);
        }
        printf("Checked out %s (external: %s)\n", commitHash.c_str(), pa.external.c_str());
    }
}

// ============ 新命令：branch / switch ============

static void handleBranch(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowExternal = true;
    spec.allowDelete = true;
    ParsedArgs pa = parseArgs("branch", spec, args);
    if (pa.showHelp) { printBranchUsage(stdout); return; }

    // 定位 path：branch 的 positional 最后一个是 Office 文件路径
    if (pa.positional.empty()) fatal("Office file path is required.");
    std::string path = pa.positional.back();
    requireOfficeFile(path);

    std::unique_ptr<Document> doc;
    if (pa.external.empty()) {
        doc = Document::load(path);
        if (!doc) fatal("failed to load " + path);
        requireHashCompat(*doc);
    } else {
        requireExistingFile(pa.external);
        doc = Document::load(pa.external);
        if (!doc) fatal("failed to load bundle: " + pa.external);
        requireHashCompat(*doc);
    }

    if (pa.del) {
        if (pa.positional.size() != 2) fatal("usage: co branch -d <name> <path>");
        const std::string& name = pa.positional[0];
        if (!isValidBranchName(name)) fatal("invalid branch name: " + name);
        if (name == currentBranch(*doc)) fatal("cannot delete the current branch: " + name);
        if (!deleteBranch(*doc, name)) fatal("branch not found: " + name);
        if (pa.external.empty()) {
            if (!doc->write(path)) fatal("failed to write " + path);
        } else {
            if (!writeBundleWithManifest(*doc, pa.external)) fatal("failed to write bundle: " + pa.external);
        }
        printf("Deleted branch %s\n", name.c_str());
        return;
    }

    if (pa.positional.size() == 1) {
        // 列出分支
        std::string cur = currentBranch(*doc);
        std::vector<std::string> branches = listBranches(*doc);
        if (branches.empty() && cur.empty()) {
            printf("(no branches)\n");
            return;
        }
        for (const auto& b : branches) {
            if (b == cur) printf("* %s\n", b.c_str());
            else printf("  %s\n", b.c_str());
        }
        if (cur.empty() && !branches.empty()) {
            printf("  (detached HEAD)\n");
        }
        return;
    }

    if (pa.positional.size() == 2) {
        // 创建分支
        const std::string& name = pa.positional[0];
        if (!isValidBranchName(name)) fatal("invalid branch name: " + name);
        if (!getBranchHash(*doc, name).empty()) fatal("branch already exists: " + name);
        std::string head = headCommitHash(*doc);
        if (head.empty()) fatal("cannot create branch: repository has no commits");
        if (!setBranch(*doc, name, head)) fatal("failed to create branch: " + name);
        if (pa.external.empty()) {
            if (!doc->write(path)) fatal("failed to write " + path);
        } else {
            if (!writeBundleWithManifest(*doc, pa.external)) fatal("failed to write bundle: " + pa.external);
        }
        printf("Created branch %s\n", name.c_str());
        return;
    }

    fatal("unexpected arguments for branch");
}

static void handleSwitch(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowExternal = true;
    ParsedArgs pa = parseArgs("switch", spec, args);
    if (pa.showHelp) { printSwitchUsage(stdout); return; }
    if (pa.positional.size() != 2) fatal("usage: co switch <branch> <path>");
    handleCheckout(args);
}

// ============ 新命令：status / diff ============

static void handleStatus(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowExternal = true;
    ParsedArgs pa = parseArgs("status", spec, args);
    if (pa.showHelp) { printStatusUsage(stdout); return; }
    if (pa.positional.empty()) fatal("Office file path is required.");
    if (pa.positional.size() > 1) fatal("unexpected arguments after path");
    std::string path = pa.positional[0];
    requireOfficeFile(path);

    StatusInfo info;
    if (pa.external.empty()) {
        auto doc = Document::load(path);
        if (!doc) fatal("failed to load " + path);
        requireHashCompat(*doc);
        info = computeStatus(*doc, *doc, path);
    } else {
        requireExistingFile(pa.external);
        auto historyDoc = Document::load(pa.external);
        if (!historyDoc) fatal("failed to load bundle: " + pa.external);
        requireHashCompat(*historyDoc);
        auto contentDoc = Document::load(path);
        if (!contentDoc) fatal("failed to load " + path);
        info = computeStatus(*historyDoc, *contentDoc, path);
    }

    if (!info.hasHistory) {
        printf("No commits yet (file size: %lld bytes)\n", (long long)info.fileSize);
        return;
    }
    if (!info.branch.empty()) {
        printf("On branch: %s\n", info.branch.c_str());
    } else {
        printf("Detached HEAD (no current branch)\n");
    }
    printf("Commit: %s (%s)\n", info.headShort.c_str(), info.headMessage.c_str());
    printf("Changes since last commit: %d file(s)\n", info.changedFiles);
    printf("Bundle size: %lld bytes (%lld commits, %lld objects)\n",
           (long long)info.fileSize, (long long)info.commitCount, (long long)info.objectCount);
    printf("Hash algorithm: %s\n", info.hashAlgo.c_str());
}

static void handleDiff(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowExternal = true;
    spec.allowStatus = true;
    ParsedArgs pa = parseArgs("diff", spec, args);
    if (pa.showHelp) { printDiffUsage(stdout); return; }
    if (pa.positional.size() < 3) fatal("two refs and Office file path are required.");
    if (pa.positional.size() > 3) fatal("unexpected arguments after path");
    const std::string& refA = pa.positional[0];
    const std::string& refB = pa.positional[1];
    const std::string& path = pa.positional[2];
    requireOfficeFile(path);

    std::unique_ptr<Document> doc;
    if (pa.external.empty()) {
        doc = Document::load(path);
        if (!doc) fatal("failed to load " + path);
        requireHashCompat(*doc);
    } else {
        requireExistingFile(pa.external);
        doc = Document::load(pa.external);
        if (!doc) fatal("failed to load bundle: " + pa.external);
        requireHashCompat(*doc);
    }

    if (pa.status) {
        std::vector<DiffEntry> entries = diffCommits(*doc, refA, refB);
        if (entries.empty()) {
            printf("No changes between %s and %s\n", refA.c_str(), refB.c_str());
            return;
        }
        for (const auto& e : entries) {
            printf("%c %s\n", e.status, e.path.c_str());
        }
        return;
    }

    std::string out;
    renderCommitDiff(*doc, refA, refB, out);
    if (out.empty()) {
        printf("No changes between %s and %s\n", refA.c_str(), refB.c_str());
        return;
    }
    fwrite(out.data(), 1, out.size(), stdout);
}

// ============ 新命令：export / import / verify-bundle ============

static void handleExport(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowOutput = true;
    spec.allowClean = true;
    spec.allowCleanOutput = true;
    spec.allowRedact = true;
    ParsedArgs pa = parseArgs("export", spec, args);
    if (pa.showHelp) { printExportUsage(stdout); return; }
    if (pa.positional.empty()) fatal("Office file path is required.");
    if (pa.positional.size() > 1) fatal("unexpected arguments after path");
    std::string path = pa.positional[0];
    requireOfficeFile(path);

    ExportOptions opts;
    opts.outputPath = pa.output.empty() ? (path + kBundleExt) : pa.output;
    opts.clean = pa.clean;
    opts.redact = pa.redact;
    if (opts.clean) {
        if (pa.cleanOutput.empty()) {
            // 默认干净副本路径：<base>.clean<ext>
            opts.cleanOutputPath = fileBase(path) + ".clean" + fileExtension(path);
        } else {
            opts.cleanOutputPath = pa.cleanOutput;
        }
    }

    std::string error;
    if (!exportBundle(path, opts, error)) fatal(error);
    printf("Exported history to %s\n", opts.outputPath.c_str());
    if (opts.redact) printf("  (redacted: blob contents omitted)\n");
    if (opts.clean) {
        printf("  Clean copy: %s\n", opts.cleanOutputPath.c_str());
        printf("  Original left untouched: %s\n", path.c_str());
    } else {
        printf("  Original left untouched: %s\n", path.c_str());
    }
}

static void handleImport(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowVerify = true;
    spec.allowForce = true;
    ParsedArgs pa = parseArgs("import", spec, args);
    if (pa.showHelp) { printImportUsage(stdout); return; }
    if (pa.positional.size() < 2) fatal("Office file path and bundle path are required.");
    if (pa.positional.size() > 2) fatal("unexpected arguments after path");
    const std::string& path = pa.positional[0];
    const std::string& bundle = pa.positional[1];
    requireOfficeFile(path);
    requireExistingFile(bundle);

    FileLock lock = acquireLockOrDie(path);
    ImportOutcome r = importBundle(path, bundle, pa.force, pa.verify);
    if (!r.ok) fatal(r.error.empty() ? std::string("import failed") : r.error);

    if (pa.verify) {
        printf("Verify: source SHA256 %s\n", r.hashMatched ? "matches current file" : "does NOT match current file");
        if (!r.hashMatched) {
            printf("  manifest: %s\n", r.manifestSourceSha256.c_str());
            printf("  current:  %s\n", r.currentSha256.c_str());
        }
        if (!r.sourceFilename.empty()) printf("  source filename: %s\n", r.sourceFilename.c_str());
        return;
    }

    printf("Imported history from %s into %s\n", bundle.c_str(), path.c_str());
    if (!r.hashMatched) {
        printf("  WARNING: source SHA256 did not match (file may have been modified since export)\n");
    }
}

static void handleVerifyBundle(const std::vector<std::string>& args) {
    FlagSpec spec;
    ParsedArgs pa = parseArgs("verify-bundle", spec, args);
    if (pa.showHelp) { printVerifyBundleUsage(stdout); return; }
    if (pa.positional.empty()) fatal("bundle path is required.");
    if (pa.positional.size() > 1) fatal("unexpected arguments after path");
    const std::string& bundle = pa.positional[0];
    requireExistingFile(bundle);

    VerifyReport rep = verifyBundle(bundle);
    printf("Bundle: %s\n", bundle.c_str());
    printf("  manifest valid:  %s\n", rep.manifestValid ? "yes" : "no");
    if (rep.manifestValid) {
        printf("  manifest version: %s\n", rep.manifestVersion.c_str());
        printf("  hash algorithm:   %s\n", rep.hashAlgo.c_str());
    }
    printf("  HEAD valid:       %s\n", rep.headValid ? "yes" : "no");
    printf("  objects readable: %s\n", rep.allObjectsReadable ? "yes" : "no");
    printf("  packs intact:     %s\n", rep.packsIntact ? "yes" : "no");
    printf("  commit count:     %lld\n", (long long)rep.commitCount);
    printf("  object count:     %lld\n", (long long)rep.objectCount);
    if (!rep.error.empty()) printf("  error: %s\n", rep.error.c_str());
    if (!rep.ok) {
        printf("Result: DAMAGED\n");
        exit(1);
    }
    printf("Result: OK\n");
}

// ============ 新命令：bundle-merge ============

static void handleBundleMerge(const std::vector<std::string>& args) {
    FlagSpec spec;
    spec.allowMessage = true;
    spec.allowOutput = true;
    ParsedArgs pa = parseArgs("bundle-merge", spec, args);
    if (pa.showHelp) { printBundleMergeUsage(stdout); return; }
    if (pa.output.empty()) fatal("--output <bundle> is required");
    if (pa.positional.size() < 2) fatal("two bundle paths are required.");
    if (pa.positional.size() > 2) fatal("unexpected arguments after bundles");
    const std::string& bundleA = pa.positional[0];
    const std::string& bundleB = pa.positional[1];
    requireExistingFile(bundleA);
    requireExistingFile(bundleB);

    FileLock lock = acquireLockOrDie(pa.output);
    int64_t nowUnix = (int64_t)time(nullptr);
    MergeResult r = bundleMerge(bundleA, bundleB, pa.output, pa.message, nowUnix);
    if (!r.ok) fatal(r.error.empty() ? std::string("bundle-merge failed") : r.error);
    printf("Merged %s + %s -> %s\n", bundleA.c_str(), bundleB.c_str(), r.outputPath.c_str());
    printf("  merge commit: %s\n", r.mergeCommitHash.c_str());
    if (!r.commonAncestor.empty()) {
        printf("  common ancestor: %s\n", r.commonAncestor.c_str());
    } else {
        printf("  common ancestor: (none — no shared history)\n");
    }
    if (r.hadConflicts) {
        printf("  conflicts (%zu):\n", r.conflicts.size());
        for (const auto& c : r.conflicts) printf("    %s\n", c.c_str());
    } else {
        printf("  conflicts: none\n");
    }
}

// ============ 新命令：migrate ============

static void handleMigrate(const std::vector<std::string>& args) {
    FlagSpec spec;
    ParsedArgs pa = parseArgs("migrate", spec, args);
    if (pa.showHelp) { printMigrateUsage(stdout); return; }
    if (pa.positional.empty()) fatal("Office file path is required.");
    if (pa.positional.size() > 1) fatal("unexpected arguments after path");
    std::string path = pa.positional[0];
    requireOfficeFile(path);

    FileLock lock = acquireLockOrDie(path);
    MigrateResult r = migrateStore(path);
    if (!r.ok) fatal(r.error.empty() ? std::string("migrate failed") : r.error);
    printf("Migrated %s: %s -> %s\n", path.c_str(), r.fromAlgo.c_str(), r.toAlgo.c_str());
    printf("  objects rewritten: %lld\n", (long long)r.objectsRewritten);
    printf("  trees rewritten:   %lld\n", (long long)r.treesRewritten);
    printf("  commits rewritten: %lld\n", (long long)r.commitsRewritten);
    printf("  new HEAD:          %s\n", r.newHead.c_str());
    printf("Note: rebuild this binary with -DCO_HASH=%s to keep using this repository.\n",
           r.toAlgo.c_str());
}

// ============ main ============

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(stdout);
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help") {
        printUsage(stdout);
        return 0;
    }
    if (cmd == "-v" || cmd == "--version") {
        printf("%s\n", VERSION);
        return 0;
    }
    if (cmd == "help") {
        if (argc > 2) {
            printCommandUsage(argv[2]);
            return 0;
        }
        printUsage(stdout);
        return 0;
    }
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) args.push_back(argv[i]);
    if (cmd == "init") handleInit(args);
    else if (cmd == "commit") handleCommit(args);
    else if (cmd == "log") handleLog(args);
    else if (cmd == "status") handleStatus(args);
    else if (cmd == "diff") handleDiff(args);
    else if (cmd == "gc") handleGC(args);
    else if (cmd == "checkout") handleCheckout(args);
    else if (cmd == "export") handleExport(args);
    else if (cmd == "import") handleImport(args);
    else if (cmd == "verify-bundle") handleVerifyBundle(args);
    else if (cmd == "bundle-merge") handleBundleMerge(args);
    else if (cmd == "migrate") handleMigrate(args);
    else if (cmd == "branch") handleBranch(args);
    else if (cmd == "switch") handleSwitch(args);
    else {
        printUsage(stdout);
        return 1;
    }
    return 0;
}
