import { build } from 'esbuild';
import { mkdir, writeFile, readFile } from 'node:fs/promises';

await mkdir('dist', { recursive: true });
const result = await build({
  entryPoints: ['runtime.mjs'], bundle: true, format: 'iife', platform: 'browser',
  target: 'es2022', minify: true, write: false, legalComments: 'inline',
  define: { 'process.env.NODE_ENV': '"production"' },
});
await writeFile('dist/runtime.js', result.outputFiles[0].contents);

const lock = JSON.parse(await readFile('package-lock.json', 'utf8'));
const notices = [];
for (const path of Object.keys(lock.packages).sort()) {
  if (!path) continue;
  let pkg;
  try { pkg = JSON.parse(await readFile(`${path}/package.json`, 'utf8')); }
  catch (error) { if (error.code === 'ENOENT') continue; throw error; }
  let license = '';
  for (const name of ['LICENSE', 'LICENSE.md', 'LICENSE.txt', 'license', 'license.md', 'LICENCE']) {
    try { license = await readFile(`${path}/${name}`, 'utf8'); break; }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  notices.push(`${pkg.name} ${pkg.version}\nLicense: ${JSON.stringify(pkg.license ?? 'See upstream package')}\n${license}`);
}
await writeFile('dist/licenses.txt', notices.join('\n\n----------------------------------------\n\n'));
