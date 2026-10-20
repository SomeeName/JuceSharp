#include "../src/dsp/RoutingModules.h"
#include <array>
#include <cmath>
#include <iostream>

int main()
{
    dcrig::MergerModule mixer;
    std::array<float, 32> a, b, c, out;
    a.fill(1.f); b.fill(2.f); c.fill(4.f);
    std::vector<std::vector<float*>> branches{{a.data()}, {b.data()}, {c.data()}};
    float* dest[] = {out.data()};
    auto check = [&](float expected, int count = 2) {
        // First block completes the existing click-free gain ramp.
        mixer.mergeRt(dest, branches, count, 1, 32);
        mixer.mergeRt(dest, branches, count, 1, 32);
        for (float sample : out)
            if (std::abs(sample - expected) > 0.00001f) {
                std::cerr << "expected " << expected << ", got " << sample << '\n';
                return false;
            }
        return true;
    };
    if (!mixer.setParam("level1", .4f) || !mixer.setParam("level2", .8f)) return 1;
    if (!check(1.f)) return 2;
    if (!mixer.setParam("mute1", 1.f) || !check(.8f)) return 3;
    if (!mixer.setParam("solo1", 1.f) || !check(0.f)) return 4; // mute wins
    if (!mixer.setParam("mute1", 0.f) || !check(.2f)) return 5;
    if (!mixer.setParam("solo2", 1.f) || !check(1.f)) return 6; // multi-solo
    if (!mixer.setParam("solo1", 0.f) || !check(.8f)) return 7;
    if (!mixer.setParam("solo2", 0.f) || !check(1.f)) return 8;
    float value = 0.f;
    if (!mixer.getParam("level1", value) || value != .4f) return 9;
    if (!mixer.getParam("level2", value) || value != .8f) return 10;
    if (!mixer.setParam("solo3", 1.f) || !check(1.f)) return 11; // inactive solo ignored
    if (!check(4.f / 3.f, 3)) return 12; // third lane's solo becomes live
    if (!mixer.setParam("mute3", 1.f) || !check(0.f, 3)) return 13;
    mixer.reset();
    if (!check(0.f, 3)) return 14; // runtime reset preserves authored switches
    if (!mixer.getParam("solo3", value) || value != 1.f) return 15;
    std::cout << "Lane mixer: 15 audio/state checks passed\n";
    return 0;
}
