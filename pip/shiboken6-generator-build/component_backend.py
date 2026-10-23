"""PEP 517 entry point for shiboken6-generator in the source superproject."""
import os
import sys
from pathlib import Path

_source = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_source))
os.chdir(_source)
os.environ["_PYSIDE_PEP517_DEFAULT_COMPONENT"] = 'shiboken6-generator'
os.environ["_PYSIDE_PEP517_GENERATOR_BUILD_ONLY"] = "1"
from importlib import import_module
_backend = import_module("_binary_wheel_backend")
def __getattr__(name):
    return getattr(_backend, name)
