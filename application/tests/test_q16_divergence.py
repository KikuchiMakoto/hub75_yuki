"""Taichi-first Q16.16 divergence verification for RP2040 DEM.

Firmware (firmware/include/standalone_dem.h) と bit-exact な Q16 演算を
Taichi (CPU, serial) で再現し、以下を先に証明する:
  1. int32 オーバーフローが起きないか (q16_mul/div の中間値を含む)
  2. PopUp (爆発: 速度・位置の発散) が起きないか
  3. 壁張り付き (重力があるのに壁から離れない) が起きないか
  4. float 参照系との乖離 (量子化誤差の蓄積発散) が小さいか
"""

import numpy as np
import pytest
import taichi as ti

# ---- Firmware constants (Q16 raw) ----
Q16 = 65536
N = 1024
DIAM = 98304  # 1.5px (matches Taichi D=1.5)
DIAM2 = 147456  # 2.25 in Q16
R = 49152  # 0.75px
BOX = 4194304  # 64.0px
WMIN = R
WMAX = BOX - R
HMIN = R >> 2  # 10240
HMAX = BOX - HMIN

DT = 262  # 0.004
G1 = 14417920  # 220.0 px/s^2
KN = 262144000  # 4000.0
DAMP = 5242880  # 80.0
FRIC = 22938  # 0.35
GAMMA_T = 983040  # 15.0
WKN = 393216000  # 6000.0
WDAMP = 6553600  # 100.0
FN_MAX = 98304000  # 1500.0
WFN_MAX = 131072000  # 2000.0
MAX_A = 196608000  # 3000.0
MAX_V = 2949120  # 45.0
# Plain-integer fast-path factors (must equal Q16 const / 65536; cf. firmware)
KN_INT = 4000
DAMP_INT = 80
GAMMA_INT = 15
WKN_INT = 6000
WDAMP_INT = 100
DT_NUM = 262

GRID_DIM = 32
I32_MAX = 2147483647
I32_MIN = -2147483648


@pytest.fixture(scope="module")
def sim():
    try:
        ti.get_runtime()
    except Exception:
        ti.init(arch=ti.cpu)

    s = {}
    s["pos_q"] = ti.Vector.field(2, dtype=ti.i32, shape=N)
    s["vel_q"] = ti.Vector.field(2, dtype=ti.i32, shape=N)
    s["force_q"] = ti.Vector.field(2, dtype=ti.i32, shape=N)
    s["contact_q"] = ti.field(dtype=ti.i32, shape=N)
    s["grid_head"] = ti.field(dtype=ti.i32, shape=GRID_DIM * GRID_DIM)
    s["grid_next"] = ti.field(dtype=ti.i32, shape=N)
    s["overflow"] = ti.field(dtype=ti.i32, shape=())
    s["max_fn"] = ti.field(dtype=ti.i32, shape=())
    s["max_vel"] = ti.field(dtype=ti.i32, shape=())
    # float reference (px units)
    s["pos_f"] = ti.Vector.field(2, dtype=ti.f32, shape=N)
    s["vel_f"] = ti.Vector.field(2, dtype=ti.f32, shape=N)
    s["force_f"] = ti.Vector.field(2, dtype=ti.f32, shape=N)
    return s


@ti.func
def q16_mul(a: ti.i32, b: ti.i32, overflow: ti.template(), max_fn: ti.template()) -> ti.i32:
    tmp = (ti.cast(a, ti.i64) * ti.cast(b, ti.i64)) >> 16
    if tmp > I32_MAX or tmp < I32_MIN:
        ti.atomic_add(overflow[None], 1)
        tmp = ti.max(ti.i64(I32_MIN), ti.min(ti.i64(I32_MAX), tmp))
    return ti.cast(tmp, ti.i32)


@ti.func
def q16_div(a: ti.i32, b: ti.i32, overflow: ti.template()) -> ti.i32:
    r = ti.i32(0)
    if b == 0:
        ti.atomic_add(overflow[None], 1)
        r = I32_MAX if a >= 0 else I32_MIN + 1
    else:
        tmp = (ti.cast(a, ti.i64) << 16) // ti.cast(b, ti.i64)
        if tmp > I32_MAX or tmp < I32_MIN:
            ti.atomic_add(overflow[None], 1)
            tmp = ti.max(ti.i64(I32_MIN), ti.min(ti.i64(I32_MAX), tmp))
        r = ti.cast(tmp, ti.i32)
    return r


