Import("env")
import subprocess

project_dir = env.get("PROJECT_DIR", env.Dir("$").abspath)

def git_cmd(args):
    return subprocess.check_output(
        ["git"] + args,
        cwd=project_dir,
        stderr=subprocess.STDOUT,
        text=True,
    ).strip()

try:
    ver = git_cmd(["describe", "--always", "--dirty", "--tags"])
except Exception:
    try:
        ver = git_cmd(["rev-parse", "--short", "HEAD"])
        dirty = git_cmd(["status", "--porcelain"])
        if dirty:
            ver += "-dirty"
    except Exception:
        ver = "unknown"

try:
    tag = git_cmd(["describe", "--tags", "--abbrev=0"])
except Exception:
    tag = ""

env.Append(BUILD_FLAGS=[f'-DFW_VERSION=\\"{ver}\\"', f'-DFW_TAG=\\"{tag}\\"'])
