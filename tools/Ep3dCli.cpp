// ep3d -- the headless command line (M17.27).
//
// A CONSOLE program, not the viewer with a flag. The viewer builds a
// QApplication before it reads argv, so every "headless" run through it is a
// GUI process that happens not to show a window -- which is a different thing
// from a tool you can run in CI, in a container, or over ssh. ViewerCore is
// Qt-free (it is the model and the commands, not the widgets), so the script
// interpreter links against it directly and this binary needs no Qt at all.
//
//   ep3d --script part.txt [--out part.ep3d] [--quiet]
//   ep3d --open part.ep3d [--script more.txt] [--out part.ep3d]
//   ep3d --connect [PORT] [--script part.txt]      drive a RUNNING viewer
//   ep3d --help

#include "Cli/SketchScript.h"
#include "Ep3dConnect.h"
#include "Core/Document/PartDocument.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonAssemblySolver.h"
#include "Solver/GaussNewtonSketchSolver.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace paramcad;

namespace {

void PrintHelp() {
    std::printf(
        "ep3d -- run a sketch script without the GUI\n"
        "\n"
        "  ep3d --script FILE [--open FILE] [--out FILE] [--quiet]\n"
        "  ep3d --open FILE [--out FILE]\n"
        "  ep3d --connect [PORT] [--script FILE]\n"
        "\n"
        "  --script FILE   the script to run (stdin when --connect is given "
        "without one)\n"
        "  --connect PORT  send it to a RUNNING viewer started with --listen,\n"
        "                  on 127.0.0.1 (default port 5310) -- the geometry\n"
        "                  appears in the window as each line lands\n"
        "  --open FILE     start from an existing .ep3d instead of an empty document\n"
        "  --out FILE      save when the script finishes (a `save` line does this too)\n"
        "  --quiet         print only failures\n"
        "  --help          this, plus the vocabulary\n"
        "\n"
        "Commands:\n"
        "  sketch NAME | tool NAME | click U V | finish\n"
        "  constrain KIND REF... | dimension KIND REF... VALUE [as NAME]\n"
        "  pad BODY LENGTH [as NAME] | solve | save PATH | echo TEXT\n"
        "\n"
        "A REF is Line1, or Line1.start / Line1.end / Circle1.center,\n"
        "or Spline1.p3 -- a spline point, counted from 0. Its first and\n"
        "last are its .start and .end and are written that way, so one\n"
        "point never has two names.\n"
        "Entities are named as they are made; every command logs what it made.\n"
        "\n");
    const auto list = [](const char* what, const std::vector<std::string>& names) {
        std::printf("%s:\n ", what);
        int column = 1;
        for (const std::string& name : names) {
            if (column + static_cast<int>(name.size()) + 1 > 76) {
                std::printf("\n ");
                column = 1;
            }
            std::printf(" %s", name.c_str());
            column += static_cast<int>(name.size()) + 1;
        }
        std::printf("\n\n");
    };
    list("Tools", ScriptToolNames());
    list("Constraints", ScriptConstraintNames());
    list("Dimensions", ScriptDimensionNames());
}

// argv scanning that accepts BOTH `--flag value` and `--flag=value`, and
// REFUSES a flag it does not know.
//
// The viewer learned this three times over: a flag it silently discarded looked
// from the outside exactly like one it had honoured, and a CI run went green
// having tested something else. Same rule here, from the first commit.
const char* ValueFor(int argc, char** argv, int i, const char* flag, bool& present) {
    present = false;
    const std::size_t length = std::strlen(flag);
    if (std::strncmp(argv[i], flag, length) != 0) return nullptr;
    present = true;
    if (argv[i][length] == '=') return argv[i] + length + 1;
    if (argv[i][length] != '\0') {
        present = false; // --scriptXYZ is not --script
        return nullptr;
    }
    return (i + 1 < argc) ? argv[i + 1] : nullptr;
}

bool ReadFile(const std::string& path, std::string* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    *out = buffer.str();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintHelp();
        return 2;
    }

