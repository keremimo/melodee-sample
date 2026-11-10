// SPDX-License-Identifier: AGPL-3.0-only
// Melodee entry point: the sample-base runtime (base/src/runtime/) hosting
// MelodeeApp, plus Melodee's own tools (--self-test, previews, sound check).
#include "runtime/app.h"
#include "runtime/control_socket.h"
#include "runtime/inventory.h"
#include "runtime/options.h"
#include "runtime/run_app.h"
#include "melodee/melodee_app.h"
#include "melodee/tools.h"
#include <sys/stat.h>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc, char** argv) {
    umask(0077);
    try {
        if (auto code = melodee::run_tool(argc, argv)) return *code;
        auto options = parse_options(argc, argv, "melodee");
        switch (options.mode) {
        case Options::Mode::Run: {
            if (options.app != "melodee") throw std::runtime_error("This build runs only Melodee, not " + options.app);
            auto app = std::make_unique<MelodeeApp>(); // Large: keep it off the stack.
            return run_app(*app, options);
        }
        case Options::Mode::Control: return send_control_command(options.runtime, options.command);
        case Options::Mode::ControlStream: return stream_control_commands(options.runtime);
        case Options::Mode::Hardware: return print_hardware();
        }
    } catch (const std::exception& error) {
        std::cerr << "melodee: " << error.what() << '\n';
    }
    return 1;
}
