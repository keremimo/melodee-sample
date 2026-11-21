// SPDX-License-Identifier: AGPL-3.0-only
#include "melodee/plugin/custom_gui.h"
#include "melodee/plugin/custom_gui_scope.h"
#include <cassert>
#include <fstream>
#include <limits>
#include <string>

using namespace melodee;

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::filesystem::path example = argv[1], package = argv[2];
    const auto path = package / "melodee-gui.txt";
    const auto sample = read_custom_gui(example);
    assert(sample.enabled && sample.accent == 0xe9bf89 && sample.count == 3);
    assert(sample.parameters[1].index == 10 && std::string(sample.parameters[1].name.data()) == "Output");
    assert(!read_custom_gui(package).enabled);

    auto parse = [&](const std::string& text) {
        std::ofstream(path, std::ios::binary) << text;
        return read_custom_gui(package);
    };
    const std::string header = "MELODEE-GUI 1\naccent E9BF89\nscope stereo\n";
    assert(parse("MELODEE-GUI 1\r\naccent 00DBFF\r\nscope stereo\r\n").enabled);
    assert(parse(header + "parameter 0 Head Bump").count == 1);
    assert(!parse("MELODEE-GUI 2\naccent E9BF89\nscope stereo\n").enabled);
    assert(!parse("MELODEE-GUI 1\naccent X9BF89\nscope stereo\n").enabled);
    assert(!parse(header + "parameter 0 Input\nparameter 0 Output").enabled);
    assert(!parse(header + "parameter -1 Input").enabled);
    assert(!parse(header + "parameter 999999999999999 Input").enabled);
    assert(!parse(header + "parameter 1 ").enabled);
    assert(!parse(header + "parameter 1 A\nparameter 2 B\nparameter 3 C\nparameter 4 D").enabled);
    assert(!parse(header + std::string(1024, 'x')).enabled);
    std::filesystem::remove(path);
    std::filesystem::create_symlink(example / path.filename(), path);
    assert(!read_custom_gui(package).enabled);

    CustomGuiScope scope;
    using namespace std::chrono_literals;
    const auto time = CustomGuiScope::Time {} + 1s;
    assert(scope.read(time) == CustomGuiTrace {});
    const float left[] {-1.0f, .5f, std::numeric_limits<float>::quiet_NaN()};
    const float right[] {0.25f, -4.0f, 0.0f};
    scope.write(left, right, 3, time);
    constexpr uint32_t stereo = 0x1f813f81;
    assert(scope.read(time).back() == stereo);
    assert(scope.read(time).front() == 0);
    for (unsigned n = 0; n < 100; ++n) scope.write(left, right, 3, time + n * 1ms);
    const auto full = scope.read(time + 100ms);
    assert(std::all_of(full.begin(), full.end(), [](auto pair) { return pair == stereo; }));
    assert(scope.read(time + 349ms) == CustomGuiTrace {});
    const float silent[] {0, 0, 0};
    scope.write(silent, right, 3, time + 350ms);
    const auto resumed = scope.read(time + 350ms);
    assert(resumed.back() == 0x1f810000 && resumed[resumed.size() - 2] == 0);
}
