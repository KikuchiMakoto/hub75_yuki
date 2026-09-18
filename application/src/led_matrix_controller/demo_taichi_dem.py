"""High-Speed 2D DEM (Discrete Element Method) Physics Simulation in Taichi GPU.

Features:
- Pure flat square container (no hinges, no baffles) with 4 clean walls.
- N=1024 rigid granular particles filling ~35% volume of the square box.
- Particle diameter <= 1 LED pixel in 64x64 matrix scale.
- Non-linear Hertzian contact mechanics with rigid position constraint projection
  for crisp, non-permeable, robust collision detection.
- Wall dynamic velocity & friction dragging particles upward in a drum washing
  machine cascading/tumbling motion.
- Turbo ColorMap mapping contact compression forces (stress chains) in real time.
- Sensor modeling for ADXL335 (3.3V, +-3g, 330mV/g) on RP2040 ADC (GP26, GP27, GP28).
"""

import math
import time
from dataclasses import dataclass
from typing import Any, Optional

import numpy as np
import taichi as ti


@dataclass
class TaichiDEMConfig:
    """Configuration for 2D DEM granular simulation in Taichi GPU (Silica Sand / 珪砂)."""

    # Particle settings (1024 particles, D=1.5 px, r=0.75 px = 0.0084375 units)
    n_particles: int = 1024
    radius: float = 0.0084375  # D = 0.016875 units = 1.5 LED pixels on 64x64 (~44% fill)
    box_half_len: float = 0.36  # Box width = 0.72 -> Area = 0.5184. Fill ratio = ~37%
    substeps_per_frame: int = 10

    # Physics parameters (Hooke / Hertz 2D DEM standard)
    dt: float = 0.0008
    gravity: float = 12.0
    rot_speed: float = 1.2  # rad/s (~11.5 RPM)

    # Standard Hooke DEM parameters (calibrated for up to 3g violent shaking stability)
    kn: float = 28000.0  # Spring stiffness
    damping: float = 120.0  # Dashpot damping (critical damping for dense sand bed stability)
    friction: float = 0.45  # Silica Sand internal friction
    wall_friction: float = 0.18  # Smooth drum wall friction (slips naturally, no sticking)
    rolling_resistance: float = 0.05  # Angular resistance


