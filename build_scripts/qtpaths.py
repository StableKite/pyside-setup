# Copyright (C) 2026
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
"""Find an installed Qt SDK without requiring its bin directory in PATH."""
import os
from pathlib import Path
import shutil
import sys


def find_qtpaths(version, *, platform=None, home=None, environ=None):
    platform = sys.platform if platform is None else platform
    home = Path.home() if home is None else Path(home)
    environ = os.environ if environ is None else environ
    tools = ('qtpaths6.exe', 'qtpaths.exe') if platform == 'win32' else ('qtpaths6', 'qtpaths')
    explicit = environ.get('QTPATHS')
    if explicit:
        tool = Path(explicit)
        if not tool.is_file():
            raise RuntimeError(f'QTPATHS points to a missing file: {explicit}')
        return str(tool.resolve())

    prefixes = [Path(environ[key]) for key in ('QT_ROOT_DIR', 'QT_HOST_PATH') if environ.get(key)]
    prefixes += [Path(value) for value in environ.get('CMAKE_PREFIX_PATH', '').split(os.pathsep) if value]
    for prefix in prefixes:
        for tool in tools:
            candidate = prefix / 'bin' / tool
            if candidate.is_file():
                return str(candidate.resolve())

    roots = [home / '.local/share/aqt', home / 'Qt']
    if platform == 'win32':
        roots.append(Path('C:/Qt'))
        architectures = ('msvc2022_64', 'msvc2019_64', 'msvc2026_64')
    elif platform == 'darwin':
        architectures = ('macos', 'clang_64')
    else:
        architectures = ('gcc_64',)
    for root in roots:
        for architecture in architectures:
            for tool in tools:
                candidate = root / version / architecture / 'bin' / tool
                if candidate.is_file():
                    return str(candidate.resolve())

    for tool in tools:
        candidate = shutil.which(tool, path=environ.get('PATH', ''))
        if candidate:
            return str(Path(candidate).resolve())
    raise RuntimeError(
        f'Unable to locate qtpaths for Qt {version}. Install the matching Qt development SDK '
        'and pass --qtpaths=<path>, set QTPATHS or QT_ROOT_DIR, or add its bin directory to PATH.'
    )
