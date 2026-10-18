import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { JSDOM } from '../web/node_modules/jsdom/lib/api.js';
const dom = new JSDOM(await readFile(new URL('../web/index.html', import.meta.url), 'utf8'), {runScripts: 'outside-only'});
const { window } = dom;
const calls = [], inserted = [];
window.TompouceCompanion = {
  async tool(name, args) {
    calls.push({name, args});
    return JSON.stringify({total: 1, items: [{name: '<unsafe>', slots: 2, code: 's("bd")'}]});
  },
  insertSnippet(code) { inserted.push(code); },
};
window.eval(await readFile(new URL('../web/sounds.js', import.meta.url), 'utf8'));
await new Promise(resolve => setTimeout(resolve, 10));
assert.equal(calls[0].args.category, 'samples');
assert.equal(window.document.querySelectorAll('#sound-tabs button').length, 6);
assert.equal(window.document.querySelector('#sound-results span').textContent, '<unsafe> · 2 samples');
assert.equal(window.document.querySelector('#sound-results unsafe'), null);
window.document.querySelector('#sound-results button').click();
assert.deepEqual(inserted, ['s("bd")']);
window.document.querySelectorAll('#sound-tabs button')[2].click();
await new Promise(resolve => setTimeout(resolve, 10));
assert.equal(calls.at(-1).args.category, 'synths');
const query = window.document.getElementById('sound-query');
query.value = 'piano'; query.dispatchEvent(new window.Event('input'));
await new Promise(resolve => setTimeout(resolve, 180));
assert.equal(calls.at(-1).args.query, 'piano');
window.document.querySelectorAll('#sound-tabs button')[5].click();
assert.equal(window.document.getElementById('sound-import').hidden, false);
assert.equal(query.disabled, true);
const uploads = [];
window.fetch = async (url, options) => { uploads.push({url, options}); return {ok: true, json: async () => ({name: 'my_drums', slots: 1})}; };
const input = window.document.getElementById('sound-files');
Object.defineProperty(input, 'files', {value: [{name: 'kick.wav', size: 100, webkitRelativePath: 'My Drums/kick.wav'}]});
await input.onchange({target: input});
assert.equal(uploads[0].url, '/api/import');
assert.equal(uploads[0].options.headers['X-Sound-Name'], 'my_drums');
assert.equal(calls.at(-1).args.category, 'user');
assert.match(window.document.getElementById('sound-status').textContent, /Imported 1 files/);
window.close();
console.log('companion sound tabs, search, safe rendering, snippet insertion and import view ok');
