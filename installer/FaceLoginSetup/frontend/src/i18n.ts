import { computed, ref } from 'vue'
import { WindowSetTitle } from '../wailsjs/runtime/runtime'
import zhCNText from '../../../../locales/zh-CN.json?raw'
import koKRText from '../../../../locales/ko-KR.json?raw'
import enUSText from '../../../../locales/en-US.json?raw'
// The upgrade announcement is deliberately NOT part of the locale packs: it
// is installer-only content, shipped in exactly two languages (Chinese for
// zh-CN, English for everything else) so release notes never pollute the
// shared packs.
import noticeZhText from './notice-zh.json?raw'
import noticeEnText from './notice-en.json?raw'

type Catalog = Record<string, string>

const FALLBACK_LOCALE = 'zh-CN'
const catalogs: Record<string, Catalog> = {
  'zh-CN': JSON.parse(zhCNText),
  'ko-KR': JSON.parse(koKRText),
  'en-US': JSON.parse(enUSText),
}

// Upgrade-announcement catalog: zh-CN reads Chinese, every other language
// reads English (exactly the two-language policy requested for release notes).
const noticeCatalog: Record<string, Catalog> = {
  'zh-CN': JSON.parse(noticeZhText),
  'en-US': JSON.parse(noticeEnText),
}

function normalizeLocale(locale: string | null | undefined): string {
  if (!locale || locale === 'auto') {
    locale = navigator.language
  }
  const normalized = locale.replace('_', '-').toLowerCase()
  if (normalized === 'en' || normalized.startsWith('en-')) return 'en-US'
  if (normalized === 'ko' || normalized.startsWith('ko-')) return 'ko-KR'
  if (normalized === 'zh' || normalized.startsWith('zh-')) return 'zh-CN'
  return FALLBACK_LOCALE
}

const savedLocale = localStorage.getItem('facelogin.uiLanguage') || 'auto'
export const localePreference = ref(savedLocale)
export const activeLocale = computed(() => normalizeLocale(localePreference.value))
export const availableLocales = Object.keys(catalogs).map(locale => ({
  locale,
  name: catalogs[locale]['meta.languageName'],
}))

export function setLocale(locale: string): void {
  localePreference.value = locale
  localStorage.setItem('facelogin.uiLanguage', locale)
  document.documentElement.lang = activeLocale.value
  document.title = t('installer.windowTitle')
  try {
    WindowSetTitle(t('installer.windowTitle'))
  } catch (_) {
    // Plain-browser previews do not expose the Wails runtime.
  }
}

export function t(key: string, values: Record<string, string | number> = {}): string {
  const catalog = catalogs[activeLocale.value] || catalogs[FALLBACK_LOCALE]
  let message = catalog[key] ?? catalogs[FALLBACK_LOCALE][key] ?? key
  for (const [name, value] of Object.entries(values)) {
    message = message.replaceAll(`{${name}}`, String(value))
  }
  return message
}

// Translate an upgrade-notice key (title/body lines). Chinese UI reads the
// Chinese announcement, every other UI language reads the English one; an
// unknown key degrades to the key itself.
export function noticeT(key: string): string {
  const lang = activeLocale.value === 'zh-CN' ? 'zh-CN' : 'en-US'
  return noticeCatalog[lang][key] ?? key
}

setLocale(savedLocale)
