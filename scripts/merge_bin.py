# PlatformIO post-script: after the app image is built, produce release files in dist/:
#   SC01Plus-Bedside-HAController-v<ver>-full.bin  bootloader + partitions + boot_app0 + app, flash at 0x0 (first install)
#   SC01Plus-Bedside-HAController-v<ver>-ota.bin   app only (web /update page and on-device updates)
# Flashing the app-only image at 0x0 leaves the board in a boot loop, so never do that.
Import("env")
import os, shutil, subprocess, sys

def make_release_files(source, target, env):
    build = env.subst("$BUILD_DIR")
    version = open(os.path.join(env["PROJECT_DIR"], "VERSION")).read().strip()
    base = "SC01Plus-Bedside-HAController-v%s" % version
    dist = os.path.join(env["PROJECT_DIR"], "dist")
    os.makedirs(dist, exist_ok=True)
    app = os.path.join(build, "firmware.bin")
    ota = os.path.join(dist, base + "-ota.bin")
    full = os.path.join(dist, base + "-full.bin")
    shutil.copyfile(app, ota)

    board = env.BoardConfig()
    flash_mode = board.get("build.flash_mode", "dio")
    flash_freq = str(int(board.get("build.f_flash", "80000000L").rstrip("L")) // 1000000) + "m"
    flash_size = board.get("upload.flash_size", "16MB")
    platform = env.PioPlatform()
    esptool = os.path.join(platform.get_package_dir("tool-esptoolpy"), "esptool.py")
    boot_app0 = os.path.join(platform.get_package_dir("framework-arduinoespressif32"),
                             "tools", "partitions", "boot_app0.bin")
    cmd = [env.subst("$PYTHONEXE"), esptool, "--chip", "esp32s3", "merge_bin", "-o", full,
           "--flash_mode", flash_mode, "--flash_freq", flash_freq, "--flash_size", flash_size,
           "0x0", os.path.join(build, "bootloader.bin"),
           "0x8000", os.path.join(build, "partitions.bin"),
           "0xe000", boot_app0,
           "0x10000", app]
    print(" ".join(cmd))
    subprocess.check_call(cmd)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", make_release_files)
