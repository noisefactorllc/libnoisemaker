import { writeFileSync } from 'node:fs'

export function partitionFixtureFiles (files, partition) {
  if (!partition) return files
  const { index, count } = partition
  if (!Number.isSafeInteger(index) || !Number.isSafeInteger(count) ||
      count < 1 || count > 16 || index < 0 || index >= count) {
    throw new Error('invalid fixture partition')
  }
  const selected = files.filter((_, ordinal) => ordinal % count === index)
  if (process.env.NM_GATE_FIXTURE_REPORT) {
    writeFileSync(process.env.NM_GATE_FIXTURE_REPORT, JSON.stringify(selected.map(file => file.path)))
  }
  return selected
}