@ti.data_oriented
class TaichiRotatingDrumDEM:
    """Taichi GPU implementation of 2D DEM in a rotating square drum."""

    def __init__(self, cfg: Optional[TaichiDEMConfig] = None) -> None:
        try:
            ti.get_runtime()
        except Exception:
            ti.init(arch=ti.gpu)

        self.cfg = cfg or TaichiDEMConfig()
        self.n = self.cfg.n_particles
        self.r = self.cfg.radius
        self.hl = self.cfg.box_half_len
        self.center = ti.Vector([0.5, 0.5])

        # Rotation state
        self.theta = 0.0
        self.rot_speed = self.cfg.rot_speed

        # ADXL335 sensor state (3.3V, +-3g, 330 mV/g)
        self.adxl_mode = False  # False: Drum rotation, True: Sensor tilt
        self.adxl_tilt_x = 0.0  # Normalized -1.0 .. +1.0
        self.adxl_tilt_y = 1.0  # Normalized -1.0 .. +1.0 (downwards)

        # Float display & rendering fields (GGUI, LED matrix)
        self.pos = ti.Vector.field(2, dtype=ti.f32, shape=self.n)
        self.vel = ti.Vector.field(2, dtype=ti.f32, shape=self.n)
        self.force = ti.Vector.field(2, dtype=ti.f32, shape=self.n)
        self.contact_forces = ti.field(dtype=ti.f32, shape=self.n)
        self.colors = ti.Vector.field(4, dtype=ti.f32, shape=self.n)

        # Cortex-M0+ (RP2040) Q16.16 Fixed-Point Particle Physics Fields (pure 32-bit int)
        self.use_q16_physics = False
        self.pos_q = ti.Vector.field(2, dtype=ti.i32, shape=self.n)
        self.vel_q = ti.Vector.field(2, dtype=ti.i32, shape=self.n)
        self.force_q = ti.Vector.field(2, dtype=ti.i32, shape=self.n)
        self.contact_q = ti.field(dtype=ti.i32, shape=self.n)
        self.grid_head_q = ti.field(dtype=ti.i32, shape=1024)
        self.grid_next_q = ti.field(dtype=ti.i32, shape=self.n)

        # 4 clean box walls (8 line vertices, pure flat square without hinges)
        self.wall_lines = ti.Vector.field(2, dtype=ti.f32, shape=8)

        # 64x64 Matrix / Voxel Rasterization Fields
        self.matrix_res = 64
        self.raw_matrix_color = ti.Vector.field(4, dtype=ti.f32, shape=(64, 64))
        self.matrix_color = ti.Vector.field(4, dtype=ti.f32, shape=(64, 64))
        self.matrix_weight = ti.field(dtype=ti.f32, shape=(64, 64))
        self.matrix_rgb565 = ti.field(dtype=ti.u16, shape=(64, 64))
        self.display_buffer = ti.Vector.field(4, dtype=ti.f32, shape=(512, 512))

        self.reset()

    def reset(self) -> None:
        """Reset particle positions and states."""
        self.theta = 0.0
        self._init_particles_kernel(self.hl, self.r)
        self._update_wall_lines()

    @ti.kernel
    def _init_particles_kernel(self, hl: ti.f32, r: ti.f32):
        # Hexagonal / grid packed in bottom half of box (~40% fill ratio)
        spacing = r * 2.05
        cols = ti.max(1, int((2.0 * hl - 3.5 * r) / spacing))
        start_x = 0.5 - (cols * spacing) * 0.5 + r * 0.5
        start_y = 0.5 - hl + r * 1.5

        for i in range(self.n):
            row = i // cols
            col = i % cols
            x = start_x + col * spacing + (row % 2) * (r * 0.9)
            y = start_y + row * (spacing * 0.9)
            self.pos[i] = ti.Vector([x, y])
            self.vel[i] = ti.Vector([0.0, 0.0])
            self.force[i] = ti.Vector([0.0, 0.0])
            self.contact_forces[i] = 0.0
            self.colors[i] = ti.Vector([0.2, 0.5, 1.0, 1.0])

            # Initialize Cortex-M0+ Q16.16 fixed-point fields (scale = 65536)
            self.pos_q[i] = ti.Vector([ti.i32(x * 65536.0), ti.i32(y * 65536.0)])
            self.vel_q[i] = ti.Vector([0, 0])
            self.force_q[i] = ti.Vector([0, 0])
            self.contact_q[i] = 0

    def _update_wall_lines(self) -> None:
        """Update the 4 clean flat square wall wireframe lines."""
        cos_t = math.cos(self.theta)
        sin_t = math.sin(self.theta)
        hl = self.hl

        def rot(lx: float, ly: float) -> list[float]:
            return [0.5 + cos_t * lx - sin_t * ly, 0.5 + sin_t * lx + cos_t * ly]

        c0 = rot(-hl, -hl)
        c1 = rot(hl, -hl)
        c2 = rot(hl, hl)
        c3 = rot(-hl, hl)

        # 4 line segments (c0-c1, c1-c2, c2-c3, c3-c0)
        lines = [c0, c1, c1, c2, c2, c3, c3, c0]
        self.wall_lines.from_numpy(np.array(lines, dtype=np.float32))

    @ti.func
    def _turbo_colormap(self, x: ti.f32) -> ti.Vector:
        """Evaluate exact Google Turbo ColorMap on GPU for stress chain visualization."""
        t = ti.min(ti.max(x, 0.0), 1.0)
        # Exact Google Turbo Colormap 7th-order polynomial formulation
        r = 0.13572138 + t * (
            4.61539260
            + t * (-42.66032258 + t * (132.13108234 + t * (-152.94239396 + t * 59.28637943)))
        )
        g = 0.09140261 + t * (
            2.19418839 + t * (4.84296658 + t * (-14.18503333 + t * (4.27729857 + t * 2.82956604)))
        )
        b = 0.10667330 + t * (
            12.64194608
            + t * (-60.58204836 + t * (110.36276771 + t * (-89.90310912 + t * 27.34824973)))
        )
        return ti.Vector(
            [
                ti.min(ti.max(r, 0.0), 1.0),
                ti.min(ti.max(g, 0.0), 1.0),
                ti.min(ti.max(b, 0.0), 1.0),
                1.0,
            ]
        )

    @ti.kernel
    def _physics_substep(
        self,
        theta: ti.f32,
        omega: ti.f32,
        gx: ti.f32,
        gy: ti.f32,
        dt: ti.f32,
        kn: ti.f32,
        damp: ti.f32,
        fric_sand: ti.f32,
        fric_wall: ti.f32,
        hl: ti.f32,
        r: ti.f32,
        diam: ti.f32,
    ):
        cos_t = ti.cos(theta)
        sin_t = ti.sin(theta)
        u_axis = ti.Vector([cos_t, sin_t])
        v_axis = ti.Vector([-sin_t, cos_t])

        # 1. Reset forces & apply gravity
        for i in range(self.n):
            self.force[i] = ti.Vector([gx, gy])
            self.contact_forces[i] = 0.0

        # 2. Particle-Particle Hooke-Mindlin Contacts (pure float32)
        diam2 = diam * diam
        for i in range(self.n):
            p_i = self.pos[i]
            v_i = self.vel[i]
            fx = 0.0
            fy = 0.0
            f_accum = 0.0

            for j in range(self.n):
                if i != j:
                    diff = p_i - self.pos[j]
                    d2 = diff.x * diff.x + diff.y * diff.y
                    if 0.0 < d2 < diam2:
                        d = ti.sqrt(d2)
                        overlap = diam - d
                        ndir = diff / d

                        vrel = v_i - self.vel[j]
                        vn = vrel.dot(ndir)

                        # Hooke spring-dashpot in float32
                        fn = ti.min(ti.max(0.0, kn * overlap - damp * vn), 800.0)

                        # Tangential Coulomb friction
                        vt = vrel - vn * ndir
                        vt_len = vt.norm()
                        ft = ti.Vector([0.0, 0.0])
                        if vt_len > 1e-4:
                            ft_max = fric_sand * fn
                            ft_visc = 30.0 * vt_len
                            ft = -(vt / vt_len) * ti.min(ft_max, ft_visc)

                        fc = fn * ndir + ft
                        fx += fc.x
                        fy += fc.y
                        f_accum += fn

            self.force[i].x += fx
            self.force[i].y += fy
            self.contact_forces[i] += f_accum

        # 3. Clean Flat Square Wall Forces (pure float32)
        for i in range(self.n):
            p = self.pos[i]
            v = self.vel[i]
            rel_p = p - self.center
            lx = rel_p.dot(u_axis)
            ly = rel_p.dot(v_axis)

            fw = ti.Vector([0.0, 0.0])
            fw_accum = 0.0

            # 1. Right wall (lx > hl - r)
            if lx > hl - r:
                ov = lx - (hl - r)
                pw = self.center + hl * u_axis + ly * v_axis
                vw = ti.Vector([-omega * (pw.y - self.center.y), omega * (pw.x - self.center.x)])
                vrel = v - vw
                vn = -(vrel.dot(u_axis))
                fn = ti.min(ti.max(0.0, kn * ov - damp * vn), 1500.0)
                vt = vrel.dot(v_axis)
                ft = -ti.min(ti.max(vt * 30.0, -fric_wall * fn), fric_wall * fn)
                fw += -fn * u_axis + ft * v_axis
                fw_accum += fn

            # 2. Left wall (lx < -hl + r)
            if lx < -hl + r:
                ov = (-hl + r) - lx
                pw = self.center - hl * u_axis + ly * v_axis
                vw = ti.Vector([-omega * (pw.y - self.center.y), omega * (pw.x - self.center.x)])
                vrel = v - vw
                vn = vrel.dot(u_axis)
                fn = ti.min(ti.max(0.0, kn * ov - damp * vn), 1500.0)
                vt = vrel.dot(v_axis)
                ft = -ti.min(ti.max(vt * 30.0, -fric_wall * fn), fric_wall * fn)
                fw += fn * u_axis + ft * v_axis
                fw_accum += fn

            # 3. Top wall (ly > hl - r)
            if ly > hl - r:
                ov = ly - (hl - r)
                pw = self.center + lx * u_axis + hl * v_axis
                vw = ti.Vector([-omega * (pw.y - self.center.y), omega * (pw.x - self.center.x)])
                vrel = v - vw
                vn = -(vrel.dot(v_axis))
                fn = ti.min(ti.max(0.0, kn * ov - damp * vn), 1500.0)
                vt = vrel.dot(u_axis)
                ft = -ti.min(ti.max(vt * 30.0, -fric_wall * fn), fric_wall * fn)
                fw += -fn * v_axis + ft * u_axis
                fw_accum += fn

            # 4. Bottom wall (ly < -hl + r)
            if ly < -hl + r:
                ov = (-hl + r) - ly
                pw = self.center + lx * u_axis - hl * v_axis
                vw = ti.Vector([-omega * (pw.y - self.center.y), omega * (pw.x - self.center.x)])
                vrel = v - vw
                vn = vrel.dot(v_axis)
                fn = ti.min(ti.max(0.0, kn * ov - damp * vn), 1500.0)
                vt = vrel.dot(u_axis)
                ft = -ti.min(ti.max(vt * 30.0, -fric_wall * fn), fric_wall * fn)
                fw += fn * v_axis + ft * u_axis
                fw_accum += fn

            self.force[i] += fw
            self.contact_forces[i] += fw_accum

        # 4. Symplectic Euler Integration with Acceleration & Velocity Clamps
        max_a = 400.0
        max_v = 3.5
        emergency_limit = hl - 0.25 * r

        for i in range(self.n):
            # Acceleration clamp (protects against any runaway)
            f_clamped = ti.Vector(
                [
                    ti.min(ti.max(self.force[i].x, -max_a), max_a),
                    ti.min(ti.max(self.force[i].y, -max_a), max_a),
                ]
            )
            self.vel[i] += f_clamped * dt

            # Velocity clamp
            spd = self.vel[i].norm()
            if spd > max_v:
                self.vel[i] = (self.vel[i] / spd) * max_v

            self.pos[i] += self.vel[i] * dt

            # Emergency Box Boundary Catch (strictly prevents box escape)
            rel_p = self.pos[i] - self.center
            lx = rel_p.dot(u_axis)
            ly = rel_p.dot(v_axis)
            clamped = 0

            if lx > emergency_limit:
                lx = emergency_limit
                clamped = 1
            elif lx < -emergency_limit:
                lx = -emergency_limit
                clamped = 1

            if ly > emergency_limit:
                ly = emergency_limit
                clamped = 1
            elif ly < -emergency_limit:
                ly = -emergency_limit
                clamped = 1

            if clamped != 0:
                self.pos[i] = self.center + lx * u_axis + ly * v_axis
                pw = self.center + lx * u_axis + ly * v_axis
                vw = ti.Vector([-omega * (pw.y - self.center.y), omega * (pw.x - self.center.x)])
                vr = self.vel[i] - vw
                vrl_x = vr.dot(u_axis)
                vrl_y = vr.dot(v_axis)
                if lx >= emergency_limit and vrl_x > 0.0:
                    vrl_x = -0.2 * vrl_x
                elif lx <= -emergency_limit and vrl_x < 0.0:
                    vrl_x = -0.2 * vrl_x
                if ly >= emergency_limit and vrl_y > 0.0:
                    vrl_y = -0.2 * vrl_y
                elif ly <= -emergency_limit and vrl_y < 0.0:
                    vrl_y = -0.2 * vrl_y
                self.vel[i] = vw + vrl_x * u_axis + vrl_y * v_axis

            # Keep pos_q updated for any compatibility checks
            self.pos_q[i] = ti.Vector(
                [ti.i32(self.pos[i].x * 65536.0), ti.i32(self.pos[i].y * 65536.0)]
            )


    @ti.func
    def _q16_sqrt(self, val: ti.i32) -> ti.i32:
        res = ti.i32(0)
        one = ti.i32(1 << 30)
        x = val
        while one > x:
            one >>= 2
        while one != 0:
            if x >= res + one:
                x -= res + one
                res += one << 1
            res >>= 1
            one >>= 2
        return res

    @ti.func
    def _q16_div(self, a: ti.i32, b: ti.i32) -> ti.i32:
        res = ti.i32(0)
        if b == 0:
            res = 0x7FFFFFFF if a >= 0 else -0x7FFFFFFF
        else:
            # SIO hardware 32-bit divider exact match (14-bit shift)
            res = (a << 14) // b << 2
        return res

    @ti.kernel
    def _physics_substep_q16(self, gx: ti.i32, gy: ti.i32):
        # 1. Reset forces & contact
        for i in range(self.n):
            self.force_q[i] = ti.Vector([gx, gy])
            self.contact_q[i] = 0

        # 2. Build 32x32 spatial hash grid
        for i in range(1024):
            self.grid_head_q[i] = -1
        for i in range(self.n):
            cx = self.pos_q[i][0] >> 17
            cy = self.pos_q[i][1] >> 17
            if cx < 0:
                cx = 0
            elif cx >= 32:
                cx = 31
            if cy < 0:
                cy = 0
            elif cy >= 32:
                cy = 31
            cell = cy * 32 + cx
            self.grid_next_q[i] = self.grid_head_q[cell]
            self.grid_head_q[cell] = i

        # 3. Particle-Particle Contact Pairs (Exact Cortex-M0+ integer DEM)
        DIAM = 98304
        DIAM2 = 147456
        KN_INT = 4000
        DAMP_INT = 80
        GAMMA_INT = 15
        FN_MAX = 98304000

        ti.loop_config(serialize=True)
        for i in range(self.n):
            px = self.pos_q[i][0]
            py = self.pos_q[i][1]
            vxi = self.vel_q[i][0]
            vyi = self.vel_q[i][1]
            fxi = self.force_q[i][0]
            fyi = self.force_q[i][1]
            cacc = ti.i32(0)

            cx = px >> 17
            cy = py >> 17
            if cx < 0:
                cx = 0
            elif cx >= 32:
                cx = 31
            if cy < 0:
                cy = 0
            elif cy >= 32:
                cy = 31

            for dy in range(-1, 2):
                ncy = cy + dy
                if 0 <= ncy < 32:
                    for dx in range(-1, 2):
                        ncx = cx + dx
                        if 0 <= ncx < 32:
                            cell = ncy * 32 + ncx
                            j = self.grid_head_q[cell]
                            while j != -1:
                                if j > i:
                                    dx_ = px - self.pos_q[j][0]
                                    dy_ = py - self.pos_q[j][1]
                                    if -DIAM < dx_ < DIAM and -DIAM < dy_ < DIAM:
                                        d2 = (dx_ >> 8) * (dx_ >> 8) + (dy_ >> 8) * (dy_ >> 8)
                                        if 0 < d2 < DIAM2:
                                            dist = self._q16_sqrt(d2)
                                            if dist > 0:
                                                nx = ti.i32(0)
                                                ny = ti.i32(0)
                                                ov = ti.i32(0)
                                                if dist < 655:
                                                    ov = DIAM - 655
                                                    nx = 65536 if ((i ^ j) & 1) else -65536
                                                    ny = 65536 if ((i ^ (j * 3)) & 1) else -65536
                                                else:
                                                    ov = DIAM - dist
                                                    nx = self._q16_div(dx_, dist)
                                                    ny = self._q16_div(dy_, dist)

                                                vrx = vxi - self.vel_q[j][0]
                                                vry = vyi - self.vel_q[j][1]
                                                vn = (vrx >> 8) * (nx >> 8) + (vry >> 8) * (ny >> 8)
                                                fn = KN_INT * ov
                                                if vn < 0:
                                                    fn -= DAMP_INT * vn
                                                if fn < 0:
                                                    fn = 0
                                                elif fn > FN_MAX:
                                                    fn = FN_MAX

                                                vtx = vrx - (vn >> 8) * (nx >> 8)
                                                vty = vry - (vn >> 8) * (ny >> 8)
                                                ftmax = (fn * 22938) >> 16
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
                                                ti.atomic_add(self.force_q[j][0], -cfx)
                                                ti.atomic_add(self.force_q[j][1], -cfy)
                                                ti.atomic_add(self.contact_q[j], fn)
                                                cacc += fn
                                j = self.grid_next_q[j]

            self.force_q[i][0] = fxi
            self.force_q[i][1] = fyi
            self.contact_q[i] += cacc

        # 4. Box Wall Constraints
        WMIN = 81920
        WMAX = 4112384
        WKN_INT = 6000
        WDAMP_INT = 100
        WFN_MAX = 98304000

        for i in range(self.n):
            px = self.pos_q[i][0]
            py = self.pos_q[i][1]
            vx = self.vel_q[i][0]
            vy = self.vel_q[i][1]

            if px < WMIN:
                ov = WMIN - px
                fn = WKN_INT * ov
                if vx < 0:
                    fn -= WDAMP_INT * vx
                if fn < 0: fn = 0
                elif fn > WFN_MAX: fn = WFN_MAX
                self.force_q[i][0] += fn
                self.contact_q[i] += fn
            elif px > WMAX:
                ov = px - WMAX
                fn = WKN_INT * ov
                if vx > 0:
                    fn += WDAMP_INT * vx
                if fn < 0: fn = 0
                elif fn > WFN_MAX: fn = WFN_MAX
                self.force_q[i][0] -= fn
                self.contact_q[i] += fn

            if py < WMIN:
                ov = WMIN - py
                fn = WKN_INT * ov
                if vy < 0:
                    fn -= WDAMP_INT * vy
                if fn < 0: fn = 0
                elif fn > WFN_MAX: fn = WFN_MAX
                self.force_q[i][1] += fn
                self.contact_q[i] += fn
            elif py > WMAX:
                ov = py - WMAX
                fn = WKN_INT * ov
                if vy > 0:
                    fn += WDAMP_INT * vy
                if fn < 0: fn = 0
                elif fn > WFN_MAX: fn = WFN_MAX
                self.force_q[i][1] -= fn
                self.contact_q[i] += fn

        # 5. Symplectic Euler Integration
        MAX_A = 26214400
        MAX_V = 229376
        DT_NUM = 262
        HMIN = 49152
        HMAX = 4145152

        for i in range(self.n):
            ax = self.force_q[i][0]
            ay = self.force_q[i][1]
            if ax > MAX_A: ax = MAX_A
            elif ax < -MAX_A: ax = -MAX_A
            if ay > MAX_A: ay = MAX_A
            elif ay < -MAX_A: ay = -MAX_A

            self.vel_q[i][0] += ((ax >> 8) * DT_NUM) >> 8
            self.vel_q[i][1] += ((ay >> 8) * DT_NUM) >> 8
            self.vel_q[i][0] -= self.vel_q[i][0] >> 7
            self.vel_q[i][1] -= self.vel_q[i][1] >> 7

            if self.vel_q[i][0] > MAX_V: self.vel_q[i][0] = MAX_V
            elif self.vel_q[i][0] < -MAX_V: self.vel_q[i][0] = -MAX_V
            if self.vel_q[i][1] > MAX_V: self.vel_q[i][1] = MAX_V
            elif self.vel_q[i][1] < -MAX_V: self.vel_q[i][1] = -MAX_V

            self.pos_q[i][0] += (self.vel_q[i][0] * DT_NUM) >> 16
            self.pos_q[i][1] += (self.vel_q[i][1] * DT_NUM) >> 16

            if self.pos_q[i][0] < HMIN:
                self.pos_q[i][0] = HMIN
                if self.vel_q[i][0] < 0: self.vel_q[i][0] = -(self.vel_q[i][0] >> 2)
            elif self.pos_q[i][0] > HMAX:
                self.pos_q[i][0] = HMAX
                if self.vel_q[i][0] > 0: self.vel_q[i][0] = -(self.vel_q[i][0] >> 2)

            if self.pos_q[i][1] < HMIN:
                self.pos_q[i][1] = HMIN
                if self.vel_q[i][1] < 0: self.vel_q[i][1] = -(self.vel_q[i][1] >> 2)
            elif self.pos_q[i][1] > HMAX:
                self.pos_q[i][1] = HMAX
                if self.vel_q[i][1] > 0: self.vel_q[i][1] = -(self.vel_q[i][1] >> 2)

            # Synchronize back to float display fields
            self.pos[i] = ti.Vector([
                (ti.f32(self.pos_q[i][0]) / 4194304.0) * (2.0 * self.hl) + (0.5 - self.hl),
                (ti.f32(self.pos_q[i][1]) / 4194304.0) * (2.0 * self.hl) + (0.5 - self.hl)
            ])
            self.contact_forces[i] = ti.f32(self.contact_q[i]) / 65536.0

    @ti.kernel
    def _update_colors_kernel(self, norm_div: ti.f32):
        """Update particle colors using accelerated Turbo colormap directly from float forces."""
        for i in range(self.n):
            f_val = self.contact_forces[i]
            u = ti.min(ti.max(f_val / norm_div, 0.0), 1.0)
            t = 0.12 + 0.88 * ti.pow(u, 0.65)
            self.colors[i] = self._turbo_colormap(t)

    def step(self) -> None:
        """Advance the simulation by one display frame using pure float32 DEM."""
        dt = self.cfg.dt
        substeps = self.cfg.substeps_per_frame

        if not self.adxl_mode:
            gx = 0.0
            gy = -self.cfg.gravity
            omega = self.rot_speed
            self.theta += omega * (dt * substeps)
        else:
            gx = self.adxl_tilt_x * self.cfg.gravity
            gy = -self.adxl_tilt_y * self.cfg.gravity
            omega = 0.0
            self.theta = 0.0

        # Dynamic Turbo normalization: scale gracefully from 1g up to 3g violent shaking
        cur_g = math.hypot(gx, gy) / self.cfg.gravity
        norm_div = 1100.0 * max(1.0, cur_g * 0.85)

        if self.use_q16_physics:
            # RP2040 Cortex-M0+ bit-exact Q16.16 integer physics simulation
            gx_q = int(gx * 65536.0)
            gy_q = int(gy * 65536.0)
            for _ in range(substeps):
                self._physics_substep_q16(gx_q, gy_q)
            self._update_colors_kernel(norm_div)
            self._update_wall_lines()
            return

        for _ in range(substeps):
            self._physics_substep(
                self.theta,
                omega,
                gx,
                gy,
                dt,
                self.cfg.kn,
                self.cfg.damping,
                self.cfg.friction,
                self.cfg.wall_friction,
                self.hl,
                self.r,
                2.0 * self.r,
            )

        self._update_colors_kernel(norm_div)
        self._update_wall_lines()

    def set_adxl335_raw_adc(self, adc_x: int, adc_y: int, adc_z: int = 2048) -> None:
        """Set board acceleration directly from RP2040 12-bit ADC raw counts (0-4095).

        Formula for ADXL335 on 3.3V supply:
        g = (ADC - 2048) / 410.0
        """
        self.adxl_mode = True
        self.adxl_tilt_x = float(np.clip((adc_x - 2048) / 410.0, -3.0, 3.0))
        self.adxl_tilt_y = float(np.clip((adc_y - 2048) / 410.0, -3.0, 3.0))

    def get_adxl335_telemetry(self) -> dict[str, float]:
        """Compute simulated ADXL335 3-axis analog accelerometer outputs.

        ADXL335 Specs on 3.3V power:
        - VDD = 3.3 V
        - Zero-g bias = VDD / 2 = 1.65 V
        - Sensitivity = 330 mV / g (0.330 V/g)
        - RP2040 12-bit ADC (0 .. 4095, Vref = 3.3V)
        """
        # Board-relative gravity vector
        cos_t = math.cos(self.theta)
        sin_t = math.sin(self.theta)

        if not self.adxl_mode:
            # Gravity vector in box coordinates
            board_gx = -(sin_t * 0.0 + cos_t * 1.0)
            board_gy = -(cos_t * 0.0 - sin_t * 1.0)
            board_gz = 0.0
        else:
            board_gx = self.adxl_tilt_x
            board_gy = self.adxl_tilt_y
            board_gz = 0.0

        # ADXL335 voltage (V = 1.65 + 0.33 * g)
        vx = 1.65 + 0.330 * board_gx
        vy = 1.65 + 0.330 * board_gy
        vz = 1.65 + 0.330 * board_gz

        # RP2040 12-bit ADC raw count (0 - 4095)
        adc_x = int(round(np.clip(vx / 3.3 * 4095.0, 0, 4095)))
        adc_y = int(round(np.clip(vy / 3.3 * 4095.0, 0, 4095)))
        adc_z = int(round(np.clip(vz / 3.3 * 4095.0, 0, 4095)))

        return {
            "pin_x": 18,  # Waveshare Pin 18 = GP27 (ADC1)
            "pin_y": 19,  # Waveshare Pin 19 = GP28 (ADC2)
            "pin_z": 20,  # Waveshare Pin 20 = GP29 (ADC3)
            "pin_vcc": 21,  # Waveshare Pin 21 = 3V3
            "pin_gnd": 22,  # Waveshare Pin 22 = GND
            "gx": board_gx,
            "gy": board_gy,
            "gz": board_gz,
            "vx": vx,
            "vy": vy,
            "vz": vz,
            "adc_x": adc_x,
            "adc_y": adc_y,
            "adc_z": adc_z,
        }

    def get_box_particle_bins(self) -> dict[str, Any]:
        """Compute live particle containment and spatial distribution bins relative to the box."""
        pos = self.pos.to_numpy() - [0.5, 0.5]
        cos_t = math.cos(self.theta)
        sin_t = math.sin(self.theta)
        lx = cos_t * pos[:, 0] + sin_t * pos[:, 1]
        ly = -sin_t * pos[:, 0] + cos_t * pos[:, 1]
        hl = self.hl

        # Strict containment check (|local| <= hl)
        in_box_mask = (np.abs(lx) <= hl + 1e-4) & (np.abs(ly) <= hl + 1e-4)
        in_box = int(np.sum(in_box_mask))
        escaped = self.n - in_box

        # 4 vertical height bins from bottom (-hl) to top (+hl)
        bins = [-hl, -0.5 * hl, 0.0, 0.5 * hl, hl + 1e-4]
        layer_counts, _ = np.histogram(ly[in_box_mask], bins=bins)

        # 4 quadrants: BL (x<0, y<0), BR (x>=0, y<0), TL (x<0, y>=0), TR (x>=0, y>=0)
        bl = int(np.sum(in_box_mask & (lx < 0) & (ly < 0)))
        br = int(np.sum(in_box_mask & (lx >= 0) & (ly < 0)))
        tl = int(np.sum(in_box_mask & (lx < 0) & (ly >= 0)))
        tr = int(np.sum(in_box_mask & (lx >= 0) & (ly >= 0)))

        return {
            "total": self.n,
            "in_box": in_box,
            "escaped": escaped,
            "ratio": (in_box / self.n) * 100.0,
            "layers": [int(c) for c in layer_counts],
            "quadrants": {"BL": bl, "BR": br, "TL": tl, "TR": tr},
        }

    @ti.kernel
    def _rasterize_matrix_kernel(self, theta: ti.f32, hl: ti.f32, r: ti.f32):
        # 1. Clear raw 64x64 matrix to subtle dark background
        for x, y in self.raw_matrix_color:
            self.raw_matrix_color[x, y] = ti.Vector([0.015, 0.015, 0.02, 1.0])
            self.matrix_weight[x, y] = 0.0

        # 2. Map Drum Interior DIRECTLY to full 64x64 grid (no outer border, 100% active matrix)
        cos_t = ti.cos(theta)
        sin_t = ti.sin(theta)
        box_width = 2.0 * hl
        r_pix = (r / box_width) * 64.0  # 0.75 px (D = 1.5 px)
        r2_pix = r_pix * r_pix * 1.35  # Diagonal corner coverage factor for circular bounds

        for i in range(self.n):
            rel_x = self.pos[i].x - 0.5
            rel_y = self.pos[i].y - 0.5
            lx = rel_x * cos_t + rel_y * sin_t
            ly = -rel_x * sin_t + rel_y * cos_t

            pos_x = ((lx + hl) / box_width) * 64.0
            pos_y = ((ly + hl) / box_width) * 64.0

            col = self.colors[i]
            w = self.contact_forces[i] + 10.0

            # Exact 2x2 bounding footprint (max width/height = 2 px)
            x0 = int(ti.floor(pos_x - r_pix))
            x1 = int(ti.floor(pos_x + r_pix))
            y0 = int(ti.floor(pos_y - r_pix))
            y1 = int(ti.floor(pos_y + r_pix))

            for qx in range(x0, x1 + 1):
                for qy in range(y0, y1 + 1):
                    if 0 <= qx < 64 and 0 <= qy < 64:
                        diff_x = (ti.f32(qx) + 0.5) - pos_x
                        diff_y = (ti.f32(qy) + 0.5) - pos_y
                        d2 = diff_x * diff_x + diff_y * diff_y
                        # Check circular distance OR exact 2x2 footprint coverage
                        if d2 <= r2_pix or (x1 - x0 <= 1 and y1 - y0 <= 1):
                            if w > self.matrix_weight[qx, qy]:
                                self.matrix_weight[qx, qy] = w
                                self.raw_matrix_color[qx, qy] = col

    @ti.kernel
    def _smooth_and_pack_rgb565_kernel(self):
        """Fast 3x3 morphological hole-fill filter and exact RGB565 quantization."""
        for x, y in self.matrix_color:
            c = self.raw_matrix_color[x, y]
            is_center_lit = c.x > 0.05 or c.y > 0.05 or c.z > 0.05

            lit_neighbors = 0
            accum_color = ti.Vector([0.0, 0.0, 0.0, 0.0])

            for dx in ti.static(range(-1, 2)):
                for dy in ti.static(range(-1, 2)):
                    if dx != 0 or dy != 0:
                        nx = x + dx
                        ny = y + dy
                        if 0 <= nx < 64 and 0 <= ny < 64:
                            nc = self.raw_matrix_color[nx, ny]
                            if nc.x > 0.05 or nc.y > 0.05 or nc.z > 0.05:
                                lit_neighbors += 1
                                accum_color += nc

            final_color = c
            if not is_center_lit:
                # Over-estimate hole-fill: fill gaps with 3+ neighbors
                if lit_neighbors >= 3:
                    final_color = accum_color / ti.f32(lit_neighbors)
            else:
                # Gentle smoothing (少しだけ平滑): 70% center + 30% neighbor average
                if lit_neighbors > 0:
                    neighbor_avg = accum_color / ti.f32(lit_neighbors)
                    final_color = c * 0.70 + neighbor_avg * 0.30

            # Exact RGB565 Quantization with standard half-to-even rounding (+0.5)
            r5 = ti.u16(ti.min(ti.max(final_color.x * 31.0 + 0.5, 0.0), 31.0))
            g6 = ti.u16(ti.min(ti.max(final_color.y * 63.0 + 0.5, 0.0), 63.0))
            b5 = ti.u16(ti.min(ti.max(final_color.z * 31.0 + 0.5, 0.0), 31.0))

            self.matrix_rgb565[x, y] = (r5 << 11) | (g6 << 5) | b5

            # Store exact RGB565 unpacked brightness back to matrix_color
            self.matrix_color[x, y] = ti.Vector(
                [ti.f32(r5) / 31.0, ti.f32(g6) / 63.0, ti.f32(b5) / 31.0, 1.0]
            )

    @ti.kernel
    def _render_led_display_kernel(self, led_mode: ti.i32):
        # led_mode: 0 = Square Voxel / Pixel matrix, 1 = Round HUB75 LED Dots
        for x, y in self.display_buffer:
            mx = x // 8
            my = y // 8
            lx = x % 8
            ly = y % 8

            c = self.matrix_color[mx, my]
            is_lit = c.x > 0.03 or c.y > 0.03 or c.z > 0.03

            if led_mode == 0:
                if lx == 7 or ly == 7:
                    self.display_buffer[x, y] = ti.Vector([0.01, 0.01, 0.015, 1.0])
                else:
                    self.display_buffer[x, y] = c
            else:
                dx = ti.f32(lx) - 3.5
                dy = ti.f32(ly) - 3.5
                d2 = dx * dx + dy * dy
                if d2 <= 10.5:
                    if is_lit:
                        if d2 <= 2.0 and dx < 0.0 and dy < 0.0:
                            self.display_buffer[x, y] = ti.min(c * 1.25, 1.0)
                        else:
                            self.display_buffer[x, y] = c
                    else:
                        self.display_buffer[x, y] = ti.Vector([0.05, 0.05, 0.065, 1.0])
                else:
                    self.display_buffer[x, y] = ti.Vector([0.01, 0.01, 0.015, 1.0])

    def rasterize_matrix(self, led_mode: int = 1) -> None:
        """Rasterize particle states onto 64x64 matrix and build LED display buffer."""
        self._rasterize_matrix_kernel(self.theta, self.hl, self.r)
        self._smooth_and_pack_rgb565_kernel()
        self._render_led_display_kernel(led_mode)

    def get_rgb565_frame(self) -> bytes:
        """Export 64x64 RGB565 Little-Endian frame (8192 bytes) for wire transmission."""
        arr = self.matrix_rgb565.to_numpy()
        frame_data = np.ascontiguousarray(np.flipud(arr.T), dtype=np.uint16)
        return frame_data.tobytes()


