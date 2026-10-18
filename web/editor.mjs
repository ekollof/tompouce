import { EditorView, basicSetup } from 'codemirror';
import { keymap } from '@codemirror/view';
import { indentWithTab } from '@codemirror/commands';
import { javascript } from '@codemirror/lang-javascript';
import { oneDark } from '@codemirror/theme-one-dark';

export function attach(textarea, nonce) {
  const host = document.createElement('div');
  host.id = 'code-editor';
  textarea.insertAdjacentElement('beforebegin', host);
  const view = new EditorView({
    doc: textarea.value,
    parent: host,
    extensions: [
      basicSetup, keymap.of([indentWithTab]), javascript(), oneDark, EditorView.lineWrapping,
      EditorView.cspNonce.of(nonce),
      EditorView.contentAttributes.of({ 'aria-label': textarea.getAttribute('aria-label') ?? 'Session source', spellcheck: 'false' }),
      EditorView.theme({
        '&': { height: '380px', backgroundColor: '#101916' },
        '.cm-scroller': { overflow: 'auto', fontFamily: 'ui-monospace, monospace', fontSize: '13px', lineHeight: '1.65' },
        '.cm-gutters': { backgroundColor: '#18211e', borderRight: '1px solid #30433c' },
      }),
      EditorView.updateListener.of(update => {
        if (!update.docChanged) return;
        textarea.value = update.state.doc.toString();
        textarea.dispatchEvent(new Event('input', { bubbles: true }));
      }),
    ],
  });
  textarea.hidden = true;
  return {
    insertText(text) {
      view.dispatch(view.state.replaceSelection(text));
      view.focus();
    },
    setValue(text) {
      // Unchanged polling responses must not replace the document or history.
      if (view.state.doc.toString() === text) return;
      view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: text } });
    },
  };
}
