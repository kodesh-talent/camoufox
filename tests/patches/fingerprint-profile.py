"""Verify fingerprint export, validation, profile binding, and startup precedence."""

import contextlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from helpers import Marionette, free_port, marionette_args, resolve_binary


@contextlib.contextmanager
def browser(binary, profile, config=None):
    port = free_port()
    (profile / "user.js").write_text(
        f'user_pref("marionette.port", {port});\n', encoding="utf-8"
    )
    env = {k: v for k, v in os.environ.items() if not k.startswith("CAMOU_CONFIG")}
    if config is not None:
        env["CAMOU_CONFIG"] = json.dumps(config)
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(
            [str(binary), "-headless", "-no-remote", "-profile", str(profile),
             *marionette_args(), "about:blank"],
            env=env, stdout=log, stderr=log,
        )
        client = None
        try:
            client = Marionette(port)
            yield client
        finally:
            if client:
                with contextlib.suppress(Exception):
                    client.call("Marionette:Quit", {"flags": ["eForceQuit"]})
                client.close()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def run_async(client, script, *args):
    result = client.call("WebDriver:ExecuteAsyncScript", {
        "script": """
const done = arguments[arguments.length - 1];
const args = Array.from(arguments).slice(0, -1);
const module = ChromeUtils.importESModule("resource:///modules/CamoufoxFingerprint.sys.mjs");
(async () => {
""" + script + """
})().then(value => done({value}), error => done({error: String(error)}));
""",
        "args": list(args), "scriptTimeout": 30000,
    })["value"]
    assert "error" not in result, result.get("error")
    return result.get("value")


def main():
    binary = resolve_binary()
    with tempfile.TemporaryDirectory(prefix="camoufox-fingerprint-") as directory:
        profile = Path(directory)
        export = profile / "指纹-export.json"
        with browser(binary, profile) as client:
            current = client.js("return JSON.parse(ChromeUtils.camouGetFingerprintJSON());")
            assert current == {}, current
            run_async(client, "await module.exportFingerprint(args[0]); return await module.readFingerprint(args[0]);", str(export))
            assert json.loads(export.read_text(encoding="utf-8"))["fingerprint"] == {}
            print("PASS: unconfigured desktop browser exports and reads native defaults", flush=True)

        config = {"navigator.language": "zh-CN", "screen.width": 1920, "screen.height": 1080}
        with browser(binary, profile, {**config, "debug": False}) as client:
            actual = run_async(client, """
await module.exportFingerprint(args[0]);
const fingerprint = await module.readFingerprint(args[0]);
await module.bindFingerprint(fingerprint, {restartBrowser: false});
return {fingerprint, bound: await module.CamoufoxFingerprint.isBound()};
""", str(export))
            assert actual == {"fingerprint": config, "bound": True}, actual
            stored = json.loads((profile / "camoufox-fingerprint.json").read_text(encoding="utf-8"))
            assert stored["fingerprint"] == config, stored
            print("PASS: export/import round trip persists the fingerprint without runtime controls", flush=True)

        with browser(binary, profile, {"navigator.language": "en-US", "screen.width": 1280, "screen.height": 720}) as client:
            actual = client.js("return JSON.parse(ChromeUtils.camouGetFingerprintJSON());")
            assert actual == config, actual
            client.call("Marionette:SetContext", {"value": "content"})
            client.call("WebDriver:Navigate", {"url": "data:text/html,<title>Fingerprint profile guard</title>"})
            actual = client.js("return {language: navigator.language, width: screen.width, height: screen.height};")
            assert actual == {"language": "zh-CN", "width": 1920, "height": 1080}, actual
            client.call("Marionette:SetContext", {"value": "chrome"})
            actual = run_async(client, """
await module.exportFingerprint(args[0]);
await window.PanelUI.ensureReady();
await new Promise(resolve => {
  window.PanelUI.panel.addEventListener("popupshown", resolve, {once: true});
  window.PanelUI.show();
});
await module.CamoufoxFingerprint.updateMenu(window.document);
return {
  fingerprint: await module.readFingerprint(args[0]),
  export: !!window.document.getElementById("appMenu-camoufox-fingerprint-export"),
  import: !!window.document.getElementById("appMenu-camoufox-fingerprint-import"),
  canClear: !window.document.getElementById("appMenu-camoufox-fingerprint-clear").disabled,
};
""", str(export))
            assert actual == {"fingerprint": config, "export": True, "import": True, "canClear": True}, actual
            print("PASS: restart applies the bound identity to content and exposes all menu actions", flush=True)

            before = (profile / "camoufox-fingerprint.json").read_bytes()
            bad = {**stored, "format": None}
            export.write_text(json.dumps(bad), encoding="utf-8")
            result = run_async(client, """
try { await module.readFingerprint(args[0]); return false; }
catch (error) { return String(error).includes("format"); }
""", str(export))
            assert result, "invalid format was accepted"
            assert (profile / "camoufox-fingerprint.json").read_bytes() == before
            print("PASS: malformed import is rejected without crashing or changing the binding", flush=True)

            export.write_text(" " * (4 * 1024 * 1024 + 1), encoding="utf-8")
            result = run_async(client, """
try { await module.readFingerprint(args[0]); return false; }
catch (error) { return String(error).includes("4 MiB"); }
""", str(export))
            assert result, "oversized import was accepted"
            assert (profile / "camoufox-fingerprint.json").read_bytes() == before
            print("PASS: oversized import leaves the existing binding unchanged", flush=True)

        (profile / "camoufox-fingerprint.json").unlink()
        with browser(binary, profile, {"navigator.language": "en-US"}) as client:
            actual = client.js("return JSON.parse(ChromeUtils.camouGetFingerprintJSON());")
            assert actual == {"navigator.language": "en-US"}, actual
            print("PASS: removing the binding restores launch-time configuration", flush=True)


if __name__ == "__main__":
    main()
