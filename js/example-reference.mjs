/* Explicit maintenance command: emit Node/upstream event expectations.
   Tests consume committed expectations and never regenerate them. */
import fs from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const corpusPath = new URL('../tests/fixtures/strudel-examples.json', import.meta.url);
if (process.argv.includes('--worker')) {
  const example = JSON.parse(fs.readFileSync(0, 'utf8'));
  globalThis.__hostLog = () => {};
  globalThis.__hostSlider = (value) => value;
  // Suppress upstream bootstrap output; stdout is the JSON result channel.
  console.log = console.warn = console.error = () => {};
  await import('./dist/runtime.js');
  let result;
  try {
    const { bpm, samples, visuals, csound } = JSON.parse(await __compile(example.code, 120));
    const metadata = { bpm, samples, visuals, csound };
    const cycles = [0, 1, 16].map((begin) => ({
      begin,
      events: JSON.parse(__query(begin, begin + 1)).map(({ begin, end, value }) => ({ begin, end, value })),
    }));
    result = { title: example.title, sha256: example.sha256, metadata, cycles };
  } catch (error) {
    result = { title: example.title, sha256: example.sha256, error: String(error) };
  }
  process.stdout.write(JSON.stringify(result));
} else {
  const corpus = JSON.parse(fs.readFileSync(corpusPath, 'utf8'));
  const examples = corpus.examples.map((example) => {
    const child = spawnSync(process.execPath, [fileURLToPath(import.meta.url), '--worker'], {
      input: JSON.stringify(example), encoding: 'utf8', timeout: 15000, maxBuffer: 8 * 1024 * 1024,
    });
    if (child.status !== 0) throw new Error(`${example.title}: ${child.error ?? child.stderr}`);
    return JSON.parse(child.stdout);
  });
  const result = {
    reference: 'Node.js with pinned upstream Strudel packages and Tompouce host adapter',
    versions: JSON.parse(fs.readFileSync(new URL('./package.json', import.meta.url))).dependencies,
    examples,
  };
  process.stdout.write(JSON.stringify(result, null, 2) + '\n');
}
