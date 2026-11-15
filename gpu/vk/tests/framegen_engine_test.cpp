// Game-free regression for the choice of who makes the frames between the game's (gpu/vk/framegen_engine.h): DLSS Frame Generation,
// then AMD's library at 2x, then Triforce's own, and what RX_VK_FRAMEGEN_VENDOR changes.
#include <cstdio>
#include <cstring>

#include "../framegen_engine.h"

namespace {
int failures = 0;
void check(const char* name, bool ok) {
    if (ok) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    failures++;
}
namespace fe = mkwvk::fgengine;
using E = fe::Engine;
using V = fe::Vendor;

fe::Facts facts(V vendor, int framegen, bool dlssg, bool amd, bool fsr3fi) {
    fe::Facts f;
    f.vendor = vendor, f.framegen = framegen, f.dlssg_usable = dlssg, f.amd_usable = amd, f.fsr3fi_usable = fsr3fi;
    return f;
}
E pick(V vendor, int framegen, bool dlssg, bool amd, bool fsr3fi) { return fe::choose(facts(vendor, framegen, dlssg, amd, fsr3fi)).engine; }
}  // namespace

int main() {
    // the vendor text
    {
        bool known = false;
        check("unset is auto", fe::parse_vendor(nullptr, &known) == V::Auto && known);
        check("empty is auto", fe::parse_vendor("", &known) == V::Auto && known);
        check("auto", fe::parse_vendor("auto", &known) == V::Auto && known);
        check("nvidia", fe::parse_vendor("nvidia", &known) == V::Nvidia && known);
        check("amd", fe::parse_vendor("amd", &known) == V::Amd && known);
        check("triforce", fe::parse_vendor("triforce", &known) == V::Triforce && known);
        check("anything else is auto, said", fe::parse_vendor("intel", &known) == V::Auto && !known);
        check("case counts", fe::parse_vendor("AMD", &known) == V::Auto && !known);
    }
    // auto: the order DLSS-G, AMD's library at 2x, Triforce's own
    check("auto 2x: DLSS-G first", pick(V::Auto, 2, true, true, true) == E::DlssG);
    check("auto 2x: AMD's library without DLSS-G", pick(V::Auto, 2, false, true, true) == E::AmdLibrary);
    check("auto 2x: Triforce's passes without the library", pick(V::Auto, 2, false, false, true) == E::Fsr3fi);
    check("auto 2x: the flow without either", pick(V::Auto, 2, false, false, false) == E::Flow);
    check("auto 3x: DLSS-G where it makes 3x", pick(V::Auto, 3, true, true, false) == E::DlssG);
    check("auto 3x: never AMD's library", pick(V::Auto, 3, false, true, true) == E::Flow);
    check("auto Auto-row: never AMD's library", pick(V::Auto, 1, false, true, true) == E::Flow);
    check("auto Auto-row: DLSS-G where it runs", pick(V::Auto, 1, true, true, true) == E::DlssG);
    check("auto 8x: the flow", pick(V::Auto, 8, false, true, true) == E::Flow);
    // the row off, or the interpolation off for the run
    check("row off: none", pick(V::Auto, 0, true, true, true) == E::None);
    {
        fe::Facts f = facts(V::Auto, 2, true, true, true);
        f.enabled = false;
        check("disabled: none", fe::choose(f).engine == E::None);
    }
    // nvidia: DLSS-G alone
    check("nvidia: DLSS-G", pick(V::Nvidia, 2, true, true, true) == E::DlssG);
    check("nvidia: none where DLSS-G cannot run", pick(V::Nvidia, 2, false, true, true) == E::None);
    // amd: the library where it can, Triforce's own where it cannot, never DLSS-G
    check("amd: the library even with DLSS-G there", pick(V::Amd, 2, true, true, true) == E::AmdLibrary);
    check("amd 3x: Triforce's flow", pick(V::Amd, 3, true, true, true) == E::Flow);
    check("amd: no library: Triforce's passes", pick(V::Amd, 2, true, false, true) == E::Fsr3fi);
    check("amd: no library, no passes: flow", pick(V::Amd, 2, true, false, false) == E::Flow);
    // triforce: its own alone
    check("triforce 2x: its passes", pick(V::Triforce, 2, true, true, true) == E::Fsr3fi);
    check("triforce 2x without the passes: flow", pick(V::Triforce, 2, true, true, false) == E::Flow);
    check("triforce 4x: flow", pick(V::Triforce, 4, true, true, true) == E::Flow);
    check("triforce Auto: flow", pick(V::Triforce, 1, true, true, true) == E::Flow);
    // every choice says why when it is not the first of the order; the first says nothing in auto
    check("auto DLSS-G: nothing to say", !std::strlen(fe::choose(facts(V::Auto, 2, true, true, true)).why));
    check("auto AMD: says DLSS-G cannot", std::strlen(fe::choose(facts(V::Auto, 2, false, true, true)).why) > 0);
    check("auto own: says the library cannot", std::strlen(fe::choose(facts(V::Auto, 2, false, false, true)).why) > 0);
    // names
    check("names are all there", std::strlen(fe::name(E::DlssG)) && std::strlen(fe::name(E::AmdLibrary)) &&
                                     std::strlen(fe::name(E::Fsr3fi)) && std::strlen(fe::name(E::Flow)) && std::strlen(fe::name(E::None)));
    if (failures) return 1;
    std::puts("framegen_engine_test: all passed");
    return 0;
}
