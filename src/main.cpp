#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app.hpp"

namespace {

void printUsage(const char* program) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --scene <name>         load a scene at startup:\n"
        "                         lensing, disk, flash, precession, both\n"
        "  --size <W>x<H>         window size in pixels (default 1280x800)\n"
        "  --no-ui                start with the control panel hidden\n"
        "  --frames <n>           run n frames, then exit\n"
        "  --screenshot <file>    with --frames: save the last frame as PNG\n"
        "  -h, --help             show this help\n",
        program);
}

}  // namespace

int main(int argc, char** argv) {
    bh::AppOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value after %s\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--scene") {
            options.scene = next();
        } else if (arg == "--size") {
            if (std::sscanf(next(), "%dx%d", &options.width, &options.height) != 2 ||
                options.width <= 0 || options.height <= 0) {
                std::fprintf(stderr, "--size expects WIDTHxHEIGHT, e.g. 1600x900\n");
                return 2;
            }
        } else if (arg == "--no-ui") {
            options.hideUi = true;
        } else if (arg == "--frames") {
            options.frames = std::atoi(next());
        } else if (arg == "--screenshot") {
            options.screenshotPath = next();
        } else {
            std::fprintf(stderr, "Unknown option: %s\n\n", arg.c_str());
            printUsage(argv[0]);
            return 2;
        }
    }

    if (!options.screenshotPath.empty() && options.frames <= 0) {
        options.frames = 300;
    }

    bh::App app;
    return app.run(options);
}
