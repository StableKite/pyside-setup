"""Print the Python development dependency for the Debian substvars."""

from pathlib import Path
import subprocess
import sys
import sysconfig


def installed_owner(path):
    path = path.resolve(strict=True)
    match = subprocess.check_output(["dpkg-query", "-S", "--", str(path)], text=True).strip()
    owner, separator, owned_path = match.partition(": ")
    if not separator or owned_path != str(path) or "," in owner or "\n" in owner:
        raise SystemExit(f"Expected one installed package owning {path}, got {match!r}")

    status, package, version = subprocess.check_output(
        ["dpkg-query", "-W", "-f=${db:Status-Status}\t${Package}\t${Version}", owner],
        text=True,
    ).split("\t")
    if status != "installed":
        raise SystemExit(f"Python development package {owner} is not installed")
    return f"{package} (>= {version})"


if sysconfig.get_config_var("Py_GIL_DISABLED"):
    # The active FT interpreter can come from a package other than python3-dev.
    header = Path(sysconfig.get_path("include")) / "Python.h"
    library = Path(sysconfig.get_config_var("LIBDIR")) / sysconfig.get_config_var("LDLIBRARY")
    print(", ".join(dict.fromkeys(installed_owner(path) for path in (header, library))))
else:
    version, next_version = sys.argv[1:]
    print(f"python3-dev (>= {version}), python3-dev (<< {next_version}~)")