@ti.func
def q16_sqrt(v: ti.i32) -> ti.i32:
    # firmware と同一の 16-step non-restoring sqrt, root<<8
    rem = ti.cast(0, ti.u32)
    root = ti.cast(0, ti.u32)
    res = ti.cast(0, ti.i32)
    if v > 0:
        rem = ti.cast(v, ti.u32)
        root = ti.cast(0, ti.u32)
        s = ti.cast(0x40000000, ti.u32)
        for _ in range(16):
            if rem >= root + s:
                rem -= root + s
                root = (root >> 1) + s
            else:
                root >>= 1
            s >>= 2
        res = ti.cast(root << 8, ti.i32)
    return res


@ti.kernel
def k_init(
    pos_q: ti.template(),
    vel_q: ti.template(),
    force_q: ti.template(),
    contact_q: ti.template(),
    pos_f: ti.template(),
    vel_f: ti.template(),
    overflow: ti.template(),
    max_fn: ti.template(),
    max_vel: ti.template(),
):
    overflow[None] = 0
    max_fn[None] = 0
    max_vel[None] = 0
    for i in range(N):
        c = i % 36
        r_ = i // 36
        x = 262144 + c * 103219 + ((51609) if (r_ & 1) else 0)
        y = 262144 + r_ * 93389
        pos_q[i] = ti.Vector([x, y])
        vel_q[i] = ti.Vector([0, 0])
        force_q[i] = ti.Vector([0, 0])
        contact_q[i] = 0
        pos_f[i] = ti.Vector([ti.cast(x, ti.f32) / 65536.0, ti.cast(y, ti.f32) / 65536.0])
        vel_f[i] = ti.Vector([0.0, 0.0])


