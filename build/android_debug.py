#!/usr/bin/env python3
"""Start the debug APK on a device, ready for LLDB to attach (VSCode's
"Android (lldb)" launch configuration runs this as its preLaunchTask).

 1. Installs <out_dir>/app.apk (a debug build: run-as needs a debuggable app).
 2. Starts the NDK's lldb-server for the build's target_cpu in the app's data
    dir as an LLDB platform server on an abstract unix socket (no port
    forwarding).
 3. Starts the app with `am start -D`: it waits for a Java debugger before
    running any code, so startup code (_ready, extension init) can be hit.
 4. Writes <out_dir>/lldb_attach.txt (the LLDB attach commands; the debugger
    must resume after them) and prints "android_debug: ready", which ends
    the preLaunchTask.
 5. Waits for the attach (the process's TracerPid), then connects jdb over
    JDWP once, which releases the app from step 3.

Usage: android_debug.py [out_dir]   (default: out/android/debug)
Needs adb (ANDROID_HOME/platform-tools or PATH) and jdb (JAVA_HOME/bin or
PATH). With several devices, set ANDROID_SERIAL.
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import time

# lldb-server dir name in the NDK's clang runtime dir, per GN target_cpu.
LLDB_ARCH = {"arm64": "aarch64", "arm": "arm", "x64": "x86_64", "x86": "i386"}
SOCKET_NAME = "lldb-platform.sock"
JDWP_PORT = 65534
ATTACH_TIMEOUT = 60  # seconds to wait for the debugger to attach
DEVICE_TIMEOUT = 10  # seconds to wait for a device


def fail(msg):
    print("android_debug: FAILED — " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def log(msg):
    print("android_debug: " + msg, flush=True)


def read_gn_args(out_dir):
    path = os.path.join(out_dir, "args.gn")
    if not os.path.exists(path):
        fail("%s not found — run gn gen first" % path)
    args = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'\s*(\w+)\s*=\s*"?([^"#\n]*)"?', line)
            if m:
                args[m.group(1)] = m.group(2).strip()
    return args


def find_tool(name, env_var, subdir):
    root = os.environ.get(env_var)
    if root:
        path = os.path.join(root, subdir, name)
        if os.path.exists(path):
            return path
    path = shutil.which(name)
    if not path:
        fail("%s not found (looked in $%s/%s and PATH)" % (name, env_var, subdir))
    return path


def package_name(srcdir):
    # All presets share the package name.
    with open(os.path.join(srcdir, "export_presets.cfg")) as f:
        m = re.search(r'^package/unique_name="([^"]+)"', f.read(), re.M)
    if not m:
        fail("no package/unique_name in export_presets.cfg")
    return m.group(1)


def main():
    srcdir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(srcdir, sys.argv[1] if len(sys.argv) > 1
                           else "out/android/debug")
    gn_args = read_gn_args(out_dir)
    if gn_args.get("target_os") != "android":
        fail("%s is not an android build" % out_dir)
    if gn_args.get("is_debug") != "true":
        fail("%s is not a debug build (is_debug=true); release APKs are not "
             "debuggable" % out_dir)
    target_cpu = gn_args.get("target_cpu", "arm64")
    servers = glob.glob(os.path.join(
        gn_args["ndk"], "toolchains/llvm/prebuilt/*/lib/clang/*/lib/linux",
        LLDB_ARCH[target_cpu], "lldb-server"))
    if not servers:
        fail("no lldb-server for %s in the NDK %s" % (target_cpu, gn_args["ndk"]))
    apk = os.path.join(out_dir, "app.apk")
    if not os.path.exists(apk):
        fail("%s not found — build it with ninja -C %s" % (apk, out_dir))

    adb_path = find_tool("adb", "ANDROID_HOME", "platform-tools")
    jdb_path = find_tool("jdb", "JAVA_HOME", "bin")
    pkg = package_name(srcdir)
    # A stale file would attach to a dead pid: without one, a failed run
    # makes the debugger fail clearly.
    attach_file = os.path.join(out_dir, "lldb_attach.txt")
    if os.path.exists(attach_file):
        os.remove(attach_file)

    def adb(*args, check=True):
        proc = subprocess.run([adb_path] + list(args), stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
        if check and proc.returncode != 0:
            fail("adb %s:\n%s" % (" ".join(args), proc.stdout))
        return proc.stdout.strip()

    def run_as(cmd, check=True):
        return adb("shell", "run-as %s %s" % (pkg, cmd), check=check)

    # The export's editor kills the adb server on exit, so a fresh server
    # starts here and needs a moment to find USB devices.
    try:
        subprocess.run([adb_path, "wait-for-device"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=DEVICE_TIMEOUT)
    except subprocess.TimeoutExpired:
        fail("no device found within %ds" % DEVICE_TIMEOUT)
    serial = adb("get-serialno")
    log("installing %s on %s" % (apk, serial))
    adb("install", "-r", apk)
    adb("shell", "am", "force-stop", pkg)
    run_as("pkill -f lldb-server", check=False)

    # SELinux doesn't allow executing from /data/local/tmp as the app: copy
    # it into the app's data dir.
    tmp = "/data/local/tmp/%s-lldb-server" % LLDB_ARCH[target_cpu]
    adb("push", servers[0], tmp)
    adb("shell", "cat %s | run-as %s sh -c 'cat > lldb-server && "
        "chmod 700 lldb-server'" % (tmp, pkg))
    run_as("sh -c './lldb-server platform --server --listen "
           "unix-abstract:///%s/%s </dev/null >/dev/null 2>&1 &'"
           % (pkg, SOCKET_NAME))

    activity = adb("shell", "cmd package resolve-activity --brief %s" % pkg)
    activity = activity.splitlines()[-1]
    adb("shell", "am", "start", "-D", "-n", activity)
    pid = ""
    for _ in range(50):
        pid = adb("shell", "pidof", pkg, check=False)
        if pid:
            break
        time.sleep(0.2)
    if not pid:
        fail("%s did not start" % pkg)

    # ART uses SIGSEGV/SIGBUS internally (null/stack checks): pass them on
    # silently, or each one stops the app (then killed as unresponsive).
    with open(attach_file, "w") as f:
        f.write("platform select remote-android\n"
                "platform connect unix-abstract-connect://%s/%s/%s\n"
                "settings set plugin.jit-loader.gdb.enable off\n"
                "settings append target.exec-search-paths %s\n"
                "process attach --pid %s\n"
                "process handle SIGSEGV SIGBUS -n false -p true -s false\n"
                % (serial, pkg, SOCKET_NAME, out_dir, pid))
    log("%s started (pid %s), waiting for the debugger" % (pkg, pid))
    log("ready")

    deadline = time.time() + ATTACH_TIMEOUT
    while True:
        status = run_as("cat /proc/%s/status" % pid, check=False)
        m = re.search(r"^TracerPid:\s*(\d+)", status, re.M)
        if not m:
            fail("%s (pid %s) is gone" % (pkg, pid))
        if m.group(1) != "0":
            break
        if time.time() > deadline:
            fail("no debugger attached within %ds; %s is still waiting"
                 % (ATTACH_TIMEOUT, pkg))
        time.sleep(0.2)

    # Releases `am start -D`: once jdb answers, the app continues. Give it
    # a moment to notice before closing.
    log("debugger attached, releasing the app")
    adb("forward", "tcp:%d" % JDWP_PORT, "jdwp:%s" % pid)
    jdb = subprocess.Popen(
        [jdb_path, "-connect",
         "com.sun.jdi.SocketAttach:hostname=localhost,port=%d" % JDWP_PORT],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)
    released = False
    try:
        jdb.stdin.write('print "__jdb_ready__"\n')
        jdb.stdin.flush()
        for line in jdb.stdout:
            if "__jdb_ready__" in line and "print" not in line:
                released = True
                time.sleep(0.5)
                break
        jdb.stdin.write("exit\n")
        jdb.stdin.flush()
    except BrokenPipeError:
        pass  # jdb exited early: couldn't connect.
    jdb.wait()
    adb("forward", "--remove", "tcp:%d" % JDWP_PORT, check=False)
    if not released:
        fail("jdb could not connect to %s (still stopped in the debugger "
             "after attaching?)" % pkg)
    log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
