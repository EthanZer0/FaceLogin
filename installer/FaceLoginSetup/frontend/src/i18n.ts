import { computed, ref } from 'vue'
import { WindowSetTitle } from '../wailsjs/runtime/runtime'
import zhCNText from '../../../../locales/zh-CN.json?raw'
import koKRText from '../../../../locales/ko-KR.json?raw'

type Catalog = Record<string, string>

const FALLBACK_LOCALE = 'zh-CN'
const catalogs: Record<string, Catalog> = {
  'zh-CN': JSON.parse(zhCNText),
  'ko-KR': JSON.parse(koKRText),
}

function normalizeLocale(locale: string | null | undefined): string {
  if (!locale || locale === 'auto') {
    locale = navigator.language
  }
  const normalized = locale.replace('_', '-').toLowerCase()
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

setLocale(savedLocale)
