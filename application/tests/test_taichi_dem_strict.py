"""Strict Taichi reference (float32) regression tests for PopUp and wall sticking.

Q16 ファームウェアの正解系である float 参照 DEM そのものを先に green にする。
Pop = 非有限・ボックス脱出・速度発散。Sticking = 傾けても壁に残留。
"""

import numpy as np
import pytest
import taichi as ti

from led_matrix_controller.demo_taichi_dem import TaichiDEMConfig, TaichiRotatingDrumDEM


@pytest.fixture(scope="module")
def ti_init():
    try:
        ti.init(arch=ti.gpu)
    except Exception:
        ti.init(arch=ti.cpu)


def _make_settled(sim, frames=120):
    sim.adxl_mode = True
    sim.set_adxl335_raw_adc(2048, 2048 + 410)  # tilt_x=0, tilt_y=+1 (down)
    for _ in range(frames):
        sim.step()
    return sim


def test_reference_settles_down_without_popup(ti_init):
    cfg = TaichiDEMConfig(n_particles=200, substeps_per_frame=4, dt=0.001)
    sim = TaichiRotatingDrumDEM(cfg)
    _make_settled(sim)

    pos = sim.pos.to_numpy()
    vel = sim.vel.to_numpy()
    assert np.all(np.isfinite(pos)) and np.all(np.isfinite(vel)), "PopUp: non-finite state"
    stats = sim.get_box_particle_bins()
    assert stats["escaped"] == 0, f"PopUp: escaped={stats['escaped']}"
    # pile must rest in bottom half (box coords, center 0.5)
    assert pos[:, 1].mean() < 0.5, f"did not settle: mean_y={pos[:, 1].mean()}"
    # calm after settling: no lingering high speeds
    for _ in range(20):
        sim.step()
    vmax = np.linalg.norm(sim.vel.to_numpy(), axis=1).max()
    assert vmax < 2.0, f"PopUp/jitter: vmax={vmax}"


def test_reference_no_wall_stick_on_tilt(ti_init):
    cfg = TaichiDEMConfig(n_particles=200, substeps_per_frame=4, dt=0.001)
    sim = TaichiRotatingDrumDEM(cfg)
    _make_settled(sim)

    # tilt hard right: +1g on X, 0 on Y
    sim.set_adxl335_raw_adc(2048 + 410, 2048)
    for _ in range(120):
        sim.step()

    pos = sim.pos.to_numpy() - np.array([0.5, 0.5])
    hl = sim.hl
    n_left = int(np.sum(pos[:, 0] < -hl + 0.05))
    assert n_left == 0, f"wall stick: {n_left} particles remain near left wall"
    assert pos[:, 0].mean() > 0.0, "pile did not migrate right"


def test_reference_drum_mode_no_boiling(ti_init):
    """Drum rotation (default GUI --dem-matrix mode) must not boil.

    Regression: non-unit contact normals used to inject energy until ~1/3 of
    the bed exceeded 2.0 u/s with speeds pegged at the clamp. Needs N=1024:
    shallow piles never triggered it.
    """
    cfg = TaichiDEMConfig(n_particles=1024)  # default rot_speed/dt/substeps
    sim = TaichiRotatingDrumDEM(cfg)
    assert not sim.adxl_mode
    for _ in range(60):
        sim.step()

    vel = sim.vel.to_numpy()
    spd = np.linalg.norm(vel, axis=1)
    stats = sim.get_box_particle_bins()
    assert stats["escaped"] == 0, f"escaped={stats['escaped']}"
    assert spd.max() < 1.5, f"boiling: maxspd={spd.max()}"
    assert int((spd > 2.0).sum()) == 0, "boiling: particles faster than 2.0 u/s"


def test_reference_no_popup_on_violent_shake(ti_init):
    cfg = TaichiDEMConfig(n_particles=200, substeps_per_frame=4, dt=0.001)
    sim = TaichiRotatingDrumDEM(cfg)
    sim.adxl_mode = True
    shakes = [(2048 + 1230, 2048), (2048 - 1230, 2048), (2048, 2048 + 1230), (2048, 2048 - 1230)]
    for f in range(40):
        ax, ay = shakes[f % 4]
        sim.set_adxl335_raw_adc(ax, ay)
        sim.step()

    pos = sim.pos.to_numpy()
    vel = sim.vel.to_numpy()
    assert np.all(np.isfinite(pos)) and np.all(np.isfinite(vel)), "PopUp on shake"
    stats = sim.get_box_particle_bins()
    assert stats["escaped"] == 0, f"escaped on shake: {stats['escaped']}"
