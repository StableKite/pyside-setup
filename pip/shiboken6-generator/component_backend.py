"""PEP 517 entry point for shiboken6-generator in the source superproject."""
import os
import sys
from pathlib import Path

_source = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_source))
os.chdir(_source)
os.environ["_PYSIDE_PEP517_DEFAULT_COMPONENT"] = 'shiboken6-generator'
from build_scripts.pep517_backend import (
    get_requires_for_build_wheel, prepare_metadata_for_build_wheel, build_wheel,
    get_requires_for_build_sdist, build_sdist, get_requires_for_build_editable,
    prepare_metadata_for_build_editable, build_editable,
)
