# Linux CPython 3.15t release wheels

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
