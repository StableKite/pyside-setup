# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
from __future__ import annotations

"""
This is a setuptools setup-script for the Qt for Python project.
For more information see README.md
"""

import os
import sys

# Change the current directory to setup.py's dir.
try:
    this_file = __file__
except NameError:
    this_file = sys.argv[0]
this_file_original = this_file
this_file = os.path.abspath(this_file)
if os.path.dirname(this_file):
    os.chdir(os.path.dirname(this_file))

# The source superproject normally orchestrates three distributions. PEP 517
# frontends request exactly one wheel / metadata directory per invocation. The
# in-tree build backend selects one component while leaving direct setup.py
# development builds unchanged.
_pep517_component = os.environ.get("_PYSIDE_PEP517_COMPONENT")
if _pep517_component is not None:
    if _pep517_component not in ("pyside6", "shiboken6", "shiboken6-generator"):
        raise RuntimeError(f"Invalid PEP 517 component: {_pep517_component!r}")
    if any(arg.startswith("--internal-build-type") for arg in sys.argv[1:]):
        raise RuntimeError("Do not combine PEP 517 component selection with internal build flags")
    sys.argv.append(f"--internal-build-type={_pep517_component}")

# Save the original command line arguments to pass them on to the setup
# mechanism.
original_argv = list(sys.argv)

# If setup.py was invoked via -c "some code" or -m some_command, make sure
# to replace the first argv to be the script name, so that sub-invocations
# continue to work.
if original_argv and original_argv[0] in ['-c', '-m']:
    original_argv[0] = this_file_original

from build_scripts.main import get_package_version, check_allowed_python_version
from build_scripts.setup_runner import SetupRunner

# The __version__ variable is just for PEP compliance, and shouldn't be
# used as a value source. Use get_package_version() instead.
__version__ = get_package_version()

check_allowed_python_version()

setup_runner = SetupRunner(original_argv)
setup_runner.run_setup()
