# PlatformIO pre-script: compile the contents of /VERSION into the firmware as FW_VERSION.
Import("env")
import os
path = os.path.join(env["PROJECT_DIR"], "VERSION")
version = open(path).read().strip()
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
print("Firmware version:", version)
