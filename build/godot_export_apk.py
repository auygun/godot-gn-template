#!/usr/bin/env python3
"""Export the Android APK (the apk action).

Runs the engine's headless export with the given preset (one ABI per APK),
--export-debug / --export-release for debug / release builds.

The export can "succeed" with an empty pck (e.g. a .gdextension android lib
key without an arch tag), so it must exit 0 and the APK must contain both
the pck and libextension.so for <abi>; on failure the APK is deleted so the
next ninja run retries.

Usage: godot_export_apk.py <engine_binary> <apk_path> <export_type> <preset>
                           <abi>
(paths relative to the out dir, ninja's working directory; <abi> is the
APK lib dir name: arm64-v8a, x86_64, armeabi-v7a or x86)
"""

import os
import subprocess
import sys
import zipfile


def fail(msg, apk_path):
    print("godot_export_apk: FAILED — " + msg, file=sys.stderr)
    if os.path.exists(apk_path):
        os.remove(apk_path)
    return 1


def main():
    engine_binary, apk_path, export_type, preset, abi = sys.argv[1:6]
    # This script lives in <project>/build/ — derive the project root.
    srcdir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # The export signs the APK with apksigner, whose bundled Conscrypt
    # loads a native library: on JDK 24+ that prints a 4-line "restricted
    # method" warning (and a future JDK will block it) unless native access
    # is enabled. The java launcher reads JDK_JAVA_OPTIONS (and prints a
    # one-line "Picked up" note for it).
    env = dict(os.environ)
    env["JDK_JAVA_OPTIONS"] = " ".join(filter(None, [
        env.get("JDK_JAVA_OPTIONS"), "--enable-native-access=ALL-UNNAMED"]))

    apk_abs = os.path.abspath(apk_path)
    print("godot_export_apk: exporting Android APK (preset %r)" % preset,
          flush=True)
    rc = subprocess.call(
        [engine_binary, "--headless", "--path", srcdir,
         "--export-%s" % export_type, preset, apk_abs],
        env=env,
    )
    if rc != 0:
        return fail("export exited %d (release builds need the "
                    "GODOT_ANDROID_KEYSTORE_RELEASE_* variables)" % rc,
                    apk_path)

    # The export can "succeed" with an empty pck — verify the contents.
    with zipfile.ZipFile(apk_path) as z:
        names = z.namelist()
    for required in ("assets/project.binary", "lib/%s/libextension.so" % abi):
        if required not in names:
            return fail("APK missing %s" % required, apk_path)

    print("godot_export_apk: %s (%d entries)" % (apk_abs, len(names)), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
