import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { viteSingleFile } from 'vite-plugin-singlefile'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

// 版本号唯一来源：C++ 侧的 AccountManager/version.h（.rc、User-Agent 也用它）
const versionHeaderPath = fileURLToPath(new URL('../AccountManager/version.h', import.meta.url))
const versionHeader = readFileSync(versionHeaderPath, 'utf8')

const readVersionNumber = (name) => {
  const matched = new RegExp(`^\\s*#define\\s+${name}\\s+(\\d+)`, 'm').exec(versionHeader)
  if (!matched) {
    throw new Error(`无法从 AccountManager/version.h 读取 ${name}，请检查该文件的 #define`)
  }
  return Number(matched[1])
}

// 界面显示用：主.次.修订（与 version.h 的 APP_VERSION_LABEL 一致）
const APP_VERSION = [
  readVersionNumber('APP_VERSION_MAJOR'),
  readVersionNumber('APP_VERSION_MINOR'),
  readVersionNumber('APP_VERSION_PATCH'),
].join('.')

export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  base: '/',
  // 界面里直接引用 __APP_VERSION__ 即可，改版本只需改 version.h
  define: {
    __APP_VERSION__: JSON.stringify(APP_VERSION),
  },
  build: {
    assetsInlineLimit: 100000000, // 确保图片也会被转为 Base64
  }
})
