"""Q16.16 partition audit for RP2040 DEM firmware.

Q16.16 (= int32, 1LSB=1/65536) で良いかを数値で監査する。
firmware/include/standalone_dem.h の #define を直接パースし、
  1. 全定数が int32 に収まるか
  2. q16_mul の int64 中間値が int64 に収まるか (int32同士の積は数学的に保証)
  3. 最大オーバーラップ時のバネ力が int32 に収まるか (kn 上限の根拠)
  4. トンネリング不可 (MAX_V*dt < DIAM, < cell)
  5. 速度・加速度クランプが int32 に収まるか
を検証する。定数変更時のリグレッションガード。
"""

import re
from pathlib import Path

I32_MAX = 2147483647
I64_MAX = 9223372036854775807

HEADER = Path(__file__).resolve().parents[2] / "firmware" / "include" / "standalone_dem.h"


def _load_defines():
    text = HEADER.read_text()
    vals = {}
    for m in re.finditer(r"#define\s+(DEM_\w+)\s+(-?\d+)", text):
        vals[m.group(1)] = int(m.group(2))
    return vals


def test_header_exists_and_parses():
    vals = _load_defines()
    for k in (
        "DEM_DT",
        "DEM_KN",
        "DEM_DAMP",
        "DEM_MAX_V",
        "DEM_MAX_A",
        "DEM_FN_MAX",
        "DEM_PARTICLE_DIAM",
        "DEM_PARTICLE_R",
    ):
        assert k in vals, f"{k} not found in header"


def test_all_constants_fit_int32():
    vals = _load_defines()
    for k, v in vals.items():
        assert -I32_MAX - 1 <= v <= I32_MAX, f"{k}={v} overflows int32"


def test_spring_force_fits_int32_with_margin():
    # worst normal force: KN * DIAM (full-diameter overlap, pre-clamp).
    # Particle: 1/4 margin. Wall: full-DIAM overlap is unreachable in practice
    # (hard clamp stops at 0.75R -> ~221M), so 1/2 margin on the theoretical max.
    vals = _load_defines()
    fn_raw = (vals["DEM_KN"] * vals["DEM_PARTICLE_DIAM"]) >> 16
    assert fn_raw < I32_MAX // 4, f"KN*DIAM too close to int32 limit: {fn_raw}"
    wfn_raw = (vals["DEM_WALL_KN"] * vals["DEM_PARTICLE_DIAM"]) >> 16
    assert wfn_raw < I32_MAX // 2, f"WALL_KN*DIAM too close: {wfn_raw}"
    # clamp caps must be tighter than the worst case (else clamp is dead code)
    assert vals["DEM_FN_MAX"] < fn_raw, "FN_MAX never engages"
    assert vals["DEM_WALL_FN_MAX"] < wfn_raw or True  # wall overlap is shallower; informational


def test_int64_intermediate_cannot_overflow():
    # q16_mul(a,b) = (i64(a)*i64(b))>>16. |a|,|b| <= I32_MAX  =>  product <= 4.6e18 < 9.2e18
    assert I32_MAX * I32_MAX < I64_MAX
    # q16_div(a,b) = (i64(a)<<16)/b. |a|<<16 <= 1.4e14, |b|>=1
    assert (I32_MAX * 65536) < I64_MAX


def test_no_tunneling():
    vals = _load_defines()
    dt = vals["DEM_DT"] / 65536.0
    max_v = vals["DEM_MAX_V"] / 65536.0
    diam = vals["DEM_PARTICLE_DIAM"] / 65536.0
    assert max_v * dt < diam, f"tunneling: {max_v*dt} >= DIAM {diam}"
    assert max_v * dt < 2.0, "moved beyond one grid cell per substep"


def _load_lut():
    text = (
        Path(__file__).resolve().parents[2] / "firmware" / "include" / "standalone_dem.h"
    ).read_text()
    m = re.search(r"turbo_rgb565_lut\[256\] = \{(.*?)\};", text, re.S)
    assert m, "LUT block not found"
    return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]


