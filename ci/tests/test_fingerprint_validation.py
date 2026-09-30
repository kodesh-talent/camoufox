"""Exercise the shipped C++ validator with Firefox's exceptions disabled."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def validator(tmp_path_factory):
    compiler = shutil.which("g++")
    assert compiler, "g++ is required for the native fingerprint validation tests"
    directory = tmp_path_factory.mktemp("fingerprint-validator")
    debug = directory / "mozilla" / "glue" / "Debug.h"
    debug.parent.mkdir(parents=True)
    debug.write_text('#include <cstdio>\n#define printf_stderr(...) std::fprintf(stderr, __VA_ARGS__)\n')
    spec = importlib.util.spec_from_file_location(
        "fingerprint_properties", ROOT / "scripts" / "gen-fingerprint-properties.py"
    )
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    (directory / "FingerprintProperties.inc").write_text(
        generator.render(ROOT / "settings" / "properties.json"), encoding="utf-8"
    )
    source = directory / "validator.cpp"
    source.write_text('''#include <iostream>
#include <iterator>
#include "MaskConfig.hpp"
int main(int argc, char** argv) {
  std::string error;
  std::optional<std::string> result;
  if (argc > 1 && std::string(argv[1]) == "export") {
    result = MaskConfig::CurrentFingerprintJson(error);
  } else {
    std::string input((std::istreambuf_iterator<char>(std::cin)), {});
    result = MaskConfig::CanonicalizeFingerprintDocument(input, 156, error);
  }
  if (!result) { std::cerr << error; return 2; }
  std::cout << *result;
}
''', encoding="utf-8")
    executable = directory / ("validator.exe" if os.name == "nt" else "validator")
    result = subprocess.run(
        [compiler, "-std=c++17", "-fno-exceptions", f"-I{directory}",
         f"-I{ROOT / 'additions' / 'camoucfg'}", str(source), "-o", str(executable)],
        capture_output=True, text=True, timeout=90,
    )
    assert result.returncode == 0, result.stderr
    return executable


def document(fingerprint, **overrides):
    return json.dumps({
        "format": "camoufox-fingerprint", "schemaVersion": 1,
        "browserMajor": 156, "createdAt": "2026-09-30T00:00:00.000Z",
        "fingerprint": fingerprint, **overrides,
    })


def invoke(validator, data="", *, export=False, config=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("CAMOU_CONFIG")}
    if config is not None:
        env["CAMOU_CONFIG"] = json.dumps(config)
    return subprocess.run(
        [str(validator), *(["export"] if export else [])], input=data,
        encoding="utf-8", capture_output=True, env=env, timeout=10,
    )


def test_unconfigured_browser_exports_native_defaults(validator):
    result = invoke(validator, export=True)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == {}


def test_empty_fingerprint_round_trips(validator):
    result = invoke(validator, document({}))
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == {}


def test_export_excludes_runtime_controls(validator):
    result = invoke(validator, export=True, config={
        "navigator.language": "zh-CN", "debug": True, "addons": ["local-addon"],
    })
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == {"navigator.language": "zh-CN"}


@pytest.mark.parametrize("fingerprint", [
    {"fonts": [42]}, {"voices": ["invalid"]},
    {"voices": [{"lang": 1, "name": "voice", "voiceUri": "voice", "isDefault": True, "isLocalService": True}]},
    {"screen.width": 2**32}, {"window.screenX": 2**31},
    {"webGl:contextAttributes": {"alpha": "true"}},
    {"webGl:parameters": {"3386": [1]}},
    {"webGl:shaderPrecisionFormats": {"35633,36338": {"rangeMin": "bad", "rangeMax": 127, "precision": 23}}},
    {"addons": []}, {"unknown": True}, {"screen.width": -1},
])
def test_rejects_invalid_properties_without_aborting(validator, fingerprint):
    result = invoke(validator, document(fingerprint))
    assert result.returncode == 2, (result.returncode, result.stdout, result.stderr)
    assert result.stderr


@pytest.mark.parametrize("overrides", [
    {"format": None}, {"format": 1}, {"format": {}},
    {"schemaVersion": True}, {"schemaVersion": 2},
    {"browserMajor": 155}, {"createdAt": None}, {"extra": True},
])
def test_rejects_invalid_envelopes_without_aborting(validator, overrides):
    result = invoke(validator, document({"navigator.language": "zh-CN"}, **overrides))
    assert result.returncode == 2, (result.returncode, result.stdout, result.stderr)
    assert result.stderr


@pytest.mark.parametrize("data", ["{", "[]", "null", '{"format":1e1000}'])
def test_rejects_malformed_json_without_aborting(validator, data):
    result = invoke(validator, data)
    assert result.returncode == 2, (result.returncode, result.stdout, result.stderr)


def test_rejects_oversized_document(validator):
    result = invoke(validator, document({"navigator.userAgent": "a" * (4 * 1024 * 1024)}))
    assert result.returncode == 2


def test_valid_structured_fingerprint_round_trips(validator):
    config = {
        "navigator.language": "zh-CN", "screen.width": 1920,
        "fonts": ["Arial", "微软雅黑"],
        "webGl:parameters": {"3379": 16384, "3386": [32767, 32767], "34467": []},
        "webGl:shaderPrecisionFormats": {"35633,36338": {"rangeMin": 127, "rangeMax": 127, "precision": 23}},
        "webGl:contextAttributes": {"alpha": True, "powerPreference": "default"},
        "voices": [{"lang": "en-US", "name": "voice", "voiceUri": "voice", "isDefault": True, "isLocalService": True}],
    }
    result = invoke(validator, document(config))
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == config
