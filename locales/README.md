# FaceLogin locale packs

Locale packs are UTF-8 JSON files with flat, stable message keys. The file
name must be a canonical BCP 47 language tag such as `zh-CN` or `ko-KR`.

`zh-CN.json` is the source catalog and final fallback. Every other locale must
contain the same keys. Run `node scripts/check-locales.mjs` before submitting a
change; it reports missing, extra, and untranslated entries.

The installer, enrollment console, service, and credential provider all use
these files. `scripts/sync-locales.mjs` copies them into the installer's
embedded resources during a build, so installed locale packs remain separate
files under `locales/` and can be updated independently.

Locale resolution order:

1. The explicit `ui_language` setting, when present.
2. The current Windows display language.
3. `zh-CN`.

Keep placeholders such as `{count}`, `{distance}`, and `{path}` unchanged.
