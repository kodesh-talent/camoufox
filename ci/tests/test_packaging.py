"""Packaging must replace stale output even when build timestamps are fixed."""

import importlib.util
from pathlib import Path
import shlex
import shutil
import subprocess
import zipfile

import pytest


@pytest.fixture
def packager(monkeypatch, tmp_path):
    scripts = Path(__file__).resolve().parents[2] / "scripts"
    monkeypatch.syspath_prepend(str(scripts))
    spec = importlib.util.spec_from_file_location("package", scripts / "package.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(module, "inject_locales", lambda *args: None)
    seven_zip = shutil.which("7z") or shutil.which("7zz")
    assert seven_zip, "7z is required for packaging regression tests"

    def run(command, **kwargs):
        args = shlex.split(command)
        args[0] = seven_zip
        subprocess.run(args, check=True, capture_output=True)

    monkeypatch.setattr(module, "run", run)
    return module


def archive(path, files):
    with zipfile.ZipFile(path, "w") as output:
        for name, content in files.items():
            output.writestr(zipfile.ZipInfo(name, (2010, 1, 1, 0, 0, 0)), content)


def test_repack_replaces_same_size_files_and_removes_obsolete_entries(packager, tmp_path):
    source = tmp_path / "source.zip"
    output = tmp_path / "output.zip"
    archive(source, {"camoufox/camoufox.exe": b"new-build"})
    archive(output, {"camoufox.exe": b"old-build", "obsolete.dll": b"old"})
    packager.add_includes_to_package(
        str(source), [], [], str(output), "windows", "156.0.1", str(tmp_path)
    )
    with zipfile.ZipFile(output) as result:
        assert result.read("camoufox.exe") == b"new-build"
        assert "obsolete.dll" not in result.namelist()


def test_failed_repack_preserves_previous_package(packager, monkeypatch, tmp_path):
    source = tmp_path / "source.zip"
    output = tmp_path / "output.zip"
    archive(source, {"camoufox/camoufox.exe": b"new-build"})
    archive(output, {"camoufox.exe": b"old-build"})
    original = output.read_bytes()
    run = packager.run

    def fail_archive(command, **kwargs):
        if shlex.split(command)[1] != "x":
            raise RuntimeError("archive creation failed")
        return run(command, **kwargs)

    monkeypatch.setattr(packager, "run", fail_archive)
    with pytest.raises(RuntimeError, match="archive creation failed"):
        packager.add_includes_to_package(
            str(source), [], [], str(output), "windows", "156.0.1", str(tmp_path)
        )
    assert output.read_bytes() == original
