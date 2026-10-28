#!/usr/bin/env python3
"""Compile the production record-refresh callback with isolated engine boundaries."""

import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SOURCE = ROOT / "src/votv-coop/src/coop/props/prop_record_refresh.cpp"
WORK = ROOT / "local/tests/prop-record-refresh"


def main():
    stubs = WORK / "stubs"
    stubs.mkdir(parents=True, exist_ok=True)
    for name in re.findall(r'^#include "([^"]+)"', SOURCE.read_text(encoding="utf-8"), re.M):
        target = stubs / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('#pragma once\n', encoding="utf-8")
    (WORK / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(record_refresh_test LANGUAGES CXX)\n'
        f'add_executable(record_refresh_test "{HERE.as_posix()}/prop_record_refresh_test.cpp")\n'
        'target_compile_features(record_refresh_test PRIVATE cxx_std_17)\n'
        f'target_include_directories(record_refresh_test PRIVATE "{stubs.as_posix()}")\n'
        f'target_compile_definitions(record_refresh_test PRIVATE PROP_RECORD_SOURCE="{SOURCE.as_posix()}")\n',
        encoding="utf-8",
    )
    build = WORK / "build"
    configure = ["cmake", "-S", str(WORK), "-B", str(build)]
    if os.name == "nt":
        vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
        major = subprocess.check_output(
            [str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationVersion"], text=True).strip().split(".")[0]
        year = {"16": "2019", "17": "2022", "18": "2026"}[major]
        configure += ["-G", f"Visual Studio {major} {year}", "-A", "x64"]
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", str(build), "--config", "Release"], check=True)
    exe = build / "Release/record_refresh_test.exe" if os.name == "nt" else build / "record_refresh_test"
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
