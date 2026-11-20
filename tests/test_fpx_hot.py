"""The hot functions' float helpers and the four-lane helpers, game-free: bit-exact with the exact bodies.

a64_lift.py gives hot lifted functions their own copy of every FPX1 helper (a64_fpx_hot_*): the fast path inline, and past it
the exact path called without the context (a64_fpx_nc_*: the control word in, the status word in and out), the result
carried as a float or double. NEON's four-lane FADD / FSUB / FMUL, and FMUL / FMLA / FMLS by element (vectorize_elem), go
through a64_vfpx_add32 / sub32 / mul32 and a64_vfpx_fma32: one SSE or FMA op when every lane passes the scalar fast path's
tests, otherwise lane by lane through the scalar helpers, again without the context. And where the build has FMA
(x86-64-v3) the binary32 fused multiply-add's fast path is the host's instruction (a64_rtgen.fma32_native) instead of
suyu's binary64 form.

This builds what the real build builds (a64_fpx.h from a64_lift.fpx_header(), a64_suyu.h / a64_suyu.c from a64_rtgen.py,
through tests/test_fpx_sticky.py's generator) and compiles tests/fpx_hot_test.c against the real rt/a64.h at the build's
FP flags, at -march=x86-64-v3 and at -march=x86-64-v2 (the binary64 form; the four-lane multiply-add lane by lane). For
random operands (zeros, subnormals, infinities, NaNs, the extreme normals, products at the underflow and overflow edges,
cancellations) under random FPCR and FPSR states, each helper's value, FPSR and the rest of the context must equal the
exact body's: recomp_fpe_* for the scalars, and for four lanes the exact body lane by lane in order, FPSR carried from
lane to lane (what the lane loops did before).

    python -I -B tests/test_fpx_hot.py [--cc clang] [--cxx clang++] [--n 2000000]

Skipped (exit 0) where tests/test_fpx_sticky.py is.
"""
import argparse
import os
import platform
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_fpx_sticky as S  # noqa: E402  (its compilers, flags and generator)

SHAPES = {(a + s, a + s, n, n) for s in ("32", "64") for a, n in (("add", 2), ("sub", 2), ("div", 2), ("sqrt", 1), ("fma", 3))} | \
         {("mul32", "fma32", 2, 3), ("mul64", "fma64", 2, 3), ("recps32", "recps32", 2, 2), ("rsqrts32", "rsqrts32", 2, 2)}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc")
    p.add_argument("--cxx")
    p.add_argument("--n", type=int, default=2000000)
    a = p.parse_args()
    if platform.machine().lower() not in ("x86_64", "amd64"):
        S.skip("not an x86-64 host: %s" % platform.machine())
    cc = S.compiler(a.cc, "CC", ("clang", "gcc", "cc"), "clang")
    cxx = S.compiler(a.cxx, "CXX", ("clang++", "g++", "c++"), "clang++")
    if not cc or not cxx:
        S.skip("no C compiler" if not cc else "no C++ compiler")
    t0 = time.time()
    failed, ran = [], []
    with tempfile.TemporaryDirectory(prefix="hookshot-fpx-hot-") as tmp:
        S.generate(cxx, tmp, S.target_flags(cxx, tmp))
        import a64_lift
        with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:   # (every shape, not only the sticky test's two)
            f.write(a64_lift.fpx_header(SHAPES))
        for march, _ in S.BUILDS:
            exe = os.path.join(tmp, "fpx_hot_test_%s.exe" % march)
            S.build([cc] + S.target_flags(cc, tmp) + ["-march=" + march] + S.FLAGS +
                    ["-D_CRT_SECURE_NO_WARNINGS", "-Wno-everything", "-I" + os.path.join(S.KIT, "rt"), "-I" + tmp,
                     os.path.join(S.ROOT, "tests", "fpx_hot_test.c"), os.path.join(tmp, "a64_suyu.c"), "-o", exe],
                    "the driver at " + march)
            r = S.run([exe, str(a.n), "20261114"])
            print("%s: %s" % (march, r.stdout.rstrip().replace("\n", "\n  ")))
            if r.returncode == 3:
                print("fpx hot: %s skipped (this CPU is not %s)" % (march, march))
                continue
            ran.append(march)
            if r.returncode:
                failed.append(march)
    if not ran:
        S.skip("this CPU is not x86-64-v2")
    if failed:
        raise SystemExit("fpx hot: FAIL (%s) in %.1f s" % (", ".join(failed), time.time() - t0))
    print("fpx hot: pass in %.1f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
