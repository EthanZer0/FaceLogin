<script lang="ts" setup>
import { ref, computed, onMounted } from 'vue'
import { GetDefaultPaths, Install, Uninstall, PickDirectory, IsInstalled, GetUpgradeNotice } from '../wailsjs/go/main/App'
import { EventsOn, EventsOff } from '../wailsjs/runtime'
import { activeLocale, availableLocales, localePreference, setLocale, t, noticeT } from './i18n'

const installDir = ref('')
const showInstall = ref(true)
const running = ref(false)
const progressPercent = ref(0)
const progressStep = ref('')
const progressStatus = ref('')
const progressDetail = ref('')
const resultMessage = ref('')
const resultSuccess = ref(false)
const showResult = ref(false)
const alreadyInstalled = ref(false)

// Upgrade notice ("what's new") popup state
const notice = ref<any>(null)
const showNotice = ref(false)

function localizeBackendMessage(message: string): string {
  if (!message) return ''
  try {
    const payload = JSON.parse(message)
    if (payload && typeof payload.key === 'string') {
      return t(payload.key, payload.values || {})
    }
  } catch (_) {
    // A plain locale key or an OS/library error is still a valid message.
  }
  return t(message)
}

// Notice body lines, filtered to non-empty (rendered as bullets). The whole
// body is ONE locale key (installer.notice.body) whose value is a newline-
// separated list — no group headers, so the announcement stays a plain
// bullet list (see notice-zh/en.json).
const noticeLines = computed(() =>
  notice.value && notice.value.body
    ? noticeT(notice.value.body).split('\n').filter((l: string) => l.trim() !== '')
    : []
)

// Notice body parsed into {group, items} sections. The current announcement
// format is a flat bullet list (single installer.notice.body key), which
// lands in one unnamed section here. Group headers ("新功能：" etc.) are
// still tolerated for older notice formats: a line ending with '：' starts
// a section whose following lines belong to it.
interface NoticeSection { group: string; items: string[]; important?: boolean }
const noticeSections = computed<NoticeSection[]>(() => {
  const lines = noticeLines.value
  const sections: NoticeSection[] = []
  let cur: NoticeSection | null = null
  for (const raw of lines) {
    const line = raw.replace(/^[-•]\s*/, '').trim()
    if (!line) continue
    // Group header: "前缀：" on its own line (e.g. "新功能：", "修复：")
    const m = line.match(/^(.+?)[:：]$/)
    if (m) {
      // Sections whose title contains 重要/⚠ are emphasized in red — used for
      // upgrade-critical warnings (e.g. "⚠️ 重要提醒：升级后需重新录入人脸").
      const important = /重要|중요|⚠|警告|경고/.test(m[1])
      cur = { group: m[1], items: [], important }
      sections.push(cur)
      continue
    }
    if (!cur) { cur = { group: '', items: [] }; sections.push(cur) }
    cur.items.push(line)
  }
  return sections.filter(s => s.items.length > 0)
})

onMounted(async () => {
  const paths = await GetDefaultPaths()
  installDir.value = paths.installDir
  alreadyInstalled.value = await IsInstalled()
})

async function doPickDirectory() {
  const dir = await PickDirectory(t('installer.selectInstallDirectory'))
  if (dir) {
    installDir.value = dir
  }
}

function toggleMode(mode: string) {
  showInstall.value = mode === 'install'
  showResult.value = false
  progressStatus.value = ''
}

async function doInstall() {
  running.value = true
  showResult.value = false
  progressPercent.value = 0
  progressStatus.value = 'running'

  EventsOn('setup:progress', (e: any) => {
    progressPercent.value = e.percent
    progressStep.value = e.step
    progressStatus.value = e.status
    progressDetail.value = localizeBackendMessage(e.detail || '')
    if (e.percent >= 100) {
      running.value = false
      EventsOff('setup:progress')
    }
  })

  const result = await Install(installDir.value, localePreference.value)
  resultMessage.value = localizeBackendMessage(result.message)
  resultSuccess.value = result.success
  showResult.value = true
  running.value = false

  // After a successful UPGRADE install, check for a per-release "what's new"
  // announcement and show it as a popup. Fresh installs get none.
  if (result.success && alreadyInstalled.value) {
    const n = await GetUpgradeNotice()
    if (n && n.title) {
      notice.value = n
      showNotice.value = true
    }
  }
}

