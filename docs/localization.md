# Default browser language

The browser defaults to Simplified Chinese. `settings/camoufox.cfg` sets
`intl.locale.requested` to `zh-CN` as a default preference: saved profile choices
and explicit launcher preferences take precedence. Firefox derives the default
web content language list from the selected UI locale. Its `LocaleService`
only reads `intl.accept_languages` when that preference has a user or locked
value, so setting its default branch would have no effect.

The Python launcher uses its existing locale normalization for `zh-CN` when
neither an explicit locale nor the configuration or GeoIP supplies a language.
The resulting `zh-CN` request selects the packaged Simplified Chinese locale. Explicit
locales continue to control both the browser UI and the content locale.
Configuration values in `locale:all` and `navigator.language` also take
precedence over the default when `locale:language` is absent.

## Build and run on Windows

From the repository directory in PowerShell, use the existing Linux Docker
builder to cross-compile and package the Windows x86_64 browser:

```powershell
docker build -t camoufox-builder .
New-Item -ItemType Directory -Force dist | Out-Null
docker run --rm -v "${PWD}/dist:/app/dist" camoufox-builder --target windows --arch x86_64
```

Extract the generated Windows ZIP in `dist/`, then run `camoufox.exe` with a
new profile to check the defaults without an existing profile's preferences:

```powershell
$package = Get-ChildItem dist/camoufox-*-win.x86_64.zip | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Expand-Archive -LiteralPath $package.FullName -DestinationPath dist/zh-cn
$browser = Get-ChildItem dist/zh-cn -Filter camoufox.exe -Recurse | Select-Object -First 1
$profile = Join-Path $env:TEMP ([guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $profile | Out-Null
& $browser.FullName -no-remote -profile $profile about:preferences
```

The settings page should be in Simplified Chinese. In the web console,
`navigator.language` should be `zh-CN`. For this Firefox version, a direct
launch without locale overrides reports `navigator.languages` as
`["zh-CN", "zh", "zh-TW", "zh-HK", "en-US", "en"]`; these are Firefox's
standard Simplified Chinese preferences, including fallback languages.

Packaging matters: `scripts/package.py` fetches language packs for the exact
Firefox version and embeds them using `scripts/inject-locales.py`. The raw
build directory only has English resources until this step runs.

Mozilla documents locale selection and packaged resources in
[Locale management](https://firefox-source-docs.mozilla.org/intl/locale.html).
