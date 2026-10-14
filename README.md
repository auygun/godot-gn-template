# Godot GN Template

A template for Godot 4 projects written in C++ as a GDExtension and built
with GN + Ninja: desktop (Linux/Windows) and Android builds, APK export, hot
reload, a headless smoke test and CI. The example game is a bare-bones
first-person demo: walk around and shoot box targets.

## Prerequisites

- **An official Godot editor, 4.7 or newer**, from
  https://github.com/godotengine/godot/releases. The C++ bindings
  (godot-cpp, a submodule) are generated from whichever engine you point the
  build at. Put it on `PATH` as `godot` (e.g. a symlink in `~/.local/bin`),
  or pass its path to `gn gen` with
  `--args='engine_binary="/path/to/godot"'`.
- **GN and Ninja**, and Python 3 (as `python3`, which is also needed on
  Windows).
- **A C++20 compiler.** That's GCC or Clang on Linux, or MSVC on Windows (run from a
  Visual Studio developer prompt). ccache is used automatically if present.
- **For Android:** the Android SDK and NDK, a JDK, and the official export
  templates of the same release: install them once per engine version from
  the editor (Editor > Manage Export Templates > Download and Install, or
  Install from File with the release's `.tpz`).

## Build and run

```bash
git submodule update --init --recursive
godot --headless --recovery-mode --path . --import   # creates .godot/
gn gen out/host     # release; add --args='is_debug=true' for debug
ninja -C out/host   # first build also builds godot-cpp (a few minutes)

godot --path .      # play
godot --path . -e   # editor (F5 to play)
ninja -C out/host smoke_test   # headless run, fails on any ERROR: line
```

The import writes `.godot/extension_list.cfg`; without it the game starts
with no C++ classes. `.godot/` is gitignored, so run the import again
whenever it is gone (e.g. after `git clean -fdx`), after adding or
renaming a `.gdextension` file, and after pulling new assets (running the
game doesn't import them). Opening the editor does the same.

Android (one APK per ABI: `target_cpu` `arm64`, `x64`, `arm` or `x86`):

```bash
gn gen --args='ndk="/path/to/ndk" target_os="android" target_cpu="arm64"' out/android/release
ninja -C out/android/release   # libextension.so + out/android/release/app.apk
                               # (gn arg no_apk=true builds only the .so)
```

The export finds the SDK through Editor Settings > Export > Android >
Android SDK Path.

A release APK needs your signing key, from these environment variables:

```bash
export GODOT_ANDROID_KEYSTORE_RELEASE_PATH=/path/to/release.keystore
export GODOT_ANDROID_KEYSTORE_RELEASE_USER=<key alias>
export GODOT_ANDROID_KEYSTORE_RELEASE_PASSWORD=<password>
```

Debug builds (`is_debug=true`, into `out/android/debug`) need none: they
are signed with the editor's own debug keystore.

To debug the Android app in VSCode, install the CodeLLDB extension
(`vadimcn.vscode-lldb`), configure `out/android/debug` (`is_debug=true`),
connect a device with USB debugging on, and run "Android (lldb)". It
builds, installs and starts the debug APK, and attaches before any game
code runs. VSCode needs `adb` (SDK platform-tools) and `jdb` (JDK) on its PATH.

The gameplay code in `src/` (`FpsCharacter`, `Weapon`, `Target`, ...) is the
example game. Replace it with your own.

### When ninja isn't enough

`ninja` picks up every source, header, build file and project file change,
except:

- **A `.gdextension` file added or renamed, or new assets:** re-run the
  import.
- **New engine binary:** delete the out dir and rebuild.
- **New export templates (Android):** delete `app.apk` to force a re-export.

New scenes and assets must be added to the `apk` target's `inputs` in
`BUILD.gn`, like new `.cc` files to `sources`.

## Renaming the project

File, library and build target names are generic (`extension.gdextension`,
`libextension.so`, `app.apk`, ...), so only these carry the project name:

- `config/name` in `project.godot`: window title, Android app label and the
  `user://` data folder.
- `package/unique_name` in `export_presets.cfg` (all four presets): the
  Android app ID, e.g. `com.example.mygame`.