async function doUninstall() {
  if (!confirm(t('installer.uninstallConfirm'))) return

  running.value = true
  showResult.value = false
  progressPercent.value = 0
  progressStatus.value = 'running'

  EventsOn('setup:progress', (e: any) => {
    progressPercent.value = e.percent
    progressStep.value = e.step
    progressStatus.value = e.status
    progressDetail.value = localizeBackendMessage(e.detail || '')
    if (e.percent >= 100) {
      running.value = false
      EventsOff('setup:progress')
    }
  })

  const result = await Uninstall()
  resultMessage.value = localizeBackendMessage(result.message)
  resultSuccess.value = result.success
  showResult.value = true
  running.value = false
}
</script>

<template>
  <div class="flex flex-col h-screen bg-white select-none">
    <!-- Header -->
    <div class="px-8 pt-8 pb-2 flex items-start justify-between gap-4">
      <div>
        <h1 class="text-2xl font-light tracking-tight text-gray-900">FaceLogin</h1>
        <p class="text-sm text-gray-400 font-light">{{ t('installer.subtitle') }}</p>
      </div>
      <label class="flex flex-col gap-1 text-xs text-gray-400">
        <span>{{ t('installer.language') }}</span>
        <select
          class="px-2 py-1 text-xs text-gray-700 border border-gray-200 bg-white focus:outline-none focus:border-gray-400"
          :value="localePreference"
          :disabled="running"
          @change="setLocale(($event.target as HTMLSelectElement).value)"
        >
          <option value="auto">{{ t('installer.language.auto') }}</option>
          <option v-for="item in availableLocales" :key="item.locale" :value="item.locale">{{ item.name }}</option>
        </select>
      </label>
    </div>

    <!-- Mode Tabs -->
    <div class="px-8 mt-4 flex gap-6 border-b border-gray-100">
      <button
        :class="['pb-2 text-sm font-medium transition-colors',
                 showInstall ? 'text-gray-900 border-b-2 border-gray-900' : 'text-gray-400 hover:text-gray-600']"
        @click="toggleMode('install')"
        :disabled="running"
      >{{ alreadyInstalled ? t('installer.update') : t('installer.install') }}</button>
      <button
        :class="['pb-2 text-sm font-medium transition-colors',
                 !showInstall ? 'text-gray-900 border-b-2 border-gray-900' : 'text-gray-400 hover:text-gray-600']"
        @click="toggleMode('uninstall')"
        :disabled="running"
      >{{ t('installer.uninstall') }}</button>
    </div>

    <!-- Body -->
    <div class="flex-1 px-8 py-6">
      <!-- Install mode -->
      <div v-if="showInstall && !running && !showResult">
        <label class="block text-xs text-gray-500 uppercase tracking-wider mb-2">{{ t('installer.installDirectory') }}</label>
        <div class="flex items-center gap-3">
          <input
            v-model="installDir"
            class="flex-1 px-3 py-2 text-sm border border-gray-200 bg-gray-50
                   focus:outline-none focus:border-gray-400 transition-colors text-gray-800"
            :placeholder="t('installer.selectInstallDirectory')"
          />
          <button
            class="flex-shrink-0 w-9 h-9 flex items-center justify-center border border-gray-200
                   hover:bg-gray-100 transition-colors text-gray-500"
            @click="doPickDirectory"
            :title="t('installer.selectFolder')"
          >
            <svg xmlns="http://www.w3.org/2000/svg" class="w-4 h-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>
            </svg>
          </button>
        </div>
        <p class="mt-1 text-xs text-gray-400">{{ t('installer.modelsHint') }}</p>

        <button
          class="mt-6 w-full py-2.5 text-sm font-medium bg-gray-900 text-white
                 hover:bg-gray-800 transition-colors disabled:opacity-40"
          @click="doInstall"
          :disabled="!installDir"
        >{{ alreadyInstalled ? t('installer.update') : t('installer.install') }}</button>
      </div>

      <!-- Uninstall mode -->
      <div v-if="!showInstall && !running && !showResult">
        <p class="text-sm text-gray-600 leading-relaxed">
          {{ t('installer.uninstallDescription.before') }}
          <strong>{{ t('installer.uninstallDescription.strong') }}</strong>{{ t('installer.uninstallDescription.after') }}
        </p>
        <button
          class="mt-6 w-full py-2.5 text-sm font-medium border border-gray-300 text-gray-700
                 hover:bg-gray-100 transition-colors disabled:opacity-40"
          @click="doUninstall"
        >{{ t('installer.uninstallButton') }}</button>
      </div>

      <!-- Progress -->
      <div v-if="running" class="space-y-4">
        <div class="relative h-0.5 bg-gray-100">
          <div
            class="absolute top-0 left-0 h-full bg-gray-900 transition-all duration-300 ease-out"
            :style="{ width: progressPercent + '%' }"
          ></div>
        </div>
        <div class="flex items-center justify-between">
          <span class="text-sm text-gray-800">{{ t(progressStep) }}</span>
          <span class="text-xs text-gray-400">{{ progressPercent }}%</span>
        </div>
        <p v-if="progressDetail" class="text-xs text-gray-400">{{ progressDetail }}</p>
      </div>

      <!-- Result -->
      <div v-if="showResult && !running" class="space-y-4">
        <div
          :class="['text-sm whitespace-pre-line leading-relaxed',
                   resultSuccess ? 'text-gray-800' : 'text-red-600']"
        >
          {{ resultSuccess ? '✓ ' : '✗ ' }}{{ resultMessage }}
        </div>
        <button
          class="w-full py-2 text-sm text-gray-500 border border-gray-200
                 hover:bg-gray-50 transition-colors"
          @click="showResult = false"
        >{{ t('installer.back') }}</button>
      </div>
    </div>

    <!-- Footer -->
    <div class="px-8 py-4 border-t border-gray-100">
      <p class="text-xs text-gray-300">{{ t('installer.footer') }}</p>
    </div>
  </div>

  <!-- Upgrade notice popup ("what's new") — only after a successful upgrade -->
  <Transition name="notice">
    <div
      v-if="showNotice && notice"
      class="notice-overlay fixed inset-0 z-50 flex items-center justify-center p-6"
      @click.self="showNotice = false"
    >
      <div class="notice-card w-[90%] max-w-md max-h-[80vh] flex flex-col">
        <!-- Header -->
        <div class="px-6 pt-6 pb-4 border-b border-gray-100 flex items-start justify-between gap-4">
          <div class="flex flex-col gap-2.5">
            <!-- Brand: corner-marks + FaceLogin name -->
            <div class="flex items-center gap-2">
              <span class="notice-mark" aria-hidden="true"><i></i><i></i><i></i><i></i></span>
              <span class="text-sm font-medium tracking-wide text-gray-900">FaceLogin</span>
            </div>
            <div class="flex items-center gap-2">
              <span class="notice-ver-badge">v{{ notice.version }}</span>
              <span class="text-xs text-gray-400 font-light">{{ t('installer.releaseNotice') }}</span>
            </div>
            <h2 class="text-lg font-medium text-gray-900 leading-snug">{{ noticeT(notice.title) }}</h2>
          </div>
          <button
            class="notice-close text-gray-400 hover:text-gray-600 text-xl leading-none mt-0.5"
            @click="showNotice = false"
            :title="t('installer.close')"
          >×</button>
        </div>

        <!-- Body: grouped sections -->
        <div class="px-6 py-5 overflow-y-auto notice-scroll">
          <template v-for="(sec, si) in noticeSections" :key="si">
            <div
              v-if="sec.group"
              class="notice-group-title"
              :class="sec.important ? 'notice-group-title--important' : ''"
            >{{ sec.group }}</div>
            <ul class="notice-list" :class="sec.group ? 'mb-4' : ''">
              <li
                v-for="(line, i) in sec.items"
                :key="i"
                class="flex gap-2.5 text-sm leading-relaxed"
                :class="sec.important ? 'text-red-700 font-medium' : 'text-gray-600'"
              >
                <span
                  class="notice-dot mt-[7px] flex-shrink-0"
                  :class="sec.important ? 'notice-dot--important' : ''"
                  aria-hidden="true"
                ></span>
                <span>{{ line }}</span>
              </li>
            </ul>
          </template>
        </div>

        <!-- Footer -->
        <div class="px-6 py-4 border-t border-gray-100 flex justify-end">
          <button
            class="notice-btn px-5 py-1.5 text-sm font-medium text-white"
            @click="showNotice = false"
          >{{ t('installer.acknowledge') }}</button>
        </div>
      </div>
    </div>
  </Transition>