    const char* scriptPath = nullptr;
    const char* openPath = nullptr;
    const char* outPath = nullptr;
    bool quiet = false;
    bool connectRequested = false;
    unsigned short connectPort = 5310;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            PrintHelp();
            return 0;
        }
        if (std::strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
            continue;
        }
        bool present = false;
        if (const char* value = ValueFor(argc, argv, i, "--script", present); present) {
            if (value == nullptr || value[0] == '\0') {
                std::fprintf(stderr, "--script needs a file path\n");
                return 2;
            }
            scriptPath = value;
            continue;
        }
        if (const char* value = ValueFor(argc, argv, i, "--open", present); present) {
            if (value == nullptr || value[0] == '\0') {
                std::fprintf(stderr, "--open needs a file path\n");
                return 2;
            }
            openPath = value;
            continue;
        }
        if (const char* value = ValueFor(argc, argv, i, "--connect", present); present) {
            connectRequested = true;
            // The PORT is optional. A following token that is not a number
            // belongs to the next flag, so it is left alone.
            if (value != nullptr && value[0] != '\0') {
                char* end = nullptr;
                const long parsed = std::strtol(value, &end, 10);
                if (end != nullptr && *end == '\0' && parsed > 0 && parsed <= 65535)
                    connectPort = static_cast<unsigned short>(parsed);
            }
            continue;
        }
        if (const char* value = ValueFor(argc, argv, i, "--out", present); present) {
            if (value == nullptr || value[0] == '\0') {
                std::fprintf(stderr, "--out needs a file path\n");
                return 2;
            }
            outPath = value;
            continue;
        }
        // A VALUE that belongs to the flag before it, not an unknown argument.
        if (i > 0 && argv[i - 1][0] == '-' && argv[i][0] != '-') continue;
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "unknown option '%s' -- try --help\n", argv[i]);
            return 2;
        }
        std::fprintf(stderr, "unexpected argument '%s' -- try --help\n", argv[i]);
        return 2;
    }

    if (connectRequested) {
        // TO A RUNNING VIEWER. The document lives in that process, so nothing
        // here opens, solves or saves -- `save` is a command the script sends,
        // and the viewer runs it.
        if (openPath != nullptr || outPath != nullptr) {
            std::fprintf(stderr,
                         "--connect drives a running viewer, so --open and --out do not "
                         "apply; send `save PATH` as a command instead\n");
            return 2;
        }
        std::string text;
        if (scriptPath != nullptr) {
            if (!ReadFile(scriptPath, &text)) {
                std::fprintf(stderr, "could not read %s\n", scriptPath);
                return 1;
            }
        } else {
            // STDIN, so the connection can be typed at or piped into. This is
            // what makes it usable as a console for a window that is open.
            std::ostringstream buffer;
            buffer << std::cin.rdbuf();
            text = buffer.str();
        }
        return ep3d::SendScript(connectPort, text, quiet);
    }

    if (scriptPath == nullptr && openPath == nullptr) {
        std::fprintf(stderr, "nothing to do: give --script, --open or --connect\n");
        return 2;
    }

    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;

    std::unique_ptr<PartDocument> owned;
    PartDocument* document = nullptr;
    if (openPath != nullptr) {
        LoadResult loaded = loadPartDocumentFromFile(openPath);
        if (!loaded) {
            std::fprintf(stderr, "could not open %s: %s\n", openPath, loaded.message.c_str());
            return 1;
        }
        owned = std::move(loaded.document);
        document = owned.get();
        if (!quiet) std::printf("opened %s\n", openPath);
    } else {
        owned = std::make_unique<PartDocument>("Part");
        document = owned.get();
    }
    document->setGeometryKernel(&kernel);
    document->setSketchSolver(&solver);

    if (scriptPath != nullptr) {
        std::string text;
        if (!ReadFile(scriptPath, &text)) {
            std::fprintf(stderr, "could not read %s\n", scriptPath);
            return 1;
        }
        // A CLOSED-LOOP SOLVER, because a script can mate a linkage. It is
        // handed in here rather than made inside the script library for the
        // same reason the sketch solver is: that library links no backend.
        GaussNewtonAssemblySolver assemblySolver;
        const ScriptOutcome outcome = RunSketchScript(*document, text, &assemblySolver);
        if (!quiet)
            for (const ScriptLogEntry& entry : outcome.log)
                std::printf("line %d: %s\n", entry.line, entry.text.c_str());
        if (!outcome.ok) {
            // FLUSHED FIRST: the log explains where the script got to, and two
            // independently buffered streams print it AFTER the failure it
            // explains -- which reads as though nothing ran at all.
            std::fflush(stdout);
            std::fprintf(stderr, "line %d: %s\n", outcome.failedLine,
                         outcome.message.c_str());
            return 1;
        }
        if (!quiet) std::printf("%s\n", outcome.message.c_str());
    }

    if (outPath != nullptr) {
        const SaveResult saved = savePartDocumentToFile(*document, outPath);
        if (!saved) {
            std::fprintf(stderr, "could not save %s: %s\n", outPath, saved.message.c_str());
            return 1;
        }
        if (!quiet) std::printf("saved %s\n", outPath);
    }
    return 0;
}
