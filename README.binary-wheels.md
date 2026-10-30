# CPython 3.15t release wheels

Windows x86-64 CPython 3.15t is also supported. The Windows wheels are compiled
against the genuine CPython 3.15.2t headers and import library and Qt 6.12.0.
Both platforms contain the complete set of bindings discovered in the matching
Qt SDK; no module subset is requested. Windows provides 59 extension modules.
The rebuilt Linux payload provides 58, including all three WebEngine modules
from the previous Linux release. The SDK add-ons were combined only after all
3,871 shared ELF/header files matched exactly. Both platforms use the same
unchanged native source tree; the native and delivery source identities are
recorded separately in the release manifest.

Normal installation, including Shiboken runtime dependency resolution:

```console
python -m pip install "PySide6 @ git+https://github.com/StableKite/pyside-setup.git"
python -m pip check
```

The separate `pip/shiboken6-generator-build` project provides the native generator
and its CMake files without depending on the Shiboken runtime it must first build.
`pip/shiboken6-generator` is the full developer distribution and keeps its matching
runtime dependency. Each project selects its own hash-verified platform wheel.
Consumers of the binary wheels do not need a Qt or LLVM development SDK.

Source builds still require those development SDKs. The backend discovers matching
Qt installations under `~/Qt` and aqt's per-user SDK directory, including Windows
`msvc2022_64` installations. `--qtpaths`, `QTPATHS`, `QT_ROOT_DIR`, `QT_HOST_PATH`,
`CMAKE_PREFIX_PATH`, and PATH provide explicit SDK locations. An unavailable SDK
now produces an actionable error instead of a silent exit with status -1.
Normal Windows Git CRLF checkout conversion preserves delivery source identity;
changed source contents and mismatched wheel hashes remain rejected.

The default PEP 517 entry point uses the verified release wheel for an unchanged
source snapshot on Linux x86_64 with glibc 2.39 or newer. pip downloads the wheel
and validates its SHA256; consumers need no Qt or LLVM development SDK.

The original backend remains available at build_scripts.pep517_backend. Changes
to tracked runtime/build source files, project metadata, a different ABI/platform
or a source distribution without Git provenance use the original source backend.
A release manifest must be refreshed when publishing a new native source snapshot.
Editable installs and sdists continue to use the original source build hooks.

The Linux payload includes the non-driver runtime shared libraries (including
WebEngine X11 and Wayland client libraries and open-source SQL clients). Shiboken
is provided by the matching separate wheel. Mimer/Oracle SQL driver plugins keep
their optional vendor-client requirement. System graphics driver frontends
(EGL/GL/GLX/GLdispatch) remain supplied by the operating system.

Qt tool executables in this payload require x86-64-v2. Binary selection checks
that requirement automatically, and the wheel retains a Linux platform tag.
It is not advertised as a generic-ISA manylinux artifact.