</template>

<style scoped>
/* ============================================================
   Upgrade notice — brand-green redesign (1.6.0)
   Overlay: deep tint + brand glow + backdrop blur.
   Card:    rounded, layered shadow, viewfinder brand-mark,
            grouped sections, brand-green CTA.
   Motion:  overlay fade + card scale/translate with overshoot.
   ============================================================ */
.notice-overlay {
  background:
    radial-gradient(ellipse at 50% 42%, rgba(14, 159, 110, 0.10), transparent 62%),
    rgba(17, 24, 39, 0.46);
  -webkit-backdrop-filter: blur(4px);
  backdrop-filter: blur(4px);
}

.notice-card {
  background: #ffffff;
  border-radius: 10px;
  box-shadow: 0 24px 60px -14px rgba(0, 0, 0, 0.30),
              0 4px 14px rgba(0, 0, 0, 0.08);
  overflow: hidden;
  position: relative;
}

/* Brand mark — the FaceLogin "viewfinder" signature, identical to the
   Console header's .brand-mark: a rounded frame with corner-markers inside.
   Keeping both products on the same brand language. */
.notice-mark {
  width: 30px; height: 30px;
  position: relative;
  flex-shrink: 0;
  border: 1px solid #E2E5EA;
  border-radius: 3px;
}
.notice-mark i {
  position: absolute;
  width: 8px; height: 8px;
  border: 2px solid #0E9F6E;
  opacity: 0.85;
}
.notice-mark i:nth-child(1){top:0;left:0;border-right:0;border-bottom:0}
.notice-mark i:nth-child(2){top:0;right:0;border-left:0;border-bottom:0}
.notice-mark i:nth-child(3){bottom:0;left:0;border-right:0;border-top:0}
.notice-mark i:nth-child(4){bottom:0;right:0;border-left:0;border-top:0}

