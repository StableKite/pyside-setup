"""Source identities for pip's recursively built Qt for Python SDKs."""
from pathlib import Path
import subprocess


def sdk_requirement(name):
    if name not in ("shiboken6", "shiboken6-generator-build"):
        raise ValueError(name)
    root = Path(__file__).resolve().parents[1]
    revision = None
    try:
        revision = subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "--verify", "HEAD"],
            stderr=subprocess.DEVNULL, text=True, timeout=10,
        ).strip()
    except (OSError, subprocess.SubprocessError):
        pass
    if revision and len(revision) == 40 and all(c in "0123456789abcdef" for c in revision):
        # Immutable SDK sources match the enclosing checkout and let pip cache
        # their wheels. Both HTTPS spellings are supported by GitHub and give
        # nested SDK projects distinct, fragment-independent tracker identities.
        suffix = ".git" if name == "shiboken6" else ""
        url = "git+https://github.com/StableKite/pyside-setup" + suffix + "@" + revision
    else:
        extension = "zip" if name == "shiboken6" else "tar.gz"
        url = "https://github.com/StableKite/pyside-setup/archive/HEAD." + extension
    return name + " @ " + url + "#subdirectory=pip/" + name


def runtime_requirement(name):
    """Resolve the tested Linux free-threaded runtime without a producer SDK."""
    import platform
    import sys
    import sysconfig
    if (name == "shiboken6" and sys.version_info[:2] == (3, 15)
            and sysconfig.get_config_var("Py_GIL_DISABLED")
            and sys.platform == "linux" and platform.machine() == "x86_64"):
        return 'shiboken6 @ https://github.com/StableKite/pyside-setup/releases/download/cp315t-linux-r68-20261024/shiboken6-6.12.0a1-6.12.0-cp315-cp315t-linux_x86_64.whl#sha256=16128899ac59465e8a9b00bdd0eafb3b5c5a2803d487a2c61dc2fe407b99c27f'
    return sdk_requirement(name)
