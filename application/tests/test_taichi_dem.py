"""Test Taichi GPU DEM simulation components."""

import numpy as np
import pytest
import taichi as ti

from led_matrix_controller.demo_taichi_dem import TaichiDEMConfig, TaichiRotatingDrumDEM


@pytest.fixture(scope="module")
def ti_init():
    """Initialize Taichi on CPU or GPU for testing."""
    try:
        ti.init(arch=ti.gpu)
    except Exception:
        ti.init(arch=ti.cpu)


def test_taichi_dem_initialization(ti_init):
    """Test particle field allocation and initial placement."""
    cfg = TaichiDEMConfig(n_particles=200, box_half_len=0.36)
    sim = TaichiRotatingDrumDEM(cfg)

    assert sim.n == 200
    pos = sim.pos.to_numpy()
    assert pos.shape == (200, 2)
    # Check all particles are within the box bounds
    assert np.all(pos[:, 0] > (0.5 - 0.36))
    assert np.all(pos[:, 0] < (0.5 + 0.36))
    assert np.all(pos[:, 1] > (0.5 - 0.36))
    assert np.all(pos[:, 1] < (0.5 + 0.36))

    # Verify pure flat walls (4 walls = 8 line segment endpoints)
    walls = sim.wall_lines.to_numpy()
    assert walls.shape == (8, 2)


def test_taichi_dem_step(ti_init):
    """Test advancing simulation steps without NaN or explosion."""
    cfg = TaichiDEMConfig(n_particles=200, substeps_per_frame=4, dt=0.001)
    sim = TaichiRotatingDrumDEM(cfg)

    for _ in range(10):
        sim.step()

    pos = sim.pos.to_numpy()
    vel = sim.vel.to_numpy()
    forces = sim.contact_forces.to_numpy()

    # Check finite numbers (no NaN or inf)
    assert np.all(np.isfinite(pos))
    assert np.all(np.isfinite(vel))
    assert np.all(np.isfinite(forces))
    assert sim.theta > 0.0


def test_adxl335_sensor_telemetry(ti_init):
    """Test ADXL335 3.3V 3-axis accelerometer sensor modeling."""
    cfg = TaichiDEMConfig(n_particles=100)
    sim = TaichiRotatingDrumDEM(cfg)

    telem = sim.get_adxl335_telemetry()
    assert "vx" in telem and "vy" in telem and "vz" in telem
    assert "adc_x" in telem and "adc_y" in telem and "adc_z" in telem

    # 3.3V power: voltages must be between 0.0V and 3.3V
    assert 0.0 <= telem["vx"] <= 3.3
    assert 0.0 <= telem["vy"] <= 3.3
    assert 0.0 <= telem["vz"] <= 3.3

    # RP2040 12-bit ADC raw counts must be in 0..4095
    assert 0 <= telem["adc_x"] <= 4095
    assert 0 <= telem["adc_y"] <= 4095
    assert 0 <= telem["adc_z"] <= 4095


def test_adxl335_raw_adc_injection(ti_init):
    """Test injecting raw 12-bit ADC values to drive DEM gravity."""
    cfg = TaichiDEMConfig(n_particles=100)
    sim = TaichiRotatingDrumDEM(cfg)

    # Inject 0g: 2048 counts on both X and Y
    sim.set_adxl335_raw_adc(2048, 2048)
    assert abs(sim.adxl_tilt_x) < 1e-4
    assert abs(sim.adxl_tilt_y) < 1e-4

    # Inject +1g on X: 2048 + 410 = 2458 counts
    sim.set_adxl335_raw_adc(2458, 2048)
    assert abs(sim.adxl_tilt_x - 1.0) < 0.05
    assert abs(sim.adxl_tilt_y) < 1e-4

    # Run step with injected gravity
    sim.step()
    assert np.all(np.isfinite(sim.pos.to_numpy()))


def test_matrix_rasterization(ti_init):
    """Test 64x64 matrix voxel rasterization and RGB565 export."""
    cfg = TaichiDEMConfig(n_particles=200)
    sim = TaichiRotatingDrumDEM(cfg)

    # Step simulation
    for _ in range(5):
        sim.step()

    # Rasterize to 64x64 matrix in both LED modes
    sim.rasterize_matrix(led_mode=0)  # Square voxels
    sim.rasterize_matrix(led_mode=1)  # Round LEDs

    # Verify RGB565 export
    frame = sim.get_rgb565_frame()
    assert len(frame) == 64 * 64 * 2  # 8192 bytes for 64x64 RGB565
    assert isinstance(frame, bytes)

    # Verify display buffer
    buf = sim.display_buffer.to_numpy()
    assert buf.shape == (512, 512, 4)
    assert np.all(np.isfinite(buf))


def test_box_particle_bins(ti_init):
    """Test live box particle containment monitoring and spatial layer bins."""
    cfg = TaichiDEMConfig(n_particles=200)
    sim = TaichiRotatingDrumDEM(cfg)

    # Step simulation
    for _ in range(15):
        sim.step()

    stats = sim.get_box_particle_bins()
    assert stats["total"] == 200
    assert stats["in_box"] == 200
    assert stats["escaped"] == 0
    assert stats["ratio"] == 100.0
    assert sum(stats["layers"]) == 200
