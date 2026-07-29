#include "zip.hpp"
#include "objectstore.hpp"
#include "commit.hpp"
#include "gc.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
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

static void printUsage(FILE* out) {
    fprintf(out, "Usage: co <command> [args]\n");
    fprintf(out, "       co [--help] [--version]\n");
    fprintf(out, "Note: paths with spaces must be quoted or escaped.\n");
    fprintf(out, "Commands:\n");
    fprintf(out, "  init <path>                Initialize .co metadata inside the Office file\n");
    fprintf(out, "  commit -m <msg> <path>     Create a commit of the Office file contents\n");
    fprintf(out, "  log <path>                 Show commit history stored inside the Office file\n");
    fprintf(out, "  gc <path>                  Pack objects and prune unreachable ones to reduce file size\n");
    fprintf(out, "  checkout <commit> <path>   Restore the Office file contents from a specific commit\n");
    fprintf(out, "Run 'co <command> --help' for command-specific help.\n");
}

static void printInitUsage(FILE* out) {
    fprintf(out, "Usage: co init [--help] <path>\n");
    fprintf(out, "Initialize .co metadata inside an Office file.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co init \"My File.docx\"\n");
}

static void printCommitUsage(FILE* out) {
    fprintf(out, "Usage: co commit -m <message> [--help] <path>\n");
    fprintf(out, "Create a commit of the Office file contents.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co commit -m \"Draft edits\" ./report.docx\n");
}

static void printLogUsage(FILE* out) {
    fprintf(out, "Usage: co log [--help] <path>\n");
    fprintf(out, "Show commit history stored inside the Office file.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co log ./report.docx\n");
}

static void printGCUsage(FILE* out) {
    fprintf(out, "Usage: co gc [--help] <path>\n");
    fprintf(out, "Pack objects and prune unreachable ones to reduce file size.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co gc ./report.docx\n");
}

static void printCheckoutUsage(FILE* out) {
    fprintf(out, "Usage: co checkout <commit> [--help] <path>\n");
    fprintf(out, "Restore the Office file contents from a specific commit.\n");
    fprintf(out, "Example:\n");
    fprintf(out, "  co checkout a1b2c3d4 ./report.docx\n");
}

static void commandUsage(const std::string& command, FILE* out) {
    if (command == "init") printInitUsage(out);
    else if (command == "commit") printCommitUsage(out);
    else if (command == "log") printLogUsage(out);
    else if (command == "gc") printGCUsage(out);
    else if (command == "checkout") printCheckoutUsage(out);
}

static void printCommandUsage(const std::string& cmd) {
    if (cmd == "init") { printInitUsage(stdout); return; }
    if (cmd == "commit") { printCommitUsage(stdout); return; }
    if (cmd == "log") { printLogUsage(stdout); return; }
    if (cmd == "gc") { printGCUsage(stdout); return; }
    if (cmd == "checkout") { printCheckoutUsage(stdout); return; }
    fprintf(stderr, "Unknown command: %s\n\n", cmd.c_str());
    printUsage(stderr);
    exit(1);
}

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

static std::string requirePathArg(const std::vector<std::string>& args) {
    if (args.empty()) {
        fatal("Office file path is required. Quote or escape paths with spaces.");
    }
    if (args.size() > 1) {
        std::string extra = args[1];
        for (size_t i = 2; i < args.size(); ++i) {
            extra += " ";
            extra += args[i];
        }
        fatal("unexpected arguments: " + extra + " (if the path contains spaces, wrap it in quotes or escape the spaces)");
    }
    return args[0];
}

static std::pair<std::string, std::string> requireCommitAndPathArg(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        fatal("commit hash and Office file path are required. Quote or escape paths with spaces.");
    }
    if (args.size() > 2) {
        std::string extra = args[2];
        for (size_t i = 3; i < args.size(); ++i) {
            extra += " ";
            extra += args[i];
        }
        fatal("unexpected arguments: " + extra + " (if the path contains spaces, wrap it in quotes or escape the spaces)");
    }
    return {args[0], args[1]};
}

struct ParsedFlags {
    bool showHelp = false;
    std::string message;
    std::vector<std::string> positional;
};

