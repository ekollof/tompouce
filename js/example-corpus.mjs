/* Maintenance command only: preserve the source embedded in the official
   examples-page links, including its original authorship/license comments. */
import { createHash } from 'node:crypto';
const source = 'https://strudel.cc/examples/';
const response = await fetch(source);
if (!response.ok) throw new Error(`Examples fetch failed: ${response.status}`);
const html = await response.text();
const examples = [...html.matchAll(/href="\/#([^"]+)"[^>]*>([\s\S]*?)<\/a>/g)].map((match) => {
  const title = match[2].match(/<span[^>]*>([^<]+)<\/span>/)?.[1];
  if (!title || !/^[A-Za-z0-9+/=]+$/.test(match[1])) throw new Error('Unexpected examples-page format');
  const code = new TextDecoder('utf-8', { fatal: true }).decode(Buffer.from(match[1], 'base64'));
  return { title, code, sha256: createHash('sha256').update(code).digest('hex') };
});
if (!examples.length) throw new Error('No examples found');
process.stdout.write(JSON.stringify({ source, retrieved: new Date().toISOString().slice(0, 10), examples }, null, 2)+'\n');
