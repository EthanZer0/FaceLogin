import { readFile, readdir } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = dirname(dirname(fileURLToPath(import.meta.url)))
const localeDir = join(root, 'locales')
const sourceName = 'zh-CN.json'
const source = JSON.parse(await readFile(join(localeDir, sourceName), 'utf8'))
const sourceKeys = new Set(Object.keys(source))
let failed = false

// Language names are intentionally self-labeled (each pack spells every
// language in its own script — 简体中文/한국어/English), so their values are
// IDENTICAL across packs by design and must not count as "untranslated".
const SELF_LABELED_PREFIXES = ['console.settings.language']

const isSelfLabeled = key => SELF_LABELED_PREFIXES.some(prefix => key.startsWith(prefix))

// Keys whose zh-CN value deliberately differs from the mapped DOM source
// string: the DOM text is a static placeholder that JS replaces with a
// formatted t() call before it becomes visible (the append-info "N/N+1"
// text, the refresh dialog body), so the pack carries the formatted
// template with {placeholders} instead of the placeholder text.
const SOURCE_PARITY_ALLOWLIST = new Set([
  'console.faces.appendInfo',
  'console.refresh.description',
])
for (const name of (await readdir(localeDir)).filter(name => name.endsWith('.json') && name !== sourceName)) {
  const catalog = JSON.parse(await readFile(join(localeDir, name), 'utf8'))
  const keys = new Set(Object.keys(catalog))
  const missing = [...sourceKeys].filter(key => !keys.has(key))
  const extra = [...keys].filter(key => !sourceKeys.has(key))
  const untranslated = [...sourceKeys].filter(
    key => key !== 'meta.locale' && !isSelfLabeled(key) && catalog[key] === source[key]
  )
  const placeholderMismatch = [...sourceKeys].filter(key => {
    if (!(key in catalog)) return false
    const sourcePlaceholders = [...source[key].matchAll(/\{([a-zA-Z0-9_]+)\}/g)].map(match => match[1]).sort()
    const targetPlaceholders = [...catalog[key].matchAll(/\{([a-zA-Z0-9_]+)\}/g)].map(match => match[1]).sort()
    return sourcePlaceholders.join('\0') !== targetPlaceholders.join('\0')
  })
  if (missing.length || extra.length || untranslated.length || placeholderMismatch.length) {
    failed = true
    if (missing.length) console.error(`${name}: missing keys: ${missing.join(', ')}`)
    if (extra.length) console.error(`${name}: extra keys: ${extra.join(', ')}`)
    if (untranslated.length) console.error(`${name}: untranslated values: ${untranslated.join(', ')}`)
    if (placeholderMismatch.length) console.error(`${name}: placeholder mismatch: ${placeholderMismatch.join(', ')}`)
  }
}

// ============================================================================
// Console i18n consistency (enrollment_app/index.html)
//
// The Console's static text lives in the DOM as Chinese source strings, and
// STATIC_TEXT_KEYS / STATIC_PLACEHOLDER_KEYS / RUNTIME_TEXT_KEYS map each
// exact source string to a locale key. The zh-CN pack must therefore match
// those source strings byte-for-byte — otherwise applyI18n's TreeWalker
// cannot find the nodes and the UI silently shows raw keys (the tooltip
// drift bug). Every mapped key must also exist in all three packs.
// ============================================================================

const htmlPath = join(root, 'enrollment_app', 'index.html')
const html = await readFile(htmlPath, 'utf8')

// Decode the JS string literals used as mapping keys/values. Only the
// escapes that actually occur in index.html are handled: \\ and \uXXXX.
const unescapeJs = s =>
  s.replace(/\\u([0-9a-fA-F]{4})/g, (_, hex) => String.fromCharCode(parseInt(hex, 16)))
   .replace(/\\\\/g, '\\')

const parseBlock = blockName => {
  const match = html.match(new RegExp(`var ${blockName} = \\{([\\s\\S]*?)\\};`))
  if (!match) return []
  const entries = [...match[1].matchAll(/'((?:[^'\\]|\\.)*)'\s*:\s*'((?:[^'\\]|\\.)*)'/g)]
  return entries.map(([, sourceText, localeKey]) => ({
    sourceText: unescapeJs(sourceText),
    localeKey: unescapeJs(localeKey),
  }))
}

const mappings = [
  ...parseBlock('STATIC_TEXT_KEYS'),
  ...parseBlock('STATIC_PLACEHOLDER_KEYS'),
  ...parseBlock('RUNTIME_TEXT_KEYS'),
]

const seen = new Set()
for (const { sourceText, localeKey } of mappings) {
  if (seen.has(localeKey)) continue
  seen.add(localeKey)
  if (!sourceKeys.has(localeKey)) {
    failed = true
    console.error(`index.html: mapped key '${localeKey}' (source '${sourceText.slice(0, 40)}…') missing from ${sourceName}`)
    continue
  }
  if (source[localeKey] !== sourceText && !SOURCE_PARITY_ALLOWLIST.has(localeKey)) {
    failed = true
    console.error(`index.html: zh-CN value for '${localeKey}' does not match the source string:\n  zh-CN: ${JSON.stringify(source[localeKey])}\n  DOM:   ${JSON.stringify(sourceText)}`)
  }
  for (const name of (await readdir(localeDir)).filter(n => n.endsWith('.json') && n !== sourceName)) {
    const catalog = JSON.parse(await readFile(join(localeDir, name), 'utf8'))
    if (!(localeKey in catalog)) {
      failed = true
      console.error(`index.html: mapped key '${localeKey}' missing from ${name}`)
    }
  }
}

if (failed) process.exit(1)
console.log(`Locale catalogs are in sync (${sourceKeys.size} keys); Console mappings match the zh-CN pack (${seen.size} keys).`)
