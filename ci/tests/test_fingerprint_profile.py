import importlib.util
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _load_script(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_fingerprint_property_table_is_generated_from_safe_schema(tmp_path):
    generator = _load_script(
        "gen_fingerprint_properties",
        ROOT / "scripts" / "gen-fingerprint-properties.py",
    )
    properties = ROOT / "settings" / "properties.json"
    rendered = generator.render(properties)
    schema = json.loads(properties.read_text(encoding="utf-8"))

    safe = {item["property"] for item in schema if item.get("fingerprint")}
    unsafe = {item["property"] for item in schema if not item.get("fingerprint")}
    assert "navigator.userAgent" in safe
    assert "webGl:parameters" in safe
    assert {"addons", "certificates", "debug", "disableWorldIsolation"} <= unsafe
    for name in safe:
        assert f'X("{name}",' in rendered
    for name in unsafe:
        assert f'X("{name}",' not in rendered


def test_zh_cn_locale_overlay_appends_only_matching_resource(tmp_path):
    injector = _load_script(
        "inject_locales", ROOT / "scripts" / "inject-locales.py"
    )
    overlay = tmp_path / "zh-CN" / "browser"
    overlay.mkdir(parents=True)
    (overlay / "appmenu.ftl").write_text(
        "camoufox-fingerprint-export = 导出当前指纹\n", encoding="utf-8"
    )

    original = b"appmenuitem-settings = Settings\n"
    merged = injector.apply_locale_overlay(
        "zh-CN",
        "browser/localization/zh-CN/browser/appmenu.ftl",
        original,
        str(tmp_path),
    )
    assert original.strip() in merged
    assert "导出当前指纹" in merged.decode("utf-8")
    assert (
        injector.apply_locale_overlay(
            "fr",
            "browser/localization/fr/browser/appmenu.ftl",
            original,
            str(tmp_path),
        )
        == original
    )
