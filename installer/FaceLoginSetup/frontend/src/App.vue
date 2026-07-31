<script lang="ts" setup>
import { ref, onMounted } from 'vue'
import { GetDefaultPaths, Install, Uninstall, PickDirectory, IsInstalled } from '../wailsjs/go/main/App'
import { EventsOn, EventsOff } from '../wailsjs/runtime'

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

onMounted(async () => {
  const paths = await GetDefaultPaths()
  installDir.value = paths.installDir
  alreadyInstalled.value = await IsInstalled()
})

async function doPickDirectory() {
  const dir = await PickDirectory()
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
    progressDetail.value = e.detail || ''
    if (e.percent >= 100) {
      running.value = false
      EventsOff('setup:progress')
    }
  })

  const result = await Install(installDir.value)
  resultMessage.value = result.message
  resultSuccess.value = result.success
  showResult.value = true
  running.value = false
}

async function doUninstall() {
  if (!confirm('确定要卸载 FaceLogin 人脸登录吗？')) return

  running.value = true
  showResult.value = false
  progressPercent.value = 0
  progressStatus.value = 'running'

  EventsOn('setup:progress', (e: any) => {
    progressPercent.value = e.percent
    progressStep.value = e.step
    progressStatus.value = e.status
    progressDetail.value = e.detail || ''
    if (e.percent >= 100) {
      running.value = false
      EventsOff('setup:progress')
    }
  })

  const result = await Uninstall()
  resultMessage.value = result.message
  resultSuccess.value = result.success
  showResult.value = true
  running.value = false
}
</script>

<template>
  <div class="flex flex-col h-screen bg-white select-none">
    <!-- Header -->
    <div class="px-8 pt-8 pb-2">
      <h1 class="text-2xl font-light tracking-tight text-gray-900">FaceLogin</h1>
      <p class="text-sm text-gray-400 font-light">人脸识别登录系统 · 安装程序</p>
    </div>

    <!-- Mode Tabs -->
    <div class="px-8 mt-4 flex gap-6 border-b border-gray-100">
      <button
        :class="['pb-2 text-sm font-medium transition-colors',
                 showInstall ? 'text-gray-900 border-b-2 border-gray-900' : 'text-gray-400 hover:text-gray-600']"
        @click="toggleMode('install')"
        :disabled="running"
      >{{ alreadyInstalled ? '更新' : '安装' }}</button>
      <button
        :class="['pb-2 text-sm font-medium transition-colors',
                 !showInstall ? 'text-gray-900 border-b-2 border-gray-900' : 'text-gray-400 hover:text-gray-600']"
        @click="toggleMode('uninstall')"
        :disabled="running"
      >卸载</button>
    </div>

    <!-- Body -->
    <div class="flex-1 px-8 py-6">
      <!-- Install mode -->
      <div v-if="showInstall && !running && !showResult">
        <label class="block text-xs text-gray-500 uppercase tracking-wider mb-2">安装目录</label>
        <div class="flex items-center gap-3">
          <input
            v-model="installDir"
            class="flex-1 px-3 py-2 text-sm border border-gray-200 bg-gray-50
                   focus:outline-none focus:border-gray-400 transition-colors text-gray-800"
            placeholder="选择安装目录"
          />
          <button
            class="flex-shrink-0 w-9 h-9 flex items-center justify-center border border-gray-200
                   hover:bg-gray-100 transition-colors text-gray-500"
            @click="doPickDirectory"
            title="选择文件夹"
          >
            <svg xmlns="http://www.w3.org/2000/svg" class="w-4 h-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>
            </svg>
          </button>
        </div>
        <p class="mt-1 text-xs text-gray-400">模型文件（约 120 MB）将安装到该目录下的 models/ 子目录</p>

        <button
          class="mt-6 w-full py-2.5 text-sm font-medium bg-gray-900 text-white
                 hover:bg-gray-800 transition-colors disabled:opacity-40"
          @click="doInstall"
          :disabled="!installDir"
        >{{ alreadyInstalled ? '更新' : '安装' }}</button>
      </div>

      <!-- Uninstall mode -->
      <div v-if="!showInstall && !running && !showResult">
        <p class="text-sm text-gray-600 leading-relaxed">
          卸载将停止并删除 FaceLogin 服务、注销登录组件、删除程序文件。用户数据和日志将保留。
        </p>
        <button
          class="mt-6 w-full py-2.5 text-sm font-medium border border-gray-300 text-gray-700
                 hover:bg-gray-100 transition-colors disabled:opacity-40"
          @click="doUninstall"
        >卸载 FaceLogin</button>
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
          <span class="text-sm text-gray-800">{{ progressStep }}</span>
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
        >返回</button>
      </div>
    </div>

    <!-- Footer -->
    <div class="px-8 py-4 border-t border-gray-100">
      <p class="text-xs text-gray-300">Windows 人脸识别登录</p>
    </div>
  </div>
</template>
