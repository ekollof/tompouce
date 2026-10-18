(() => {
  const element = id => document.getElementById(id);
  const categories = ['samples', 'drum-machines', 'synths', 'wavetables', 'user', 'import-sounds'];
  let category = categories[0], offset = 0, sequence = 0, timer;
  const buttons = categories.map(name => {
    const button = document.createElement('button');
    button.textContent = name.replaceAll('-', ' ');
    button.onclick = () => { category = name; offset = 0; refresh(); };
    element('sound-tabs').append(button); return button;
  });
  async function refresh() {
    const request = ++sequence;
    buttons.forEach((button, index) => button.setAttribute('aria-pressed', String(categories[index] === category)));
    const importing = category === 'import-sounds';
    element('sound-import').hidden = importing === false;
    element('sound-query').disabled = importing;
    element('sound-results').replaceChildren();
    element('sound-prev').disabled = true; element('sound-next').disabled = true;
    if (importing) { element('sound-status').textContent = 'Choose a folder to import.'; return; }
    try {
      const result = JSON.parse(await TompouceCompanion.tool('music_sounds', {category, query: element('sound-query').value, offset, limit: 100}));
      if (request !== sequence) return;
      element('sound-status').textContent = `${result.total} sounds`;
      for (const item of result.items) {
        const row = document.createElement('div'); row.className = 'sound-row';
        const label = document.createElement('span'); label.textContent = `${item.name} · ${item.slots} sample${item.slots === 1 ? '' : 's'}`;
        const insert = document.createElement('button'); insert.textContent = 'Insert';
        insert.onclick = () => TompouceCompanion.insertSnippet(item.code);
        row.append(label, insert); element('sound-results').append(row);
      }
      element('sound-prev').disabled = offset === 0;
      element('sound-next').disabled = offset + result.items.length >= result.total;
    } catch (error) { if (request === sequence) element('sound-status').textContent = error.message; }
  }
  element('sound-query').oninput = () => { clearTimeout(timer); offset = 0; timer = setTimeout(refresh, 150); };
  element('sound-prev').onclick = () => { offset = Math.max(0, offset - 100); refresh(); };
  element('sound-next').onclick = () => { offset += 100; refresh(); };
  element('sound-files').onchange = async event => {
    const input = event.target;
    const files = [...input.files].filter(file => /\.(wav|mp3|flac)$/i.test(file.name));
    files.sort((a, b) => a.webkitRelativePath.localeCompare(b.webkitRelativePath));
    input.disabled = true;
    let imported = 0;
    try {
      if (!files.length || files.length > 2048) throw new Error('Choose a folder containing 1–2048 supported audio files.');
      for (const file of files) {
        if (file.size > 8 * 1024 * 1024) throw new Error(`${file.name} exceeds 8 MiB.`);
        const parts = (file.webkitRelativePath || file.name).split('/');
        let name = (parts.at(-2) || 'imported').toLowerCase().replace(/[^a-z0-9_-]+/g, '_').replace(/^[_-]+|[_-]+$/g, '').slice(0, 55);
        if (!/^[a-z]/.test(name)) name = `sound_${name}`;
        element('sound-status').textContent = `Importing ${imported + 1}/${files.length}: ${file.name}`;
        const response = await fetch('/api/import', {method: 'POST', headers: {
          'Content-Type': 'application/octet-stream',
          'X-Companion-Token': document.querySelector('meta[name="companion-token"]').content,
          'X-Sound-Name': encodeURIComponent(name), 'X-Sound-File': encodeURIComponent(file.name),
        }, body: file});
        const result = await response.json();
        if (!response.ok) throw new Error(result.error?.message ?? result.error ?? 'Import failed');
        imported++;
      }
      category = 'user'; offset = 0; element('sound-query').value = ''; await refresh();
      element('sound-status').textContent += ` · Imported ${imported} files`;
    } catch (error) { element('sound-status').textContent = `${imported} files imported. ${error.message}`; }
    finally { input.disabled = false; input.value = ''; }
  };
  refresh();
})();