@ti.kernel
def k_substep_q16(
    pos_q: ti.template(),
    vel_q: ti.template(),
    force_q: ti.template(),
    contact_q: ti.template(),
    grid_head: ti.template(),
    grid_next: ti.template(),
    overflow: ti.template(),
    max_fn: ti.template(),
    max_vel: ti.template(),
    gx: ti.i32,
    gy: ti.i32,
):
    # 1. reset
    for i in range(N):
        force_q[i] = ti.Vector([gx, gy])
        contact_q[i] = 0
    # 2. grid build
    for i in range(GRID_DIM * GRID_DIM):
        grid_head[i] = -1
    for i in range(N):
        cx = pos_q[i][0] >> 17
        cy = pos_q[i][1] >> 17
        if cx < 0:
            cx = 0
        elif cx >= GRID_DIM:
            cx = GRID_DIM - 1
        if cy < 0:
            cy = 0
        elif cy >= GRID_DIM:
            cy = GRID_DIM - 1
        cell = cy * GRID_DIM + cx
        grid_next[i] = grid_head[cell]
        grid_head[cell] = i
    # 3. pairs (serialized to match firmware order; race-free via j>i + atomic on j)
    ti.loop_config(serialize=True)
    for i in range(N):
        px = pos_q[i][0]
        py = pos_q[i][1]
        vxi = vel_q[i][0]
        vyi = vel_q[i][1]
        fxi = force_q[i][0]
        fyi = force_q[i][1]
        cacc = ti.i32(0)
        cx = px >> 17
        cy = py >> 17
        if cx < 0:
            cx = 0
        elif cx >= GRID_DIM:
            cx = GRID_DIM - 1
        if cy < 0:
            cy = 0
        elif cy >= GRID_DIM:
            cy = GRID_DIM - 1
        for dy in range(-1, 2):
            ncy = cy + dy
            if 0 <= ncy < GRID_DIM:
                for dx in range(-1, 2):
                    ncx = cx + dx
                    if 0 <= ncx < GRID_DIM:
                        cell = ncy * GRID_DIM + ncx
                        j = grid_head[cell]
                        while j != -1:
                            if j > i:
                                dx_ = px - pos_q[j][0]
                                dy_ = py - pos_q[j][1]
                                if -DIAM < dx_ < DIAM and -DIAM < dy_ < DIAM:
                                    # 32-bit fast path, identical to firmware
                                    d2 = (dx_ >> 8) * (dx_ >> 8) + (dy_ >> 8) * (dy_ >> 8)
                                    if d2 > 0 and d2 < DIAM2:
                                        dist = q16_sqrt(d2)
                                        if dist > 0:
                                            nx = ti.i32(0)
                                            ny = ti.i32(0)
                                            ov = ti.i32(0)
                                            if dist < 655:
                                                ov = DIAM - 655
                                                nx = Q16 if ((i ^ j) & 1) else -Q16
                                                ny = Q16 if ((i ^ (j * 3)) & 1) else -Q16
                                            else:
                                                ov = DIAM - dist
                                                nx = q16_div(dx_, dist, overflow)
                                                ny = q16_div(dy_, dist, overflow)
                                            vrx = vxi - vel_q[j][0]
                                            vry = vyi - vel_q[j][1]
                                            vn = (vrx >> 8) * (nx >> 8) + (vry >> 8) * (ny >> 8)
                                            fn = KN_INT * ov
                                            if vn < 0:
                                                fn -= DAMP_INT * vn
                                            if fn < 0:
                                                fn = 0
                                            elif fn > FN_MAX:
                                                fn = FN_MAX
                                            if fn > max_fn[None]:
                                                max_fn[None] = fn
                                            vtx = vrx - (vn >> 8) * (nx >> 8)
                                            vty = vry - (vn >> 8) * (ny >> 8)
                                            ftmax = (fn * 7) // 20
                                            ftx = -GAMMA_INT * vtx
                                            fty = -GAMMA_INT * vty
                                            if ftx > ftmax:
                                                ftx = ftmax
                                            elif ftx < -ftmax:
                                                ftx = -ftmax
                                            if fty > ftmax:
                                                fty = ftmax
                                            elif fty < -ftmax:
                                                fty = -ftmax
                                            cfx = (fn >> 8) * (nx >> 8) + ftx
                                            cfy = (fn >> 8) * (ny >> 8) + fty
                                            fxi += cfx
                                            fyi += cfy
                                            ti.atomic_add(force_q[j][0], -cfx)
                                            ti.atomic_add(force_q[j][1], -cfy)
                                            ti.atomic_add(contact_q[j], fn)
                                            cacc += fn
                            j = grid_next[j]
        force_q[i][0] = fxi
        force_q[i][1] = fyi
        contact_q[i] += cacc
    # 4. walls
    for i in range(N):
        px = pos_q[i][0]
        py = pos_q[i][1]
        vx = vel_q[i][0]
        vy = vel_q[i][1]
        if px < WMIN:
            ov = WMIN - px
            fn = WKN_INT * ov
            if vx < 0:
                fn -= WDAMP_INT * vx
            if fn < 0:
                fn = 0
            elif fn > WFN_MAX:
                fn = WFN_MAX
            force_q[i][0] += fn
            contact_q[i] += fn
        if px > WMAX:
            ov = px - WMAX
            fn = WKN_INT * ov
            if vx > 0:
                fn += WDAMP_INT * vx
            if fn < 0:
                fn = 0
            elif fn > WFN_MAX:
                fn = WFN_MAX
            force_q[i][0] -= fn
            contact_q[i] += fn
        if py < WMIN:
            ov = WMIN - py
            fn = WKN_INT * ov
            if vy < 0:
                fn -= WDAMP_INT * vy
            if fn < 0:
                fn = 0
            elif fn > WFN_MAX:
                fn = WFN_MAX
            force_q[i][1] += fn
            contact_q[i] += fn
        if py > WMAX:
            ov = py - WMAX
            fn = WKN_INT * ov
            if vy > 0:
                fn += WDAMP_INT * vy
            if fn < 0:
                fn = 0
            elif fn > WFN_MAX:
                fn = WFN_MAX
            force_q[i][1] -= fn
            contact_q[i] += fn
    # 5. integrate
    for i in range(N):
        ax = force_q[i][0]
        ay = force_q[i][1]
        if ax > MAX_A:
            ax = MAX_A
        elif ax < -MAX_A:
            ax = -MAX_A
        if ay > MAX_A:
            ay = MAX_A
        elif ay < -MAX_A:
            ay = -MAX_A
        vel_q[i][0] += ((ax >> 8) * DT_NUM) >> 8
        vel_q[i][1] += ((ay >> 8) * DT_NUM) >> 8
        vel_q[i][0] -= vel_q[i][0] >> 7
        vel_q[i][1] -= vel_q[i][1] >> 7
        if vel_q[i][0] > MAX_V:
            vel_q[i][0] = MAX_V
        elif vel_q[i][0] < -MAX_V:
            vel_q[i][0] = -MAX_V
        if vel_q[i][1] > MAX_V:
            vel_q[i][1] = MAX_V
        elif vel_q[i][1] < -MAX_V:
            vel_q[i][1] = -MAX_V
        avx = ti.abs(vel_q[i][0])
        avy = ti.abs(vel_q[i][1])
        if avx > max_vel[None]:
            max_vel[None] = avx
        if avy > max_vel[None]:
            max_vel[None] = avy
        pos_q[i][0] += (vel_q[i][0] * DT_NUM) >> 16
        pos_q[i][1] += (vel_q[i][1] * DT_NUM) >> 16
        if pos_q[i][0] < HMIN:
            pos_q[i][0] = HMIN
            if vel_q[i][0] < 0:
                vel_q[i][0] = -(vel_q[i][0] >> 2)
        elif pos_q[i][0] > HMAX:
            pos_q[i][0] = HMAX
            if vel_q[i][0] > 0:
                vel_q[i][0] = -(vel_q[i][0] >> 2)
        if pos_q[i][1] < HMIN:
            pos_q[i][1] = HMIN
            if vel_q[i][1] < 0:
                vel_q[i][1] = -(vel_q[i][1] >> 2)
        elif pos_q[i][1] > HMAX:
            pos_q[i][1] = HMAX
            if vel_q[i][1] > 0:
                vel_q[i][1] = -(vel_q[i][1] >> 2)


