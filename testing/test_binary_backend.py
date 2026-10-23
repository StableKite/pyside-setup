"""Verify binary selection, source invalidation and pip's real hash checks."""
import copy
import os
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import _binary_wheel_backend as backend

class BinaryWheelBackendTests(unittest.TestCase):
    def test_matching_native_snapshot_selects_the_declared_wheel(self):
        asset = backend._asset()
        self.assertIsNotNone(asset)
        self.assertIn("cp315-cp315t-linux_x86_64.whl", asset["filename"])
        self.assertEqual(backend.get_requires_for_build_wheel(), ["pip"])

    def test_changed_native_source_uses_the_source_backend(self):
        p = backend.ROOT / "sources/pyside6/.cmake.conf"
        original = p.read_bytes()
        try:
            p.write_bytes(original + b"\n# modified native source snapshot\n")
            self.assertIsNone(backend._asset())
        finally:
            p.write_bytes(original)

    def test_changed_project_build_requirements_use_the_source_backend(self):
        p = backend.ROOT / "pyproject.toml"
        original = p.read_bytes()
        try:
            p.write_text(p.read_text().replace('"numpy"', '"numpy>=99"'))
            self.assertIsNone(backend._asset())
        finally:
            p.write_bytes(original)

    def test_different_python_abi_uses_the_source_backend(self):
        with patch.object(backend.sysconfig, "get_config_var", return_value=0):
            self.assertIsNone(backend._asset())

    @unittest.skipUnless(os.environ.get("PYSIDE_BINARY_TEST_WHEEL"), "set PYSIDE_BINARY_TEST_WHEEL to a verified fixture wheel")
    def test_real_pip_copies_only_the_hash_verified_wheel_without_source_sdk(self):
        data = copy.deepcopy(backend._manifest())
        entry = data["artifacts"]["pyside6"]
        wheel = Path(os.environ["PYSIDE_BINARY_TEST_WHEEL"]).resolve()
        self.assertEqual(wheel.name, entry["filename"])
        entry["url"] = wheel.as_uri()
        with tempfile.TemporaryDirectory(prefix="pyside_binary_") as directory:
            with patch.object(backend, "_manifest", return_value=data), \
                    patch.object(backend, "_source", side_effect=AssertionError("source SDK must not be used")):
                result = backend.build_wheel(directory)
            self.assertEqual(result, wheel.name)
            self.assertEqual((Path(directory) / result).stat().st_size, wheel.stat().st_size)
        entry["sha256"] = "0" * 64
        with tempfile.TemporaryDirectory(prefix="pyside_bad_hash_") as directory:
            with patch.object(backend, "_manifest", return_value=data):
                with self.assertRaises(subprocess.CalledProcessError):
                    backend.build_wheel(directory)

if __name__ == "__main__":
    unittest.main()
