import { copyFile, mkdir, readdir } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = dirname(dirname(fileURLToPath(import.meta.url)))
const source = join(root, 'locales')
const destination = join(root, 'installer', 'FaceLoginSetup', 'resources', 'locales')

await mkdir(destination, { recursive: true })
for (const entry of await readdir(source, { withFileTypes: true })) {
  if (entry.isFile() && entry.name.endsWith('.json')) {
    await copyFile(join(source, entry.name), join(destination, entry.name))
  }
}
