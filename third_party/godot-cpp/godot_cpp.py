#!/usr/bin/env python3
"""Helper for the GN build: wraps godot-cpp's binding generator.

Lives in third_party/godot-cpp/ (the root repo, next to the godot-cpp/
submodule) and is used by third_party/godot-cpp/BUILD.gn.

GN's exec_script needs newline-separated output, and this GN build has no
list comprehensions, so the generated file list is computed here.

Commands:
  list <api_file> <gen_dir> <engine_binary>
                                   print all generated files, one per line
  sources                          print hand-written godot-cpp sources,
                                   relative to this script's directory
  generate <api_file> <interface_file> <gen_dir> <engine_binary>
                                   run the binding generator

If <api_file> is missing it is created by running <engine_binary>
--headless --dump-extension-api (which writes the dump to the current
directory) before proceeding. <engine_binary> is a path or a name on PATH.

<gen_dir> is a GN label-style path (//out/gen/godot_cpp); paths returned
keep that form so GN resolves them. Relative paths passed in (interface
file) are resolved against the project root, so pass //labels instead.
The generator itself writes under the corresponding filesystem path.

`generate` runs the generator into a scratch dir and only copies over files
whose content changed (and removes files that are no longer generated), so
unchanged files keep their mtime and ninja (restat) skips recompiling them.

Note: stdout is parsed by GN as the file list — all diagnostics go to
stderr.
"""

import filecmp
import glob
import os
import shutil
import subprocess
import sys


def find_root(start):
    d = os.path.abspath(start)
    while d != os.path.dirname(d):
        if os.path.exists(os.path.join(d, ".gn")):
            return d
        d = os.path.dirname(d)
    raise RuntimeError("could not find project root (no .gn file)")


ROOT = find_root(os.getcwd())
GODOT_CPP = os.path.join(ROOT, "third_party", "godot-cpp", "godot-cpp")
sys.path.insert(0, GODOT_CPP)

import binding_generator  # noqa: E402


def resolve(p):
    """Resolve a GN path (//label-style or relative) to a filesystem path."""
    if p.startswith("//"):
        return os.path.join(ROOT, p[2:])
    if os.path.isabs(p):
        return p
    return os.path.join(ROOT, p)


def ensure_api_file(api_file, engine_binary):
    """Create the engine's API dump if it is missing.

    Engine updates are not tracked: after one, delete the out dir (or just
    the dump) and rebuild.
    """
    if os.path.exists(api_file):
        return
    if not shutil.which(engine_binary):
        raise SystemExit(
            f"error: engine binary not found: {engine_binary!r} — put the "
            f"Godot editor on PATH as 'godot', or pass its path with "
            f"--args='engine_binary=\"/path/to/godot\"'")
    api_dir = os.path.dirname(os.path.abspath(api_file))
    os.makedirs(api_dir, exist_ok=True)  # Gen dir may not exist yet.
    print(f"godot_cpp: creating {api_file} via --dump-extension-api",
          file=sys.stderr)
    # The engine prints to stdout — keep it out of our file list.
    subprocess.call([engine_binary, "--headless", "--dump-extension-api"],
                    cwd=api_dir, stdout=sys.stderr)
    if not os.path.exists(api_file):
        raise SystemExit(f"error: {engine_binary} --dump-extension-api did "
                         f"not write {api_file}")


def sync_tree(src, dst):
    """Make dst match src, touching only files whose content differs."""
    for dirpath, _, filenames in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in filenames:
            s = os.path.join(dirpath, f)
            d = os.path.join(dst, rel, f)
            if not os.path.exists(d) or not filecmp.cmp(s, d, shallow=False):
                shutil.copyfile(s, d)
    for dirpath, dirnames, filenames in os.walk(dst, topdown=False):
        rel = os.path.relpath(dirpath, dst)
        for f in filenames:
            if not os.path.exists(os.path.join(src, rel, f)):
                os.remove(os.path.join(dirpath, f))
        if not os.path.exists(os.path.join(src, rel)):
            os.rmdir(dirpath)


def main():
    cmd = sys.argv[1]
    if cmd == "list":
        api_file, gen_dir, engine_binary = sys.argv[2:5]
        ensure_api_file(resolve(api_file), engine_binary)
        for f in binding_generator.get_file_list(
                resolve(api_file), gen_dir, headers=True, sources=True):
            print(f)
    elif cmd == "sources":
        # Paths relative to this script's directory (where BUILD.gn lives),
        # since GN resolves relative paths against the BUILD.gn's directory.
        script_dir = os.path.join(ROOT, "third_party", "godot-cpp")
        for f in sorted(glob.glob(os.path.join(GODOT_CPP, "src", "**", "*.cpp"),
                                  recursive=True)):
            print(os.path.relpath(f, script_dir))
    elif cmd == "generate":
        api_file, interface_file, gen_dir, engine_binary = sys.argv[2:6]
        ensure_api_file(resolve(api_file), engine_binary)
        # The generator wipes and rewrites <dir>/gen; let it do that in a
        # scratch dir, then sync into the real one.
        scratch = resolve(gen_dir) + ".tmp"
        shutil.rmtree(scratch, ignore_errors=True)
        binding_generator.generate_bindings(
            api_filepath=resolve(api_file),
            interface_filepath=resolve(interface_file),
            use_template_get_node="False",
            bits="64",
            precision="single",
            output_dir=scratch,
            hooks_path=None,
        )
        sync_tree(os.path.join(scratch, "gen"),
                  os.path.join(resolve(gen_dir), "gen"))
        shutil.rmtree(scratch)
    else:
        raise SystemExit("unknown command: " + cmd)


if __name__ == "__main__":
    main()
