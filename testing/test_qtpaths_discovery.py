"""SDK discovery and actionable failure before native compilation."""
import os
from pathlib import Path
import tempfile
import unittest
from build_scripts.qtpaths import find_qtpaths


class QtPathsDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.home = Path(self.directory.name)

    def find(self, platform='win32', **environment):
        return find_qtpaths('6.12.0', platform=platform, home=self.home,
                            environ={'PATH': '', **environment})

    def tool(self, relative):
        path = self.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return str(path.resolve())

    def test_windows_aqt_sdk_without_path(self):
        tool = self.tool('Qt/6.12.0/msvc2022_64/bin/qtpaths.exe')
        self.assertEqual(self.find(), tool)

    def test_linux_aqt_sdk_without_path(self):
        tool = self.tool('.local/share/aqt/6.12.0/gcc_64/bin/qtpaths6')
        self.assertEqual(self.find('linux'), tool)

    def test_macos_sdk_without_path(self):
        tool = self.tool('Qt/6.12.0/macos/bin/qtpaths')
        self.assertEqual(self.find('darwin'), tool)

    def test_explicit_tool_has_priority(self):
        self.tool('Qt/6.12.0/msvc2022_64/bin/qtpaths.exe')
        explicit = self.tool('custom/bin/qtpaths.exe')
        self.assertEqual(self.find(QTPATHS=explicit), explicit)

    def test_sdk_root_with_spaces(self):
        tool = self.tool('SDK with spaces/bin/qtpaths6.exe')
        self.assertEqual(self.find(QT_ROOT_DIR=str(self.home / 'SDK with spaces')), tool)

    def test_old_version_is_not_selected(self):
        self.tool('Qt/6.11.0/msvc2022_64/bin/qtpaths.exe')
        with self.assertRaisesRegex(RuntimeError, 'Qt 6.12.0.*development SDK'):
            self.find()

    def test_invalid_explicit_tool_reports_the_path(self):
        with self.assertRaisesRegex(RuntimeError, 'QTPATHS.*missing file'):
            self.find(QTPATHS=str(self.home / 'absent'))


if __name__ == '__main__':
    unittest.main()
