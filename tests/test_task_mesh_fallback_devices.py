"""The GPUs whose task + mesh draws take the compute fallback by default (gpu/vk/vk_main.cpp, issue 996).

On AMD's Windows driver the native task + mesh path timed out the driver on Navi 22 (#1747) and on Navi 21: an RX 6800 XT lost
the device within minutes at Hyrule Castle's entrance, and played 25 minutes there with none through the fallback. The default
was those two chips by their device ids. It is now every GPU of that driver's branch for RDNA 1 and 2 (the one whose
driverInfo names the "AMD proprietary shader compiler"), which on a GPU with mesh shaders means RDNA 2: the RX 6600 and the
other RX 6000 cards lost the device the same way in NX2navi, and the driver for RDNA 3 and 4 (LLPC) is left native.

This checks that the default is that branch, only for AMD and only on Windows, that no list of device ids narrows it again,
that the branch is told by the driver and its driverInfo (the test tests/test_vendor_fixes.py runs on made-up devices for
nan_bits, which is the same function), and that RX_VK_TASK_MESH_FALLBACK still decides over it.

python -I -B tests/test_task_mesh_fallback_devices.py
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def body(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    vendor = (ROOT / "gpu/vk/vk_vendor.h").read_text(encoding="utf-8")
    at = main_cpp.index("bool task_mesh_fallback = false;")
    block = main_cpp[at:main_cpp.index("dev.task_mesh_fallback =", at)]
    default = re.search(r"#ifdef _WIN32\n(.*?)#endif\n", block, re.S)
    assert default, "the default is no longer Windows-only"
    code = "\n".join(line for line in default.group(1).splitlines() if not line.strip().startswith("//"))
    line = re.search(r"task_mesh_fallback = (.*?);", code, re.S)
    assert line, "no default for task_mesh_fallback"
    cond = line.group(1)
    assert "dev.props.vendorID == 0x1002" in cond, "the default is not limited to AMD"
    assert re.search(r"&&\s*amd_older\b", cond), "the default is not AMD's driver branch for RDNA 1 and 2"
    assert "deviceID" not in cond, "a list of device ids narrows the default again: the RX 6600 and others are left out"
    # (the branch is told once, from the driver's own properties, where the other vendor facts are read)
    assert "amd_older = amd_older_compiler(facts);" in main_cpp, "amd_older is not vk_vendor.h's amd_older_compiler"
    assert main_cpp.index("amd_older = amd_older_compiler(facts);") < at, "amd_older is set after the default reads it"
    rule = body(vendor, "inline bool amd_older_compiler(")
    assert "f.driver_id == VK_DRIVER_ID_AMD_PROPRIETARY" in rule and '"proprietary shader compiler"' in rule, \
        "amd_older_compiler no longer tells the branch by AMD's proprietary driver and its driverInfo"
    assert "x.nan_bits = amd_older_compiler(f);" in vendor, \
        "nan_bits is no longer the same branch: tests/test_vendor_fixes.py no longer checks amd_older_compiler on its devices"
    # (the environment variable is read after the default, so it decides for any GPU)
    override = block.index('getenv("RX_VK_TASK_MESH_FALLBACK")')
    assert override > default.end(), "RX_VK_TASK_MESH_FALLBACK is read before the default and is overwritten by it"
    # (and only a GPU with mesh shaders takes it: one without draws through the older fallback, as before)
    assert "dev.task_mesh_fallback = task_mesh_fallback && dev.mesh_shader" in main_cpp, \
        "the task + mesh fallback no longer needs a GPU with mesh shaders"
    print("PASS: task + mesh fallback by default on Windows for AMD's driver branch for RDNA 1 and 2")


if __name__ == "__main__":
    main()
