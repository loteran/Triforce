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
    f.amd_gpu = true;   // (the order; the GPU rule has its own checks below)
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
    {
        fe::Facts f = facts(V::Auto, 2, false, true, true);
        f.amd_gpu = false;
        check("auto 2x, not an AMD GPU: Triforce's passes, not AMD's library", fe::choose(f).engine == E::Fsr3fi && std::strlen(fe::choose(f).why) > 0);
        f.fsr3fi_usable = false;
        check("auto 2x, not an AMD GPU, no passes: the flow", fe::choose(f).engine == E::Flow);
        f.vendor = V::Amd;
        check("amd chosen on purpose, not an AMD GPU: the library", fe::choose(f).engine == E::AmdLibrary);
    }
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
    // the Frame generation engine row: the vendors in order, the env var over it, NVIDIA without DLSS-G is Auto, and stepping skips what cannot run
    {
        check("row 0 is auto", fe::vendor_from_row(0) == V::Auto);
        check("row 1 is nvidia", fe::vendor_from_row(1) == V::Nvidia);
        check("row 2 is amd", fe::vendor_from_row(2) == V::Amd);
        check("row 3 is triforce", fe::vendor_from_row(3) == V::Triforce);
        check("a row past the end is auto", fe::vendor_from_row(9) == V::Auto && fe::vendor_from_row(-1) == V::Auto);
        check("the row rules without the env var", fe::effective_vendor(false, V::Auto, 3, true) == V::Triforce);
        check("the env var goes over the row", fe::effective_vendor(true, V::Amd, 3, true) == V::Amd);
        check("the env var 'auto' goes over the row too", fe::effective_vendor(true, V::Auto, 3, true) == V::Auto);
        check("a row on NVIDIA without DLSS-G is auto", fe::effective_vendor(false, V::Auto, 1, false) == V::Auto);
        check("the env var on nvidia without DLSS-G stays strict", fe::effective_vendor(true, V::Nvidia, 0, false) == V::Nvidia);
        check("a row on AMD stays AMD (the choice falls to Triforce's own by itself)", fe::effective_vendor(false, V::Auto, 2, false) == V::Amd);
        check("all four run with DLSS-G and the library", fe::rows_available(true, true) == 15u);
        check("auto and triforce always run", fe::rows_available(false, false) == 9u);
        check("step over what cannot run", fe::step_row(0, 1, fe::rows_available(false, false)) == 3 && fe::step_row(3, 1, fe::rows_available(false, false)) == 0);
        check("step down wraps", fe::step_row(0, -1, fe::rows_available(true, true)) == 3 && fe::step_row(3, -1, fe::rows_available(true, true)) == 2);
        check("step with only the library", fe::step_row(0, 1, fe::rows_available(false, true)) == 2);
        check("a value that cannot run steps as auto does", fe::step_row(1, 1, fe::rows_available(false, false)) == 3 && fe::step_row(1, -1, fe::rows_available(false, false)) == 3);
        check("a saved value past the row steps as auto does", fe::step_row(7, 1, fe::rows_available(true, true)) == 1);
        // the row's choice, through the order
        check("row triforce makes it Triforce's own", pick(fe::effective_vendor(false, V::Auto, 3, true), 2, true, true, true) == E::Fsr3fi);
        check("row nvidia without DLSS-G falls to the order", pick(fe::effective_vendor(false, V::Auto, 1, false), 2, false, true, true) == E::AmdLibrary);
    }
    return 0;
}
