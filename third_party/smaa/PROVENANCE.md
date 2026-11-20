# SMAA

SMAA (Enhanced Subpixel Morphological Antialiasing), used for the Insert menu's Anti-aliasing row's **SMAA Ultra**
(`gpu/vk/vk_aa.cpp`, `gpu/vk/shaders/smaa_*.comp`).

- Upstream: https://github.com/iryoku/smaa, commit `71c806a838bdd7d517df19192a20f0c61b3ca29d`
- Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez
- License: MIT, in `LICENSE.txt`. Keep it with the sources. (Its text says the notice need not go with binaries.)

What is here and why:

| | |
|---|---|
| `SMAA.hlsl` | The reference implementation, unmodified (sha256 `fafdaa26c30ee6c12c2610c3093d7109f9df99207f23fcf9e215b6516296aafd`). The passes in `gpu/vk/shaders/smaa_*.comp` include it with `SMAA_GLSL_4` and `SMAA_PRESET_ULTRA` and call its functions from compute shaders |
| `AreaTex.h`, `SearchTex.h` | The two precomputed lookup textures: the bytes of upstream's `Textures/AreaTex.h` (160 x 560, two bytes a texel) and `Textures/SearchTex.h` (64 x 16, one byte a texel), under upstream's names (`areaTexBytes`, `searchTexBytes`, `AREATEX_*`, `SEARCHTEX_*`). The arrays are written out again, a decimal number a byte, so the two files are not upstream's text; each names the sha256 of its bytes (`35065cef2a02cabcad711d6bf430239ae64e27d71c4e4fa06f29cce2c992f0d2` and `3694eae5e9d44b8ebb4415a13f8c7b94dc08a2fc86658434d771c4610fe5744d`). `gpu/vk/vk_aa.cpp` includes them |
| `LICENSE.txt` | The MIT notice |

These files came by way of NX2navi (`runtime/shaders/smaa.glsl` and `runtime/third_party/smaa/` at its commit
`62cc3b13`), which names the upstream commit above; they were not compared with upstream again when they were added
here. To check them: `SMAA.hlsl` against upstream's file of that name, the two arrays against the bytes upstream's
headers list (`tests/test_aa_shaders.py` checks each header's bytes against the sha256 it names).

The SPIR-V in `gpu/vk/shaders/smaa_*_comp_spv.h` is compiled from `gpu/vk/shaders/smaa_*.comp` and `SMAA.hlsl` by
`python -I -B tests/test_aa_shaders.py --write` (glslangValidator), and the same test proves the checked-in headers
still match their sources.

Nothing in this folder is edited. A change belongs in a new folder with its own note, per `CONTRIBUTING.md`.
