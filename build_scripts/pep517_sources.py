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
