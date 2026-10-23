# Copyright (C) 2026
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
"""PEP 517 adapter for the multi-distribution Qt for Python source tree.

One PEP 517 call produces exactly one distribution. Legacy ``setup.py build``
still orchestrates shiboken6-generator, shiboken6 and PySide6 as before.
"""
from __future__ import annotations

import os
from importlib.util import find_spec
from pathlib import Path
from contextlib import contextmanager

from setuptools import build_meta as _setuptools

_COMPONENT_ENV = "_PYSIDE_PEP517_COMPONENT"
_COMPONENT_KEY = "pyside-build-type"
_COMPONENTS = {"pyside6", "shiboken6", "shiboken6-generator"}


def _component(config_settings):
    value = (config_settings or {}).get(_COMPONENT_KEY, os.environ.get("_PYSIDE_PEP517_DEFAULT_COMPONENT", "pyside6"))
    if isinstance(value, list):
        if len(value) != 1:
            raise ValueError(f"{_COMPONENT_KEY} must have exactly one value")
        value = value[0]
    if value not in _COMPONENTS:
        raise ValueError(f"Invalid {_COMPONENT_KEY}: {value!r}")
    return value


@contextmanager
def _selected_component(config_settings):
    name = _component(config_settings)
    sentinel = object()
    previous = os.environ.get(_COMPONENT_ENV, sentinel)
    os.environ[_COMPONENT_ENV] = name
    try:
        yield
    finally:
        if previous is sentinel:
            os.environ.pop(_COMPONENT_ENV, None)
        else:
            os.environ[_COMPONENT_ENV] = previous


def get_requires_for_build_wheel(config_settings=None):
    with _selected_component(config_settings):
        requires = _setuptools.get_requires_for_build_wheel(config_settings)
        component = _component(config_settings)
        generator = (
            "shiboken6-generator-build @ "
            "https://github.com/StableKite/pyside-setup/archive/HEAD.tar.gz"
            "#subdirectory=pip/shiboken6-generator-build"
        )
        if component == "shiboken6":
            requires.append(generator)
        elif component == "pyside6":
            # Distinct Git/ZIP/tar URLs avoid pip's fragment-insensitive build
            # tracker collision while every source follows the default HEAD.
            requires += [
                "shiboken6 @ https://github.com/StableKite/pyside-setup/archive/HEAD.zip#subdirectory=pip/shiboken6",
                generator,
            ]
        return requires


def prepare_metadata_for_build_wheel(metadata_directory, config_settings=None):
    with _selected_component(config_settings):
        return _setuptools.prepare_metadata_for_build_wheel(
            metadata_directory, config_settings
        )


def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    # Build each component as a normal, standalone wheel. pip installs the
    # declared Shiboken build dependencies into its isolated environment.
    settings = dict(config_settings or {})
    options = settings.get("--build-option", [])
    if isinstance(options, str):
        options = [options]
    settings["--build-option"] = [*options, "--standalone", "--reuse-build", "--parallel=2"]
    prefix = os.environ.get("CMAKE_PREFIX_PATH")
    sdk_paths = []
    if _component(config_settings) in ("pyside6", "shiboken6"):
        for module in ("shiboken6", "shiboken6_generator"):
            spec = find_spec(module)
            if spec is not None and spec.submodule_search_locations:
                sdk_paths.append(str(Path(next(iter(spec.submodule_search_locations)))))
    try:
        if sdk_paths:
            os.environ["CMAKE_PREFIX_PATH"] = os.pathsep.join(sdk_paths + ([prefix] if prefix else []))
        with _selected_component(config_settings):
            return _setuptools.build_wheel(
                wheel_directory, settings, metadata_directory
            )
    finally:
        if prefix is None:
            os.environ.pop("CMAKE_PREFIX_PATH", None)
        else:
            os.environ["CMAKE_PREFIX_PATH"] = prefix


def get_requires_for_build_sdist(config_settings=None):
    with _selected_component(config_settings):
        return _setuptools.get_requires_for_build_sdist(config_settings)


def build_sdist(sdist_directory, config_settings=None):
    with _selected_component(config_settings):
        return _setuptools.build_sdist(sdist_directory, config_settings)


def get_requires_for_build_editable(config_settings=None):
    with _selected_component(config_settings):
        return _setuptools.get_requires_for_build_editable(config_settings)


def prepare_metadata_for_build_editable(metadata_directory, config_settings=None):
    with _selected_component(config_settings):
        return _setuptools.prepare_metadata_for_build_editable(
            metadata_directory, config_settings
        )


def build_editable(wheel_directory, config_settings=None, metadata_directory=None):
    with _selected_component(config_settings):
        return _setuptools.build_editable(
            wheel_directory, config_settings, metadata_directory
        )
