# Profile fingerprint files

Camoufox exposes three fingerprint actions in the browser application menu:

- **Export current fingerprint…** writes the active process-wide fingerprint
  configuration to a JSON file.
- **Import fingerprint and restart…** validates the selected file, binds its
  fingerprint to the current Firefox profile, and restarts the browser.
- **Remove fingerprint binding and restart…** deletes the profile binding and
  restarts. The next launch then uses its normal launch-time configuration.

The binding is stored as `camoufox-fingerprint.json` in the current profile.
When that file exists, it takes precedence over `CAMOU_CONFIG` and
`CAMOU_CONFIG_<n>` so the identity remains stable across later launches. Import
and removal both require an explicit restart confirmation because the native
fingerprint configuration is read once during process startup.

When the desktop browser has no fingerprint overrides, export writes an empty
`fingerprint` object. Importing that file selects native defaults. Unspecified
properties continue to use the current device's defaults: exporting a partial
configuration does not record every physical device characteristic, cookies,
or browsing data, and does not copy per-context Playwright overrides.

## File format

Exported files use this versioned envelope:

```json
{
  "format": "camoufox-fingerprint",
  "schemaVersion": 1,
  "browserMajor": 156,
  "createdAt": "2026-09-29T00:00:00.000Z",
  "fingerprint": {
    "navigator.language": "zh-CN",
    "screen.width": 1920,
    "screen.height": 1080
  }
}
```

Imports must match the running browser major version and may contain only
properties marked as fingerprint data in `settings/properties.json`. Runtime
and security controls such as add-on paths, certificate paths, debugging, and
world-isolation settings are rejected. Validation checks property types, native
integer bounds, string lists, voice records, and nested WebGL values before the
profile file is written and again before startup activates it. Malformed files
produce an error and leave the existing binding unchanged.

Import, export, and binding share a 4 MiB UTF-8 file size limit. Files are written
through a temporary file and flushed before replacement. An invalid bound file
stops startup with a message naming `camoufox-fingerprint.json`; remove that file
from the profile to recover.

Run the focused checks with:

```bash
python3 -m pytest ci/tests/test_fingerprint_profile.py ci/tests/test_fingerprint_validation.py -q
python3 -m ci.run_patch_guards --binary /path/to/camoufox --only fingerprint-profile
```

The native validator tests require `g++`. The browser guard uses an isolated,
temporary profile and checks export/import round trips, validation, and startup
precedence against the built browser.

## Generator integration

Browser-side import persists identities through the exported
`bindFingerprint()` function in `CamoufoxFingerprint.sys.mjs`. A future native
or browser-side random generator should pass its final, coherent fingerprint to
that function so generated and imported identities share the same validation,
atomic write, and restart path.
