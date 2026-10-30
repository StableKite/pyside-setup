"""Verify binary selection, source invalidation and pip's real hash checks."""
import copy
import os
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import platform
import sys
import sysconfig
from unittest.mock import patch
import _binary_wheel_backend as backend

@unittest.skipUnless(
    sys.version_info[:2] == (3, 15)
    and sysconfig.get_config_var("Py_GIL_DISABLED")
    and ((sys.platform == "win32" and platform.machine().lower() in ("amd64", "x86_64"))
    or (sys.platform == "linux" and platform.machine() == "x86_64"
    and platform.libc_ver()[0] == "glibc"
    and tuple(map(int, platform.libc_ver()[1].split("."))) >= (2, 39)
    and backend._cpu_supports(["cx16", "lahf_lm", "popcnt", "pni", "ssse3", "sse4_1", "sse4_2"]))),
    "requires a supported CPython 3.15t binary target",
)
class BinaryWheelBackendTests(unittest.TestCase):
    def test_matching_native_snapshot_selects_the_declared_wheel(self):
        asset = backend._asset()
        self.assertIsNotNone(asset)
        target = "win_amd64" if sys.platform == "win32" else "linux_x86_64"
        self.assertIn("cp315-cp315t-" + target + ".whl", asset["filename"])
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

    def test_debug_python_abi_uses_the_source_backend(self):
        original = backend.sysconfig.get_config_var
        with patch.object(backend.sysconfig, 'get_config_var',
                          side_effect=lambda name: 1 if name == 'Py_DEBUG' else original(name)):
            self.assertIsNone(backend._asset())

    def test_all_components_select_the_matching_platform(self):
        target = "win_amd64" if sys.platform == "win32" else "linux_x86_64"
        for component in ("pyside6", "shiboken6", "shiboken6-generator", "shiboken6-generator-build"):
            with self.subTest(component=component):
                asset = backend._asset({"pyside-build-type": component})
                self.assertIsNotNone(asset)
                self.assertTrue(asset['filename'].endswith(target + '.whl'))

    def test_source_runtime_requirement_uses_the_same_verified_asset(self):
        from build_scripts.pep517_sources import runtime_requirement
        asset = backend._asset({"pyside-build-type": "shiboken6"})
        self.assertEqual(runtime_requirement('shiboken6'),
                         'shiboken6 @ ' + asset['url'] + '#sha256=' + asset['sha256'])

    def test_changed_sources_do_not_reference_an_old_runtime_wheel(self):
        from build_scripts.pep517_sources import runtime_requirement
        with patch.object(backend, '_asset', return_value=None):
            requirement = runtime_requirement('shiboken6')
        self.assertIn('git+https://github.com/StableKite/pyside-setup.git@', requirement)
        self.assertIn('#subdirectory=pip/shiboken6', requirement)

    @unittest.skipUnless(os.environ.get("PYSIDE_BINARY_TEST_WHEEL"), "set PYSIDE_BINARY_TEST_WHEEL to a verified fixture wheel")
    def test_real_pip_copies_only_the_hash_verified_wheel_without_source_sdk(self):
        data = copy.deepcopy(backend._manifest())
        target = data["windows"] if sys.platform == "win32" else data
        entry = target["artifacts"]["pyside6"]
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


class CpuEligibilityTests(unittest.TestCase):
    def test_checkout_line_endings_preserve_delivery_source_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'backend.py'
            source.write_bytes(b'x = 1\ny = 2\n')
            digest = backend._source_hash(source)
            source.write_bytes(b'x = 1\r\ny = 2\r\n')
            self.assertEqual(backend._source_hash(source), digest)
            source.write_bytes(b'x = 9\r\ny = 2\r\n')
            self.assertNotEqual(backend._source_hash(source), digest)

    def test_every_cpu_must_supply_the_required_features(self):
        with tempfile.TemporaryDirectory() as directory:
            info = Path(directory) / "cpuinfo"
            info.write_text("flags : cx16 pni popcnt\nflags : cx16 pni popcnt\n")
            self.assertTrue(backend._cpu_supports(["cx16", "pni"], info))
            info.write_text("flags : cx16 pni popcnt\nflags : pni popcnt\n")
            self.assertFalse(backend._cpu_supports(["cx16", "pni"], info))

    def test_missing_cpu_information_does_not_select_an_unsafe_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertFalse(backend._cpu_supports(["cx16"], Path(directory) / "missing"))

    def test_unsupported_cpu_uses_the_source_backend(self):
        if sys.platform != 'linux':
            self.skipTest('Linux payload CPU-feature guard')
        with patch.object(backend, "_cpu_supports", return_value=False):
            self.assertIsNone(backend._asset())

    def test_explicit_native_build_options_use_the_source_backend(self):
        self.assertIsNone(backend._asset({"cmake.define.CUSTOM_FEATURE": "1"}))

if __name__ == "__main__":
    unittest.main()