// 模拟 Go flag 包：遇到第一个非 flag 参数即停止解析 flag
static ParsedFlags parseFlags(const std::string& command, const std::vector<std::string>& args) {
    ParsedFlags f;
    bool allowMessage = (command == "commit");
    size_t i = 0;
    while (i < args.size()) {
        const std::string& arg = args[i];
        if (arg == "--") { i++; break; }
        if (arg == "-h" || arg == "--help") { f.showHelp = true; i++; continue; }
        if (allowMessage && (arg == "-m" || arg == "--m")) {
            if (i + 1 >= args.size()) {
                fprintf(stderr, "flag needs an argument: -m\n");
                commandUsage(command, stderr);
                exit(2);
            }
            f.message = args[i + 1];
            i += 2;
            continue;
        }
        if (allowMessage && arg.rfind("-m=", 0) == 0) { f.message = arg.substr(3); i++; continue; }
        if (allowMessage && arg.rfind("--m=", 0) == 0) { f.message = arg.substr(4); i++; continue; }
        if (arg.size() > 1 && arg[0] == '-') {
            fprintf(stderr, "flag provided but not defined: %s\n", arg.c_str());
            commandUsage(command, stderr);
            exit(2);
        }
        break;
    }
    for (; i < args.size(); ++i) f.positional.push_back(args[i]);
    return f;
}

static void handleInit(const std::vector<std::string>& args) {
    ParsedFlags f = parseFlags("init", args);
    if (f.showHelp) { printInitUsage(stdout); return; }
    std::string path = requirePathArg(f.positional);
    requireOfficeFile(path);
    auto doc = Document::load(path);
    if (!doc) fatal("failed to load " + path);
    Store store(*doc);
    if (store.head() == "") store.setHead("");
    if (!doc->write(path)) fatal("failed to write " + path);
    printf("Initialized .co metadata inside %s\n", path.c_str());
}

static void handleCommit(const std::vector<std::string>& args) {
    ParsedFlags f = parseFlags("commit", args);
    if (f.showHelp) { printCommitUsage(stdout); return; }
    std::string path = requirePathArg(f.positional);
    requireOfficeFile(path);
    if (f.message.empty()) fatal("commit message required (-m)");
    auto doc = Document::load(path);
    if (!doc) fatal("failed to load " + path);
    int64_t nowUnix = (int64_t)time(nullptr);
    std::string hash = createCommit(*doc, f.message, nowUnix);
    if (!doc->write(path)) fatal("failed to write " + path);
    printf("Committed %s\n", hash.c_str());
}

static void handleLog(const std::vector<std::string>& args) {
    ParsedFlags f = parseFlags("log", args);
    if (f.showHelp) { printLogUsage(stdout); return; }
    std::string path = requirePathArg(f.positional);
    requireOfficeFile(path);
    auto doc = Document::load(path);
    if (!doc) fatal("failed to load " + path);
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
    ParsedFlags f = parseFlags("gc", args);
    if (f.showHelp) { printGCUsage(stdout); return; }
    std::string path = requirePathArg(f.positional);
    requireOfficeFile(path);
    auto doc = Document::load(path);
    if (!doc) fatal("failed to load " + path);
    GCStats stats;
    if (!garbageCollect(*doc, stats)) fatal("garbage collection failed");
    if (!doc->write(path)) fatal("failed to write " + path);
    printf("Garbage collection completed:\n");
    printf("  Reachable objects: %d\n", stats.reachableObjects);
    printf("  Packed objects: %d\n", stats.packedObjects);
    printf("  Removed loose objects: %d\n", stats.removedLoose);
    printf("  Removed old packs: %d\n", stats.removedPacks);
}

static void handleCheckout(const std::vector<std::string>& args) {
    ParsedFlags f = parseFlags("checkout", args);
    if (f.showHelp) { printCheckoutUsage(stdout); return; }
    std::pair<std::string, std::string> cp = requireCommitAndPathArg(f.positional);
    const std::string& commitHash = cp.first;
    const std::string& path = cp.second;
    requireOfficeFile(path);
    auto doc = Document::load(path);
    if (!doc) fatal("failed to load " + path);
    if (!checkoutCommit(*doc, commitHash)) fatal("checkout failed: " + commitHash);
    if (!doc->write(path)) fatal("failed to write " + path);
    printf("Checked out %s\n", commitHash.c_str());
}

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
    else if (cmd == "gc") handleGC(args);
    else if (cmd == "checkout") handleCheckout(args);
    else {
        printUsage(stdout);
        return 1;
    }
    return 0;
}