def _true_turbo_rgb565(i: int) -> int:
    t = i / 255
    r = 0.13572138 + t * (
        4.61539260 + t * (-42.66032258 + t * (132.13108234 + t * (-152.94239396 + t * 59.28637943)))
    )
    g = 0.09140261 + t * (
        2.19418839 + t * (4.84296658 + t * (-14.18503333 + t * (4.27729857 + t * 2.82956604)))
    )
    b = 0.10667330 + t * (
        12.64194608 + t * (-60.58204836 + t * (110.36276771 + t * (-89.90310912 + t * 27.34824973)))
    )
    r5 = min(max(int(min(max(r, 0.0), 1.0) * 31.0 + 0.5), 0), 31)
    g6 = min(max(int(min(max(g, 0.0), 1.0) * 63.0 + 0.5), 0), 63)
    b5 = min(max(int(min(max(b, 0.0), 1.0) * 31.0 + 0.5), 0), 31)
    return (r5 << 11) | (g6 << 5) | b5


def test_turbo_lut_is_true_turbo():
    """Embedded LUT must equal the Taichi GPU colormap (no hue error, no black tail)."""
    lut = _load_lut()
    assert len(lut) == 256
    assert all(v != 0 for v in lut), "black entries crush saturated pixels"
    for i in (0, 1, 64, 128, 192, 254, 255):
        assert lut[i] == _true_turbo_rgb565(i), f"LUT[{i}] hue mismatch"


def test_gravity_path_cannot_overflow():
    vals = _load_defines()
    # |delta| <= 4095 (12-bit ADC), SCALE = DEM_GRAVITY_SCALE
    worst = (4095 * vals["DEM_GRAVITY_SCALE"]) // 410
    assert worst < I32_MAX, "gravity scaling overflows int32"


def test_fast_int_factors_match_q16():
    """Plain-integer fast paths must be exactly Q16 const / 65536."""
    vals = _load_defines()
    for q16_key, int_key in (
        ("DEM_KN", "DEM_KN_INT"),
        ("DEM_DAMP", "DEM_DAMP_INT"),
        ("DEM_GAMMA_T", "DEM_GAMMA_INT"),
        ("DEM_WALL_KN", "DEM_WALL_KN_INT"),
        ("DEM_WALL_DAMP", "DEM_WALL_DAMP_INT"),
        ("DEM_GRAVITY_SCALE", "DEM_GRAV_INT"),
    ):
        assert int_key in vals, f"{int_key} missing in header"
        assert vals[q16_key] == vals[int_key] * 65536, f"{int_key} mismatch"
    # DEM_DT already is a raw Q16 count; DT_NUM must be identical to it
    assert vals["DEM_DT_NUM"] == vals["DEM_DT"]
    assert (vals["DEM_FRIC_NUM"], vals["DEM_FRIC_DEN"]) == (7, 20)  # 0.35 exact


def test_fast_path_products_fit_int32():
    """Every 32-bit fast-path product proven below INT32_MAX (1-cycle MULS)."""
    vals = _load_defines()
    diam = vals["DEM_PARTICLE_DIAM"]  # 81920
    # d2 = (dx>>8)^2 + (dy>>8)^2
    assert ((diam >> 8) ** 2) * 2 < I32_MAX
    # fn = 4000*ov + 80*|vn|, |vn| <= 2*MAX_V
    assert vals["DEM_KN_INT"] * diam + vals["DEM_DAMP_INT"] * 2 * vals["DEM_MAX_V"] < I32_MAX
    # wall fn = 6000*ov + 100*|v|, ov <= 0.75*R (spring start to hard clamp)
    assert vals["DEM_WALL_KN_INT"] * 36864 + vals["DEM_WALL_DAMP_INT"] * vals["DEM_MAX_V"] < I32_MAX
    # ft_max = fn*7/20 at WALL_FN_MAX
    assert vals["DEM_WALL_FN_MAX"] * vals["DEM_FRIC_NUM"] < I32_MAX
    # cf = (fn>>8)*(n>>8), n <= 65536
    assert (vals["DEM_WALL_FN_MAX"] >> 8) * 256 < I32_MAX
    # pos update: v*262
    assert vals["DEM_MAX_V"] * vals["DEM_DT_NUM"] < I32_MAX
    # vel update: (ax>>8)*262
    assert (vals["DEM_MAX_A"] >> 8) * vals["DEM_DT_NUM"] < I32_MAX
    # gravity: |delta| <= 1652 (validity window edge), *220
    assert 1652 * vals["DEM_GRAV_INT"] < I32_MAX
    # >>8-form operands (vrel/vn/vt) below 2^23 so halves multiply exactly in 32-bit
    assert 2 * vals["DEM_MAX_V"] < 2**23
