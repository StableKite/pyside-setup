"""PEP 517 adapter: verified release wheel, otherwise the original source backend.

pip performs artifact downloading, caching and hash validation. No dependency
resolution is implemented here. A changed source tree falls back to compilation.
"""
from __future__ import annotations
import importlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import sysconfig
try:
    import tomllib
except ImportError:
    import tomli as tomllib
import hashlib

ROOT = Path(__file__).resolve().parent
MANIFEST = ROOT / "binary_wheels.json"

def _manifest():
    if not MANIFEST.is_file():
        return None
    return json.loads(MANIFEST.read_text())

def _normalized_project(path):
    data = tomllib.loads(path.read_text())
    build = data.get("build-system", {})
    build.pop("build-backend", None)
    build.pop("backend-path", None)
    return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()

def _asset(config_settings=None):
    data = _manifest()
    if not data or sys.version_info[:2] != (3, 15) or not sysconfig.get_config_var("Py_GIL_DISABLED"):
        return None
    if sys.platform != "linux" or platform.machine() != "x86_64":
        return None
    libc, version = platform.libc_ver()
    if libc != "glibc" or tuple(map(int, version.split("."))) < tuple(data["minimum_glibc"]):
        return None
    for name, digest in data.get("delivery_source_hashes", {}).items():
        if not (ROOT / name).is_file() or hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            return None
    for name, digest in data["source_projects"].items():
        if not (ROOT / name).is_file() or _normalized_project(ROOT / name) != digest:
            return None
    # Only the adapter, its configuration/tests and backend-entrypoint changes
    # are excluded. All native/Python sources and source-build logic must match.
    paths = [".", *[":(exclude)" + p for p in data["delivery_paths"]]]
    try:
        match = subprocess.run(
            ["git", "-C", str(ROOT), "diff", "--quiet", data["source_commit"], "--", *paths],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30,
        )
        if match.returncode:
            return None
        untracked = subprocess.check_output(
            ["git", "-C", str(ROOT), "ls-files", "--others", "--exclude-standard"],
            text=True, timeout=30,
        ).splitlines()
        if any(p not in data["delivery_paths"] for p in untracked):
            return None
    except (OSError, subprocess.SubprocessError):
        return None
    component = (config_settings or {}).get(
        "pyside-build-type", os.environ.get("_PYSIDE_PEP517_DEFAULT_COMPONENT", data["default_component"])
    )
    if isinstance(component, list):
        if len(component) != 1:
            return None
        component = component[0]
    if component == "shiboken6-generator" and os.environ.get("_PYSIDE_PEP517_GENERATOR_BUILD_ONLY") == "1":
        component = "shiboken6-generator-build"
    return data["artifacts"].get(component)

def _source():
    data = _manifest()
    name = data["source_backend"] if data else "build_scripts.pep517_backend"
    module, separator, attribute = name.partition(":")
    for path in reversed(data.get("source_backend_path", []) if data else []):
        sys.path.insert(0, str(ROOT / path))
    backend = importlib.import_module(module)
    return getattr(backend, attribute) if separator else backend

def get_requires_for_build_wheel(config_settings=None):
    if _asset(config_settings):
        return ["pip"]
    return _source().get_requires_for_build_wheel(config_settings)

def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    asset = _asset(config_settings)
    if asset is None:
        return _source().build_wheel(wheel_directory, config_settings, metadata_directory)
    target = Path(wheel_directory).resolve()
    target.mkdir(parents=True, exist_ok=True)
    url = asset["url"] + "#sha256=" + asset["sha256"]
    subprocess.run(
        [sys.executable, "-m", "pip", "download", "--no-deps",
         "--only-binary=:all:", "--dest", str(target), url], check=True,
    )
    return asset["filename"]

def __getattr__(name):
    # Without this optional hook, pip uses its standard wheel-to-metadata
    # fallback and retains that wheel for the final installation.
    if name == "prepare_metadata_for_build_wheel" and _asset() is not None:
        raise AttributeError(name)
    return getattr(_source(), name)
