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

    # TinyUSB sources — these are NOT automatically added by the pico-sdk builder
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

    # pico_fix for RP2040 USB enumeration workaround
    env.BuildSources(
        join("$BUILD_DIR", "PicoSDKPicoFix"),
        join(framework_dir, "src", "rp2_common", "pico_fix"),
    )

    # NOTE: The following libraries are now automatically linked by the
    # platform-raspberrypi pico-sdk builder (v1.19.0+). Manually adding
    # them here causes duplicate symbol errors at link time.
    #
    # - pico_multicore    -> automatically handled
    # - pico_unique_id    -> automatically handled
    # - hardware_flash    -> automatically handled
    # - hardware_xip_cache -> automatically handled
    # - pico_platform     -> automatically handled
