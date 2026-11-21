/* The stand-in for the launcher when this page is opened in a plain browser (a design preview): it answers the page's
   messages with made-up data and sped-up timings. Inside the launcher this file does nothing. Its pictures and sounds
   (img/save*.jpg, img/game/*, audio/*) are only there in a lab copy, never in the repo or the exe. */
'use strict';
(function () {
  if (window.chrome && window.chrome.webview || window.external && typeof window.external.sendMessage === 'function') return;
  let out = null;
  const emit = m => setTimeout(() => out && out(m), 0);
  const FILES = [
    { slot: 4, label: 'File 4', place: 'Castle Town Market', when: 'yesterday', meta: '12 h 40 min played · Saved yesterday, 21:40', time: '12 h 40 min played', pic: 'img/save3.jpg', last: true },
    { slot: 2, label: 'File 2', place: 'Castle grounds', when: '16 Nov', meta: '9 h 05 min played · Saved 16 Nov', time: '9 h 05 min played', pic: 'img/save1.jpg' },
    { slot: 1, label: 'File 1', place: 'Lake Hylia', when: '15 Nov', meta: '7 h 31 min played · Saved 15 Nov', time: '7 h 31 min played', pic: 'img/save0.jpg' },
    { slot: 3, label: 'File 3', place: 'Kokiri Forest', when: '12 Nov', meta: '0 h 48 min played · Saved 12 Nov', time: '0 h 48 min played', pic: 'img/save2.jpg' },
    { slot: 5, label: 'File 5', place: 'Hyrule Field', when: '11 Nov', meta: '3 h 12 min played · Saved 11 Nov', time: '3 h 12 min played', pic: 'img/save4.jpg' },
    { slot: 6, label: 'File 6', place: 'Hyrule Castle', when: '9 Nov', meta: '5 h 50 min played · Saved 9 Nov', time: '5 h 50 min played', pic: 'img/save5.jpg' },
  ];
  const NEWS = { title: 'New in 0.5.2', items: [{ b: 'Battle music', p: 'ends again after the last enemy.' }, { b: 'Menu text', p: 'always shows on every screen.' }, { b: 'Updates', p: 'download 12 MB and keep your shaders.' }], more: true };
  const st = { lang: 'English (US)', folder: 'D:\\Games\\Triforce', ver: '0.5.2', update: false, timer: 0, kind: '', sound: { music: .4, sfx: .7, muted: false } };
  const SET = () => [
    { h: 'Launcher' }, { id: 'music', label: 'Music', kind: 'slider' }, { id: 'sfx', label: 'Sounds', kind: 'slider' },
    { id: 'autostart', label: 'Start with Windows', value: 'No', actLabel: 'Change' },
    { h: 'Game' }, { id: 'folder', label: 'Game folder', value: st.folder, actLabel: 'Move' }, { id: 'glang', label: 'Game language', value: st.lang, actLabel: 'Change' },
    { id: 'dlss', label: 'NVIDIA DLSS', value: 'Not included', actLabel: 'Include' }, { id: 'rebuild', label: 'Build the game again', value: 'About 45 minutes', actLabel: 'Build' },
    { h: 'Shortcuts' }, { id: 'desktop', label: 'Desktop shortcut', value: 'Yes', actLabel: 'Change' }, { id: 'steam', label: 'Steam', value: 'Not added', actLabel: 'Add to Steam' },
    { h: 'Help' }, { id: 'logs', label: 'Log folder', value: '', actLabel: 'Open' }, { id: 'versions', label: 'Versions', value: 'launcher 1.0.0 · game ' + st.ver, actLabel: 'Copy report' },
    { h: 'Remove' }, { id: 'uninstall', label: 'Uninstall Triforce', value: 'Your saves stay', actLabel: 'Uninstall', danger: true },
  ];
  function home() {
    const s = { t: 'state', files: FILES, settings: SET(), bench: { available: true, last: 'Not run yet on this PC' }, sound: st.sound };
    if (st.update) {
      s.home = { mode: 'update', eb: 'An update is ready', title: 'Version 0.5.3', meta: '12 MB download · About 4 minutes · your saves and mods stay', button: 'Update', act: 'update',
        links: [{ html: '<b>Play 0.5.2 first</b>', act: 'playold' }, { html: 'See all changes', act: 'open:changes' }], news: { title: 'New in 0.5.3', items: [{ b: 'RX 580', p: 'no more white speckles on grass and Link.' }, { b: 'Controller', p: 'LB and RB reach every menu tab.' }, { b: 'Mods', p: 'the whole list, and online co-op.' }] } };
      s.pill = { text: 'Update ready', dot: 'amber', act: 'update' };
    } else {
      s.home = { mode: 'ready', button: 'Play', act: 'play', links: [{ html: `Version ${st.ver} · <b>up to date</b>` }, { html: esc(st.folder), act: 'open:game' }], news: NEWS };
      s.pill = { text: 'Up to date · ' + st.ver, dot: '' };
    }
    return s;
  }
  function esc(s) { return String(s).replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c])); }
  function reveal(name) {
    emit({ t: 'state', media: { art: true, music: true, sfx: true }, reveal: {
      found: `<b>&#9670; Your game is ready to build</b> <em>· ${esc(name)} · version 1.0.0</em>`,
      rows: [{ label: 'Language', value: st.lang, act: 'lang' }, { label: 'Where it goes', value: st.folder, small: '60 GB while building · 212 GB free', act: 'folder' }],
      under: 'About 45 minutes on this PC, the tools included · you can keep using it meanwhile', canBuild: true } });
    emit({ t: 'go', s: 'reveal' });
  }
  function check(name) {
    emit({ t: 'check', st: 'checking', name });
    setTimeout(() => {
      if (!/\.nup$/i.test(name) && !/folder/.test(name)) return emit({ t: 'check', st: 'bad', msg: `${name} is not a .nup file. Drop your .nup, or the folder with romfs and exefs in it.` });
      emit({ t: 'check', st: 'idle' }); reveal(name);
    }, 1200);
  }
  function work(kind) {
    clearInterval(st.timer); st.kind = kind;
    const steps = kind === 'build' ? [['Downloading Triforce\u2019s tools', 2, '262 MB'], ['Reading your game files', 2, '1 min'], ['Translating the game to PC code', 4.5, '11 min'], ['Compiling', 6, '20 min'], ['Preparing shaders for your graphics card', 5, 'about 13 min on an RTX 4070']]
      : [['Downloading 0.5.3', 2.5, '12 MB'], ['Rebuilding the changed code', 4, '3 min'], ['Keeping your shaders', 1.2, 'kept']];
    const mins = kind === 'build' ? 45 : 4, total = steps.reduce((a, s) => a + s[1], 0); let t = 0;
    const path = kind === 'build' ? [{ name: 'Your game', st: 'done' }, { name: 'Build', st: 'now' }, { name: 'Play' }] : [];
    const tick = () => {
      let acc = 0; const p = Math.min(1, t / total), left = Math.ceil(mins * (1 - p));
      emit({ t: 'work', kind, path, eb: kind === 'build' ? 'Building your game' : 'Updating to 0.5.3', title: left <= 1 ? 'Almost done' : `About ${left} minutes left`, frac: p, canStop: true, error: null,
        stopTitle: kind === 'build' ? 'Stop the build?' : 'Stop the update?', stopText: 'What is done so far is kept, so it picks up from here next time.',
        note: kind === 'build' ? 'Keep this window open; you can minimize it. You get a notice when the game is ready.' : 'Your saves, mods and shaders stay.',
        steps: steps.map(([n, s, real]) => { const a = acc; acc += s; return { name: n, st: t >= acc ? 'd' : t >= a ? 'n' : '', right: t >= acc ? real : t >= a ? 'now' : real }; }) });
      if (Math.random() < .3) emit({ t: 'log', lines: ['@@progress ' + p.toFixed(3)] });
    };
    tick(); emit({ t: 'go', s: 'work' });
    st.timer = setInterval(() => {
      t += .1; tick();
      if (t >= total) {
        clearInterval(st.timer);
        setTimeout(() => {
          if (kind === 'build') { emit(Object.assign(home(), { done: { eb: 'Built in 44 minutes', lead: '24 GB in ' + st.folder + ' · shaders ready for your graphics card · your controller works out of the box' } })); emit({ t: 'go', s: 'done' }); }
          else { st.update = false; st.ver = '0.5.3'; NEWS.title = 'New in 0.5.3'; emit(home()); emit({ t: 'go', s: 'home' }); emit({ t: 'toast', html: '<b>&#9670;</b> Updated to 0.5.3 · your saves, mods and shaders stayed', sfx: 'success' }); }
        }, 600);
      }
    }, 100);
  }
  const MODS = [
    { id: 'sq', name: 'Squadala!', sub: 'Online co-op · daurosk · work in progress', state: 'kit', desc: 'Play together online. Installs its own kit beside your game and starts it.' },
    { id: 'a', name: 'Sample: HD Kokiri textures', sub: 'Textures · sample data', state: 'get' }, { id: 'b', name: 'Sample: Classic HUD', sub: 'Interface · sample data', state: 'get' },
    { id: 'c', name: 'Sample: Brighter nights', sub: 'Lighting · sample data', state: 'get' }, { id: 'd', name: 'Sample: Faster text', sub: 'Gameplay · sample data', state: 'get' },
    { id: 'e', name: 'Sample: Ocarina notes on screen', sub: 'Interface · sample data', state: 'get' },
  ];
  const MINE = [{ id: 'm1', name: 'Sample: Quieter Navi', sub: 'In your mods folder', state: 'on', removable: true }, { id: 'm2', name: 'Sample: Tunic recolor', sub: 'In your mods folder', state: 'off', removable: true }];
  function mods(tab, q) {
    const items = (tab === 'mine' ? MINE : MODS).filter(m => !q || m.name.toLowerCase().includes(q.toLowerCase()));
    emit({ t: 'state', mods: { tab, items, loaded: true, note: tab === 'mine' ? 'Changes take effect after the game is built again: <b>about 6 minutes</b>.' : '' } });
  }
  function bench() {
    clearInterval(st.timer); let t = 0; const per = 1.6, total = per * 8;
    const P = [{ img: 'img/save0.jpg', name: 'Lake Hylia', a: 142, b: 118 }, { img: 'img/save4.jpg', name: 'Hyrule Field', a: 96, b: 71 }, { img: 'img/save3.jpg', name: 'Castle Town Market', a: 88, b: 64 }, { img: 'img/save2.jpg', name: 'Kokiri Forest', a: 121, b: 97 }];
    st.timer = setInterval(() => {
      t += .1; const k = Math.min(7, Math.floor(t / per)), p = P[k % 4], pass = k < 4 ? 1 : 2;
      emit({ t: 'bench', phase: 'run', place: p, pass: `Finding your best settings · pass ${pass} of 2 · ${pass === 1 ? 'your settings' : 'with DLSS'}`, fps: Math.round((pass === 1 ? p.a * .55 : p.a) + Math.sin(t * 7) * 6), frac: t / total });
      if (t >= total) {
        clearInterval(st.timer);
        emit({ t: 'bench', phase: 'done', eb: 'Benchmark finished · 3 min 12 s · RTX 4070 SUPER', places: P.map(p => ({ img: p.img, name: p.name, fps: p.a, low: p.b })),
          verdict: 'Your PC holds <b>above 60 fps everywhere</b> at your current settings. With DLSS on, it holds above 120 almost everywhere, so a 120 cap plays smoother without dips.',
          recs: [{ name: 'Game frame rate cap', from: '60', to: '120' }, { name: 'Upscaler', from: 'Off', to: 'NVIDIA DLSS' }, { name: 'Render resolution', from: 'Native', to: 'Quality' }, { name: 'Frame generation', from: 'Off', to: 'Off', same: true }, { name: 'Anti-aliasing', from: 'SMAA', to: 'DLSS' }] });
      }
    }, 100);
  }
  window.DEMO = {
    jumps: [['welcome', 'First run: welcome'], ['reveal', 'First run: game found'], ['build', 'First run: building'], ['fail', 'Build failed'], ['done', 'First run: ready'], ['home', 'Home'], ['update', 'Home with an update ready'], ['saves', 'Saves'], ['mods', 'Mods'], ['settings', 'Settings'], ['bench', 'Benchmark']],
    jump(j, fn) {
      out = fn; clearInterval(st.timer);
      if (j === 'welcome') { emit({ t: 'state', media: { art: false, music: false, sfx: true } }); emit({ t: 'go', s: 'welcome' }); }
      else if (j === 'reveal') reveal('OcarinaOfTime.nup');
      else if (j === 'build') { emit({ t: 'state', media: { art: true, music: true, sfx: true } }); work('build'); }
      else if (j === 'fail') { emit({ t: 'state', media: { art: true, music: true, sfx: true } }); emit({ t: 'go', s: 'work' }); emit({ t: 'work', kind: 'build', eb: 'Building your game', title: 'The build stopped', lead: 'The drive ran out of space while compiling. Free up about 20 GB on D: and try again: it carries on from where it stopped.', error: { msg: 'disk full' }, path: [] }); }
      else if (j === 'done') { emit(Object.assign(home(), { media: { art: true, music: true, sfx: true }, done: { eb: 'Built in 44 minutes', lead: '24 GB in D:\\Games\\Triforce · shaders ready for your graphics card' } })); emit({ t: 'go', s: 'done' }); }
      else if (j === 'update') { st.update = true; emit(Object.assign(home(), { media: { art: true, music: true, sfx: true } })); emit({ t: 'go', s: 'home' }); }
      else if (j === 'bench') { emit(Object.assign(home(), { media: { art: true, music: true, sfx: true } })); bench(); }
      else { st.update = false; emit(Object.assign(home(), { media: { art: true, music: true, sfx: true } })); emit({ t: 'go', s: j }); }
    },
    receive(m, fn) {
      out = fn;
      if (m.t === 'ready') { emit({ t: 'hello', chrome: true, os: 'windows' }); emit({ t: 'state', sound: st.sound, media: { art: false, music: false, sfx: true } }); const j = location.hash.slice(1); if (j) window.DEMO.jump(j, fn); else emit({ t: 'go', s: 'welcome' }); return; }
      if (m.t === 'pick') return check(m.kind === 'folder' ? 'OcarinaOfTime (folder)' : 'OcarinaOfTime.nup');
      if (m.t === 'drop') return check(m.names[0] || 'file');
      if (m.t === 'win') return emit({ t: 'toast', html: 'Window button: ' + m.a + ' <span style="color:var(--text3)">· in the launcher only</span>' });
      if (m.t !== 'cmd') return;
      const c = m.c;
      if (c === 'build') work('build');
      else if (c === 'update') work('update');
      else if (c === 'stop' || c === 'workback') { clearInterval(st.timer); emit({ t: 'go', s: st.kind === 'update' ? 'home' : 'reveal' }); }
      else if (c === 'retry') work('build');
      else if (c === 'play' || c === 'playold') { emit({ t: 'go', s: 'playing' }); setTimeout(() => emit({ t: 'go', s: 'home' }), 6000); }
      else if (c === 'choose' && m.id === 'lang') emit({ t: 'choice', title: 'Game language', reply: 'lang', options: ['English (US)', 'English (UK)', 'Français (France)', 'Français (Canada)', 'Deutsch', 'Español (España)', 'Español (Latinoamérica)', 'Italiano', 'Nederlands', 'Português', 'Русский', '日本語', '한국어', '简体中文', '繁體中文'].map(n => ({ id: n, name: n, sel: n === st.lang })) });
      else if (c === 'lang') { st.lang = m.id; reveal('OcarinaOfTime.nup'); }
      else if (c === 'choose' && m.id === 'folder') emit({ t: 'toast', html: 'The system folder picker opens here' });
      else if (c === 'mods.list') mods(m.tab, m.q);
      else if (c === 'mods.main') { const x = MODS.concat(MINE).find(x => x.id === m.id); if (x) { x.state = x.state === 'on' ? 'off' : x.state === 'off' ? 'on' : x.state === 'kit' ? 'start' : 'on'; mods(m.tab, ''); emit({ t: 'sfx', id: 'success' }); } }
      else if (c === 'saves.list') emit({ t: 'state', saves: { eb: 'Six files in your game folder', found: 'From an emulator on this PC, or a folder. Found: Eden (6 files), Ryujinx (2 files).', items: FILES } });
      else if (c === 'saves.import') emit({ t: 'choice', title: 'Bring saves in', text: 'Pick where your saves are. Whatever they replace is backed up first.', ok: 'Bring them in', reply: 'saves.importfrom', options: [{ id: 'eden', name: 'Eden', sub: '6 files · last played 2 days ago', sel: true }, { id: 'ryu', name: 'Ryujinx', sub: '2 files · last played in March' }, { id: 'dir', name: 'A folder…', sub: 'choose it yourself' }] });
      else if (c === 'saves.importfrom') emit({ t: 'toast', html: '<b>&#9670;</b> 6 files brought in from Eden · the old ones are in backups', sfx: 'success' });
      else if (c === 'saves.export') emit({ t: 'choice', title: 'Copy saves out', text: 'Your files, copied. Nothing here changes.', ok: 'Copy them out', reply: 'saves.exportto', options: [{ id: 'eden', name: 'To Eden', sub: 'its files are backed up first', sel: true }, { id: 'dir', name: 'To a folder…' }] });
      else if (c === 'saves.exportto') emit({ t: 'toast', html: '<b>&#9670;</b> 6 files copied out', sfx: 'success' });
      else if (c === 'setting' && m.id === 'uninstall') emit({ t: 'ask', title: 'Uninstall Triforce?', html: '<p>Removes the launcher, its tools and the built game.</p><p>Your saves and mods stay in a backup folder.</p>', reply: 'uninstall', buttons: [{ id: 'no', label: 'Keep it' }, { id: 'yes', label: 'Uninstall', main: true }] });
      else if (c === 'setting') emit({ t: 'toast', html: m.id + ' <span style="color:var(--text3)">· the launcher does this</span>' });
      else if (c === 'bench') bench();
      else if (c === 'benchstop') { clearInterval(st.timer); emit({ t: 'bench', phase: 'stopped' }); }
      else if (c === 'benchapply') { emit({ t: 'go', s: 'settings' }); emit({ t: 'toast', html: '<b>&#9670;</b> Applied · they take effect the next time you play', sfx: 'success' }); }
      else if (c === 'report' || c === 'details') { }
      else if (c === 'open') emit({ t: 'toast', html: 'Opens ' + esc(m.arg || '') });
    },
  };
})();
