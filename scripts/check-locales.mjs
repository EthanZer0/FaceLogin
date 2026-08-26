import { readFile, readdir } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = dirname(dirname(fileURLToPath(import.meta.url)))
const localeDir = join(root, 'locales')
const sourceName = 'zh-CN.json'
const source = JSON.parse(await readFile(join(localeDir, sourceName), 'utf8'))
const sourceKeys = new Set(Object.keys(source))
let failed = false

for (const name of (await readdir(localeDir)).filter(name => name.endsWith('.json') && name !== sourceName)) {
  const catalog = JSON.parse(await readFile(join(localeDir, name), 'utf8'))
  const keys = new Set(Object.keys(catalog))
  const missing = [...sourceKeys].filter(key => !keys.has(key))
  const extra = [...keys].filter(key => !sourceKeys.has(key))
  const untranslated = [...sourceKeys].filter(key => key !== 'meta.locale' && catalog[key] === source[key])
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

if (failed) process.exit(1)
console.log(`Locale catalogs are in sync (${sourceKeys.size} keys).`)
