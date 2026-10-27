import { build } from 'esbuild';
import { mkdir, writeFile, readFile } from 'node:fs/promises';
import { completionHeader } from './completions.mjs';

await mkdir('dist', { recursive: true });
const result = await build({
  entryPoints: ['runtime.mjs'], bundle: true, format: 'iife', platform: 'browser',
  alias: {'fraction.js': './fraction-native.mjs'},
  target: 'es2022', minify: true, write: false, legalComments: 'inline',
  define: { 'process.env.NODE_ENV': '"production"' },
  plugins: [{name:'native-pure-query',setup(builder) {
    builder.onLoad({filter:/[\\/]@strudel[\\/]core[\\/]dist[\\/]index\.mjs$/},async ({path})=>{
      const source=await readFile(path,'utf8');
      const query='return s.span.spanCycles.map((r) => new S(m(r.begin).wholeCycle(), r, t));';
      if (source.split(query).length!==2) throw Error('Pinned pure query changed; inspect the native adapter');
      const common='const s = Object.keys(t).filter((r) => Object.keys(e).includes(r));';
      if (source.split(common).length!==2) throw Error('Pinned common-key query changed; inspect the native adapter');
      const contents=source.replace(query,
        'if(typeof globalThis.__hostPure==="function"){const native=globalThis.__hostPure(s,t);if(native!==null)return native;}'+query)
        .replace(common,'let s=typeof globalThis.__hostCommonKeys==="function"?globalThis.__hostCommonKeys(t,e):null;'+
          'if(s===null)s=Object.keys(t).filter((r)=>Object.keys(e).includes(r));');
      return {loader:'js',contents};
    });
  }}],
});
await writeFile('dist/runtime.js', result.outputFiles[0].contents);
await writeFile('dist/completions.h', await completionHeader(result.outputFiles[0].text));
await import('./reference.mjs');

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
