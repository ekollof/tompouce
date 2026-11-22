import { build } from 'esbuild';
import { mkdir, writeFile, readFile } from 'node:fs/promises';

await mkdir('dist', { recursive: true });
await build({ entryPoints: ['editor.mjs'], bundle: true, format: 'iife',
  globalName: 'TompouceEditor', platform: 'browser', target: 'es2022',
  minify: true, legalComments: 'inline', outfile: 'dist/editor.js' });
// H() reuses the engine's pinned Strudel packages. Resolve them from js/ so
// the visualizer cannot drift from playback mini-notation.
await build({ entryPoints: ['patterns.mjs'], bundle: true, format: 'iife',
  platform: 'browser', target: 'es2022', nodePaths: ['../js/node_modules'],
  minify: true, legalComments: 'inline', outfile: 'dist/patterns.js' });
const lock = JSON.parse(await readFile('package-lock.json', 'utf8'));
const notices = [];
for (const directory of Object.keys(lock.packages).sort()) {
  if (!directory) continue;
  let pkg;
  try { pkg = JSON.parse(await readFile(`${directory}/package.json`, 'utf8')); }
  catch (error) { if (error.code === 'ENOENT') continue; throw error; }
  let license = '';
  for (const name of ['LICENSE', 'LICENSE.md', 'LICENSE.txt']) {
    try { license = await readFile(`${directory}/${name}`, 'utf8'); break; }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  notices.push(`${pkg.name} ${pkg.version}\nLicense: ${JSON.stringify(pkg.license)}\n${license}`);
}
await writeFile('dist/licenses.txt', notices.join('\n\n----------------------------------------\n\n'));
