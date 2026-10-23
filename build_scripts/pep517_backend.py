# Copyright (C) 2026
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
"""PEP 517 adapter for the multi-distribution Qt for Python source tree.

One PEP 517 call produces exactly one distribution. Legacy ``setup.py build``
still orchestrates shiboken6-generator, shiboken6 and PySide6 as before.
"""
from __future__ import annotations

import os
from contextlib import contextmanager

from setuptools import build_meta as _setuptools

_COMPONENT_ENV = "_PYSIDE_PEP517_COMPONENT"
_COMPONENT_KEY = "pyside-build-type"
_COMPONENTS = {"pyside6", "shiboken6", "shiboken6-generator"}


def _component(config_settings):
    value = (config_settings or {}).get(_COMPONENT_KEY, "pyside6")
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
        return _setuptools.get_requires_for_build_wheel(config_settings)


def prepare_metadata_for_build_wheel(metadata_directory, config_settings=None):
    with _selected_component(config_settings):
        return _setuptools.prepare_metadata_for_build_wheel(
            metadata_directory, config_settings
        )


def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    with _selected_component(config_settings):
        return _setuptools.build_wheel(
            wheel_directory, config_settings, metadata_directory
        )


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