.notice-ver-badge {
  display: inline-flex;
  align-items: center;
  padding: 1px 8px;
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0.04em;
  color: #ffffff;
  background: #0E9F6E;
  border-radius: 4px;
}

.notice-close {
  width: 28px; height: 28px;
  display: flex; align-items: center; justify-content: center;
  border-radius: 6px;
  transition: background-color 0.15s ease, color 0.15s ease;
}
.notice-close:hover {
  background: #F3F4F6;
  color: #374151;
}

/* Group headers (新功能 / 修复 / 说明 ...) */
.notice-group-title {
  font-size: 12px;
  font-weight: 600;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  color: #0E9F6E;
  margin-bottom: 8px;
  display: flex;
  align-items: center;
  gap: 8px;
}
.notice-group-title::after {
  content: '';
  flex: 1;
  height: 1px;
  background: #E5E7EB;
}

/* Important (⚠️) group: red title + red rule — used for upgrade-critical
   warnings like "升级后需重新录入人脸". */
.notice-group-title--important {
  color: #DC2626;
}
.notice-group-title--important::after {
  background: #FECACA;
}

.notice-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

/* Green bullet dot */
.notice-dot {
  width: 6px; height: 6px;
  border-radius: 9999px;
  background: #0E9F6E;
  opacity: 0.75;
}
.notice-dot--important {
  background: #DC2626;
  opacity: 1;
}

/* Brand CTA */
.notice-btn {
  background: #0E9F6E;
  border-radius: 6px;
  transition: background-color 0.15s ease, transform 0.1s ease;
}
.notice-btn:hover { background: #0B8A5E; }
.notice-btn:active { transform: translateY(1px); }

/* Custom scrollbar for the body (subtle, brand-tinted) */
.notice-scroll::-webkit-scrollbar { width: 6px; }
.notice-scroll::-webkit-scrollbar-thumb {
  background: #D1D5DB;
  border-radius: 9999px;
}
.notice-scroll::-webkit-scrollbar-thumb:hover { background: #9CA3AF; }

/* Entry motion */
.notice-enter-active { transition: opacity 0.22s ease; }
.notice-enter-active .notice-card { transition: transform 0.38s cubic-bezier(0.16, 1, 0.3, 1), opacity 0.3s ease; }
.notice-leave-active { transition: opacity 0.15s ease; }
.notice-leave-active .notice-card { transition: transform 0.18s ease, opacity 0.15s ease; }
.notice-enter-from { opacity: 0; }
.notice-enter-from .notice-card { transform: translateY(14px) scale(0.96); opacity: 0; }
.notice-leave-to { opacity: 0; }
.notice-leave-to .notice-card { transform: translateY(8px) scale(0.98); opacity: 0; }
</style>
