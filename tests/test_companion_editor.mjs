import assert from 'node:assert/strict';
import { JSDOM } from '../web/node_modules/jsdom/lib/api.js';

const dom = new JSDOM('<!doctype html><textarea id="code" aria-label="Session source"></textarea>',
  {pretendToBeVisual: true, url: 'http://127.0.0.1:18990/'});
const {window} = dom;
for (const name of ['window', 'document', 'MutationObserver', 'HTMLElement', 'Node', 'Event']) globalThis[name] = window[name] ?? window;
Object.defineProperty(globalThis, 'navigator', {value: window.navigator, configurable: true});
globalThis.requestAnimationFrame = window.requestAnimationFrame.bind(window);
globalThis.cancelAnimationFrame = window.cancelAnimationFrame.bind(window);
globalThis.getComputedStyle = window.getComputedStyle.bind(window);
// jsdom has no layout engine. Geometry is deliberately stubbed; this test
// verifies real paste, parser/highlight DOM, text and selection/history only.
window.Range.prototype.getClientRects = () => [];
window.Range.prototype.getBoundingClientRect = () => ({left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0});
const {attach} = await import('../web/editor.mjs');
const {EditorView} = await import('../web/node_modules/codemirror/dist/index.js');
const {undo} = await import('../web/node_modules/@codemirror/commands/dist/index.js');
const textarea = window.document.getElementById('code');
let changes = 0;
textarea.addEventListener('input', () => changes++);
const adapter = attach(textarea, 'test-style-nonce');
const content = window.document.querySelector('.cm-content');
const view = EditorView.findFromDOM(content);
assert.ok(view && textarea.hidden);
assert.equal(content.getAttribute('aria-label'), 'Session source');
const code = '// pasted Strudel code\nconst melody = note("c4 <e4 g4>")\n'
  + '/* block\ncomment */\n$: melody.gain(.5).s(`piano\nname`)\n'
  + 'const escaped = "a\\\"b <script>";\n\n';
const event = new window.Event('paste', {bubbles: true, cancelable: true});
Object.defineProperty(event, 'clipboardData', {value: {files: [], getData: type => type === 'text/plain' ? code : '<b>wrong HTML</b>'}});
view.focus(); content.dispatchEvent(event);
assert.equal(view.state.doc.toString(), code);
assert.equal(textarea.value, code);
assert.equal(changes, 1);
assert.equal(content.querySelectorAll('script,b').length, 0);
assert.equal(view.state.doc.lines, code.split('\n').length);
assert.ok([...content.querySelectorAll('span[class]')].some(span => span.textContent === 'const'));
assert.ok([...content.querySelectorAll('span[class]')].some(span => span.textContent.includes('// pasted')));
assert.ok([...content.querySelectorAll('span[class]')].some(span => span.textContent.includes('c4 <e4 g4>')));
assert.ok([...window.document.querySelectorAll('style')].length > 0);
assert.ok([...window.document.querySelectorAll('style')].every(style => style.nonce === 'test-style-nonce'));
view.dispatch({selection: {anchor: 12, head: 20}});
const state = view.state;
adapter.setValue(code); // Unchanged server polling must be a true no-op.
assert.equal(view.state, state);
assert.equal(view.state.selection.main.anchor, 12);
assert.equal(view.state.selection.main.head, 20);
assert.ok(undo(view));
assert.equal(textarea.value, '');
adapter.setValue('note("c4")\n');
assert.equal(textarea.value, 'note("c4")\n');
assert.ok([...content.querySelectorAll('span[class]')].some(span => span.textContent === '"c4"'));
view.dispatch({selection: {anchor: 0, head: view.state.doc.length}});
adapter.insertText('s("bd")');
assert.equal(textarea.value, 's("bd")');
assert.ok(undo(view));
assert.equal(textarea.value, 'note("c4")\n');
content.dispatchEvent(new window.KeyboardEvent('keydown', {key: 'Tab', code: 'Tab', bubbles: true, cancelable: true}));
assert.equal(textarea.value, '  note("c4")\n');
content.dispatchEvent(new window.KeyboardEvent('keydown', {key: 'Tab', code: 'Tab', shiftKey: true, bubbles: true, cancelable: true}));
assert.equal(textarea.value, 'note("c4")\n');
view.destroy(); window.close();
console.log('companion real editor paste, multiline highlighting, escaped text, CSP nonce, selection and undo ok');