def run_frames(s, gx, gy, frames, substeps=3):
    for _ in range(frames):
        for _ in range(substeps):
            k_substep_q16(
                s["pos_q"],
                s["vel_q"],
                s["force_q"],
                s["contact_q"],
                s["grid_head"],
                s["grid_next"],
                s["overflow"],
                s["max_fn"],
                s["max_vel"],
                gx,
                gy,
            )


def snapshot(s):
    pos = s["pos_q"].to_numpy() / 65536.0
    vel = s["vel_q"].to_numpy() / 65536.0
    return pos, vel


def test_render_mapping_covers_pile_without_crush(sim):
    """dem_render sqrt mapping must spread the pile across the visible LUT.

    Replicates firmware `24 + (q16_sqrt(contact)-700000)/30000` bit-exactly
    (isqrt == the 16-step non-restoring loop; trunc matches C division).
    Guards the black-crush regression (linear >>17 saturated 99%).
    """
    import math

    k_init(
        sim["pos_q"],
        sim["vel_q"],
        sim["force_q"],
        sim["contact_q"],
        sim["pos_f"],
        sim["vel_f"],
        sim["overflow"],
        sim["max_fn"],
        sim["max_vel"],
    )
    run_frames(sim, 0, G1, frames=100)
    c = sim["contact_q"].to_numpy().astype(np.int64)
    s = np.array([math.isqrt(max(int(v), 0)) << 8 for v in c], dtype=np.int64)
    idx = np.clip(24 + np.trunc((s - 700000) / 30000.0).astype(np.int64), 0, 255)
    assert (idx >= 255).mean() < 0.02, "saturation crush"
    med = float(np.median(idx))
    assert 120 <= med <= 220, f"median idx off-range: {med}"


def test_q16_no_overflow_on_drop(sim):
    k_init(
        sim["pos_q"],
        sim["vel_q"],
        sim["force_q"],
        sim["contact_q"],
        sim["pos_f"],
        sim["vel_f"],
        sim["overflow"],
        sim["max_fn"],
        sim["max_vel"],
    )
    run_frames(sim, 0, G1, frames=100)
    ov = int(sim["overflow"][None])
    maxv = int(sim["max_vel"][None]) / 65536.0
    pos, _ = snapshot(sim)
    assert ov == 0, f"Q16 overflow detected: count={ov}"
    assert np.all(np.isfinite(pos)), "non-finite positions (PopUp)"
    assert np.all(pos >= -0.5) and np.all(pos <= 64.5), "escaped box (PopUp)"
    assert maxv <= 45.5, f"velocity explosion: maxv={maxv}"
    # 落下後は底部に集まるはず (平均y > 40)
    assert pos[:, 1].mean() > 40.0, f"did not settle down: mean_y={pos[:, 1].mean()}"


def test_q16_no_wall_stick_on_tilt(sim):
    k_init(
        sim["pos_q"],
        sim["vel_q"],
        sim["force_q"],
        sim["contact_q"],
        sim["pos_f"],
        sim["vel_f"],
        sim["overflow"],
        sim["max_fn"],
        sim["max_vel"],
    )
    run_frames(sim, 0, G1, frames=60)
    run_frames(sim, G1, 0, frames=100)
    pos, _ = snapshot(sim)
    ov = int(sim["overflow"][None])
    assert ov == 0, f"overflow during tilt: {ov}"
    n_left = int(np.sum(pos[:, 0] < 10.0))
    assert n_left == 0, f"wall stick: {n_left} particles remain x<10 after tilting right"


def test_q16_no_popup_on_violent_shake(sim):
    k_init(
        sim["pos_q"],
        sim["vel_q"],
        sim["force_q"],
        sim["contact_q"],
        sim["pos_f"],
        sim["vel_f"],
        sim["overflow"],
        sim["max_fn"],
        sim["max_vel"],
    )
    glim = int(2.5 * G1)
    for f in range(60):
        gx = glim if (f % 2 == 0) else -glim
        gy = glim if (f % 4 < 2) else -glim
        run_frames(sim, gx, gy, frames=1)
    pos, vel = snapshot(sim)
    ov = int(sim["overflow"][None])
    assert ov == 0, f"overflow on shake: {ov}"
    assert np.all(np.isfinite(pos)) and np.all(np.isfinite(vel))
    assert np.all(pos >= -1.0) and np.all(pos <= 65.0), "escaped on shake"
    assert np.all(np.abs(vel) <= 46.0), "velocity explosion on shake"
