from os.path import join

from SCons.Script import DefaultEnvironment


env = DefaultEnvironment()
platform = env.PioPlatform()
framework_dir = platform.get_package_dir("framework-picosdk")

if framework_dir:
    env.Append(
        CPPPATH=[
            join(framework_dir, "lib", "tinyusb", "src"),
            join(framework_dir, "src", "rp2_common", "pico_fix", "rp2040_usb_device_enumeration", "include"),
            join(framework_dir, "src", "rp2_common", "hardware_xip_cache", "include"),
        ],
        CPPDEFINES=[
            ("CFG_TUSB_DEBUG", 0),
            ("CFG_TUSB_MCU", "OPT_MCU_RP2040"),
            ("CFG_TUSB_OS", "OPT_OS_PICO"),
            ("CFG_TUH_ENABLED", 0),
            ("PICO_RP2040_USB_DEVICE_UFRAME_FIX", 1),
            ("PICO_RP2040_USB_DEVICE_ENUMERATION_FIX", 1),
        ],
    )

    env.BuildSources(
        join("$BUILD_DIR", "PicoSDKTinyUSB"),
        join(framework_dir, "lib", "tinyusb", "src"),
        "-<*>"
        " +<tusb.c>"
        " +<common/tusb_fifo.c>"
        " +<device/usbd.c>"
        " +<device/usbd_control.c>"
        " +<class/cdc/cdc_device.c>"
        " +<portable/raspberrypi/rp2040/dcd_rp2040.c>"
        " +<portable/raspberrypi/rp2040/rp2040_usb.c>",
    )

    env.BuildSources(
        join("$BUILD_DIR", "PicoSDKPicoFix"),
        join(framework_dir, "src", "rp2_common", "pico_fix"),
    )

    env.BuildSources(
        join("$BUILD_DIR", "PicoSDKMultiCore"),
        join(framework_dir, "src", "rp2_common", "pico_multicore"),
    )

    env.BuildSources(
        join("$BUILD_DIR", "PicoSDKUniqueId"),
        join(framework_dir, "src", "rp2_common", "pico_unique_id"),
    )

    env.BuildSources(
        join("$BUILD_DIR", "PicoSDKHardwareFlash"),
        join(framework_dir, "src", "rp2_common", "hardware_flash"),
    )

    env.BuildSources(
        join("$BUILD_DIR", "PicoSDKHardwareXipCache"),
        join(framework_dir, "src", "rp2_common", "hardware_xip_cache"),
    )

    env.BuildSources(
        join("$BUILD_DIR", "PicoSDKPlatform"),
        join(framework_dir, "src", "rp2040", "pico_platform"),
    )
