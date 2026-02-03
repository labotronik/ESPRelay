Import("env")
import subprocess

project_dir = env.get("PROJECT_DIR", env.Dir("$").abspath)

try:
    ver = subprocess.check_output(
        ["git", "describe", "--always", "--dirty", "--tags"],
        cwd=project_dir,
        stderr=subprocess.STDOUT,
        text=True,
    ).strip()
except Exception:
    try:
        ver = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=project_dir,
            stderr=subprocess.STDOUT,
            text=True,
        ).strip()
        dirty = subprocess.check_output(
            ["git", "status", "--porcelain"],
            cwd=project_dir,
            stderr=subprocess.STDOUT,
            text=True,
        ).strip()
        if dirty:
            ver += "-dirty"
    except Exception:
        ver = "unknown"

env.Append(BUILD_FLAGS=[f'-DFW_VERSION=\\"{ver}\\"'])