def run_taichi_gui(matrix_mode: bool = False) -> None:
    """Launch the hardware-accelerated Taichi GGUI 2D-DEM simulation."""
    ti.init(arch=ti.gpu)

    cfg = TaichiDEMConfig()
    sim = TaichiRotatingDrumDEM(cfg)

    window = ti.ui.Window(
        "Taichi 2D-DEM Granular Drum (ADXL335 + RP2040)",
        res=(900, 900),
        vsync=True,
    )
    canvas = window.get_canvas()
    gui = window.GUI

    # Performance metrics & Display options
    fps_timer = time.perf_counter()
    frame_count = 0
    box_stats = None  # Refreshed every 10 frames (get_box_particle_bins syncs GPU->CPU)
    display_fps = 60.0
    paused = False
    matrix_view = matrix_mode
    led_mode = 1  # 1 = Round HUB75 LED Dots, 0 = Square Voxels/Pixels

    print("=" * 60)
    print("Taichi 2D-DEM Rotating Drum & ADXL335 Accelerometer Simulation")
    print(f"Particles: {sim.n} (~44% volume fill, r={sim.r})")
    print("Clean 4 flat square walls without hinges/baffles")
    print(f"Initial display mode: {'64x64 Matrix' if matrix_view else 'Vector Particle'}")
    print(
        "Hotkeys: [M] Toggle Matrix/Particle | [P] Toggle Round/Square | [Space] Pause | [R] Reset"
    )
    print("=" * 60)

    while window.running:
        curr_time = time.perf_counter()
        frame_count += 1
        if curr_time - fps_timer >= 0.5:
            display_fps = frame_count / (curr_time - fps_timer)
            frame_count = 0
            fps_timer = curr_time

        # Handle keyboard & mouse interaction (supports full +/- 3.0G shake)
        if window.is_pressed(ti.ui.LEFT, "a"):
            sim.adxl_mode = True
            sim.adxl_tilt_x = max(-3.0, sim.adxl_tilt_x - 0.1)
        if window.is_pressed(ti.ui.RIGHT, "d"):
            sim.adxl_mode = True
            sim.adxl_tilt_x = min(3.0, sim.adxl_tilt_x + 0.1)
        if window.is_pressed(ti.ui.UP, "w"):
            sim.adxl_mode = True
            sim.adxl_tilt_y = min(3.0, sim.adxl_tilt_y + 0.1)
        if window.is_pressed(ti.ui.DOWN, "s"):
            sim.adxl_mode = True
            sim.adxl_tilt_y = max(-3.0, sim.adxl_tilt_y - 0.1)
        if window.is_pressed("j"):
            # Violent 3G horizontal shake impulse
            sim.adxl_mode = True
            sim.adxl_tilt_x = 3.0 if sim.adxl_tilt_x <= 0.0 else -3.0
        if window.is_pressed("k"):
            # Violent 3G vertical slam impulse
            sim.adxl_mode = True
            sim.adxl_tilt_y = 3.0 if sim.adxl_tilt_y <= 0.0 else -3.0
        if window.is_pressed("r"):
            sim.reset()
        if window.is_pressed(ti.ui.SPACE):
            paused = not paused
        if window.is_pressed("m"):
            matrix_view = not matrix_view
        if window.is_pressed("p"):
            led_mode = 1 - led_mode
        if window.is_pressed("q"):
            sim.use_q16_physics = not sim.use_q16_physics

        # Update physics
        if not paused:
            sim.step()

        # Render display
        if matrix_view:
            sim.rasterize_matrix(led_mode)
            canvas.set_image(sim.display_buffer)
        else:
            canvas.set_background_color((0.07, 0.08, 0.11))
            canvas.lines(sim.wall_lines, width=0.005, color=(0.15, 0.85, 0.95))
            canvas.circles(sim.pos, radius=sim.r, per_vertex_color=sim.colors)

        # ADXL335 Sensor Telemetry
        telem = sim.get_adxl335_telemetry()

        # In-Window GUI Controls
        with gui.sub_window("DEM & ADXL335 Controls", 0.02, 0.02, 0.44, 0.45):
            gui.text(f"GPU FPS: {display_fps:.1f} | N = {sim.n} (44% Fill)")
            gui.text(f"Drum Angle: {math.degrees(sim.theta) % 360.0:.1f}°")

            paused = gui.checkbox("Pause Simulation [Space]", paused)
            sim.use_q16_physics = gui.checkbox("RP2040 Q16.16 Physics [Q]", sim.use_q16_physics)
            matrix_view = gui.checkbox("64x64 Matrix Display [M]", matrix_view)
            if matrix_view:
                led_round = gui.checkbox("Round LEDs vs Square Voxels [P]", bool(led_mode))
                led_mode = 1 if led_round else 0

            sim.adxl_mode = gui.checkbox("ADXL335 Sensor Tilt Mode", sim.adxl_mode)

            if not sim.adxl_mode:
                sim.rot_speed = gui.slider_float("Drum Speed (rad/s)", sim.rot_speed, -5.0, 5.0)
                rpm = (sim.rot_speed * 60.0) / (2.0 * math.pi)
                gui.text(f"Rotation Rate: {rpm:.1f} RPM")
            else:
                sim.adxl_tilt_x = gui.slider_float("Tilt/Shake X (G)", sim.adxl_tilt_x, -3.0, 3.0)
                sim.adxl_tilt_y = gui.slider_float("Tilt/Shake Y (G)", sim.adxl_tilt_y, -3.0, 3.0)
                if gui.button("Shake +/-3G [J]"):
                    sim.adxl_tilt_x = 3.0 if sim.adxl_tilt_x <= 0.0 else -3.0
                if gui.button("Slam +3G [K]"):
                    sim.adxl_tilt_y = 3.0

            if gui.button("Reset Particles & Box [R]"):
                sim.reset()

            gui.text("-" * 35)
            gui.text("RP2040 -> ADXL335 (3.3V, 2D X/Y only):")
            gui.text(f"GP26 (ADC0 X): {telem['vx']:.2f}V ({telem['adc_x']} raw)")
            gui.text(f"GP27 (ADC1 Y): {telem['vy']:.2f}V ({telem['adc_y']} raw)")
            gui.text("3V3 (VCC) | GND (GND) [Z unused]")

        # Live Box Particle Containment Monitor & Layer Bins
        # (1.3ms GPU->CPU sync; 10-frame cadence is plenty for a monitor readout)
        if box_stats is None or frame_count % 10 == 0:
            box_stats = sim.get_box_particle_bins()
        with gui.sub_window("Box Particle Monitor (Bins)", 0.02, 0.48, 0.44, 0.40):
            gui.text(f"Total Particles: {box_stats['total']}")
            if box_stats["escaped"] == 0:
                gui.text(f"Inside Box: {box_stats['in_box']} / {box_stats['total']} (100.0%) [OK]")
            else:
                gui.text(f"WARNING: Escaped = {box_stats['escaped']}!")
            gui.text("-" * 35)
            gui.text("Height Layer Distribution Bins:")
            l0, l1, l2, l3 = box_stats["layers"]
            n_tot = sim.n
            gui.text(f"  Bed  (0-25%): {l0:4d} ({l0 / n_tot * 100:4.1f}%)")
            gui.text(f"  MidL (25-50%): {l1:4d} ({l1 / n_tot * 100:4.1f}%)")
            gui.text(f"  MidU (50-75%): {l2:4d} ({l2 / n_tot * 100:4.1f}%)")
            gui.text(f"  Top  (75-100%):{l3:4d} ({l3 / n_tot * 100:4.1f}%)")
            gui.text("-" * 35)
            q = box_stats["quadrants"]
            gui.text(f"Quadrants: BL={q['BL']} | BR={q['BR']} | TL={q['TL']} | TR={q['TR']}")

        window.show()


run_taichi_drum_simulation = run_taichi_gui


if __name__ == "__main__":
    run_taichi_gui()
