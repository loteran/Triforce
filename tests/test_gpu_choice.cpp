#include "../gpu/vk/gpu_choice.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::gpu_choice::menu_device;
    CHECK("Auto stays Auto", menu_device(0, 2) == -1);
    CHECK("the n-th usable GPU", menu_device(1, 2) == 0 && menu_device(2, 2) == 1);
    CHECK("a GPU taken out since is Auto", menu_device(2, 1) == -1);
    CHECK("a negative choice (a hand-edited ini) is Auto", menu_device(-1, 2) == -1);
    CHECK("no usable GPU: Auto", menu_device(1, 0) == -1);

    // ((Triforce) 11-20) by the saved name first: the launcher numbered the cards in Windows' order before the game's first run
    using mkwvk::gpu_choice::pick;
    using mkwvk::gpu_choice::name_key;
    const std::vector<std::string> laptop = {"NVIDIA GeForce RTX 5060 Laptop GPU", "AMD Radeon(TM) 860M Graphics"};
    CHECK("the name finds the card the number missed (the ticket's 2 = the 860M here)",
          pick("NVIDIA GeForce RTX 5060 Laptop GPU", laptop, 2) == 0);
    CHECK("no name: the number, as before", pick("", laptop, 2) == 1 && pick("", laptop, 1) == 0);
    CHECK("Auto stays Auto whatever name is left in the file", pick("NVIDIA GeForce RTX 5060 Laptop GPU", laptop, 0) == -1);
    CHECK("a name no card has (taken out): the number", pick("NVIDIA GeForce RTX 4090", laptop, 1) == 0);
    CHECK("spaces and case do not matter", pick("  amd  radeon(tm)   860m graphics ", laptop, 1) == 1);
    CHECK("name_key", name_key("AMD  Radeon RX 7800 XT\t") == "amd radeon rx 7800 xt");
    const std::vector<std::string> twins = {"NVIDIA GeForce RTX 3090", "NVIDIA GeForce RTX 3090"};
    CHECK("two cards of one name: the numbered one when it has that name", pick("NVIDIA GeForce RTX 3090", twins, 2) == 1);
    CHECK("a number past the list with a name: the first of that name", pick("NVIDIA GeForce RTX 3090", twins, 5) == 0);
    std::puts("gpu choice: all checks passed");
}
