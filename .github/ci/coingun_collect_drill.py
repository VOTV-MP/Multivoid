#!/usr/bin/env python3
"""Execute the production collect callback against the client state boundary."""

import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def main():
    source = (ROOT / "src/votv-coop/src/coop/items/coingun_collect.cpp").read_text(encoding="utf-8")
    start = source.index("sg::Verdict OnCollectVerb(")
    end = source.index("\n}\n", start) + 3
    work = ROOT / "local/tests/coingun-collect"
    work.mkdir(parents=True, exist_ok=True)
    callback = work / "callback.inc"
    callback.write_text(source[start:end], encoding="utf-8")
    (work / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(collect_test LANGUAGES CXX)\n'
        f'add_executable(collect_test "{HERE.as_posix()}/coingun_collect_test.cpp")\n'
        'target_compile_features(collect_test PRIVATE cxx_std_17)\n'
        f'target_compile_definitions(collect_test PRIVATE COLLECT_CALLBACK="{callback.as_posix()}")\n',
        encoding="utf-8",
    )
    build = work / "build"
    configure = ["cmake", "-S", str(work), "-B", str(build)]
    if os.name == "nt":
        vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
        major = subprocess.check_output(
            [str(vswhere), "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationVersion"],
            text=True).strip().split(".")[0]
        year = {"16": "2019", "17": "2022", "18": "2026"}[major]
        configure += ["-G", f"Visual Studio {major} {year}", "-A", "x64"]
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", str(build), "--config", "Release"], check=True)
    exe = build / "Release/collect_test.exe" if os.name == "nt" else build / "collect_test"
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
