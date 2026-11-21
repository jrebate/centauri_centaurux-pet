// Desktop shell: turns the prototype into a floating desktop pet (Neutralino window).
(() => {
  const M = window.__mascota;
  const canvas = document.getElementById('screen');
  document.getElementById('stage').appendChild(canvas);
  const wheel = document.getElementById('wheel');
  const menu = document.getElementById('menu');
  const autoBox = document.getElementById('auto');
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const inNeu = typeof Neutralino !== 'undefined' && !!window.NL_PORT;

  if (inNeu) {
    Neutralino.init();
    Neutralino.events.on('windowClose', () => Neutralino.app.exit());
    Neutralino.window.setDraggableRegion(canvas, { dragMinDistance: 6 }).catch(() => {});
  }

  // ---- Mouse: scroll = change mission, click = open/press or a button, drag = move ----
  canvas.addEventListener('wheel', e => {
    e.preventDefault();
    wheel.dispatchEvent(new WheelEvent('wheel', { deltaY: e.deltaY * 3, cancelable: true }));
  }, { passive: false });

  let talking = false, down = null, dragged = false;  // talking: Space only (the prototype handles it)
  canvas.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    hideMenu();
    M.walk.on = false;
    down = { x: e.screenX, y: e.screenY }; dragged = false;
  });
  function clickable(x, y) {
    return (M.petMode && M.hitAccept(x, y)) || M.hitMarker(x, y) >= 0 || M.hitBack(x, y);
  }
  window.addEventListener('pointermove', e => {
    if (down && Math.hypot(e.screenX - down.x, e.screenY - down.y) > 5) dragged = true;
    if (!down) canvas.style.cursor = clickable(e.clientX, e.clientY) ? 'pointer' : 'grab';
  });
  window.addEventListener('pointerup', e => {
    if (e.button !== 0 || !down) return;
    const x = e.clientX, y = e.clientY;
    if (dragged) walkOffset = 0;
    else if (M.petMode && M.hitAccept(x, y)) M.accept(M.hitAccept(x, y));
    else if (M.hitMarker(x, y) >= 0) M.select(M.hitMarker(x, y));
    else if (M.hitBack(x, y)) setBig(false);
    else if (M.petMode) setBig(true);
    else M.press();
    down = null;
  });
  // Pet mode: Enter collects a finished mission's reward, or else opens the big screen
  window.addEventListener('keydown', e => {
    if (!M.petMode || e.key !== 'Enter') return;
    e.preventDefault(); e.stopPropagation();
    if (M.state === 'happy') M.press(); else setBig(true);
  }, true);

  async function setBig(on) {
    M.setPetMode(!on);
    if (inNeu) await Neutralino.window.setSize({ width: on ? 800 : 320, height: on ? 480 : 260 }).catch(() => {});
    syncMenu();
  }

  // ---- Right-click menu ----
  let walkEnabled = true;
  function syncMenu() {
    const n = M.rewards;
    menu.querySelector('[data-act="rewards"]').hidden = !n;
    menu.querySelector('[data-act="rewards"]').textContent = n === 1 ? 'Recoger la recompensa' : `Recoger las ${n} recompensas`;
    menu.querySelector('[data-act="big"]').hidden = !M.petMode;
    menu.querySelector('[data-act="pet"]').hidden = M.petMode;
    menu.querySelector('[data-act="walk"]').textContent = `Pasear: ${walkEnabled ? 'sí' : 'no'}`;
    menu.querySelector('[data-act="demo"]').textContent = window.__realData ? 'Datos reales de Claude' : `Demo automática: ${autoBox.checked ? 'sí' : 'no'}`;
    menu.querySelector('[data-act="screen"]').textContent = screenOk ? 'Pantalla redonda: conectada' : 'Pantalla redonda: buscando…';
    menu.querySelector('[data-act="demo"]').disabled = !!window.__realData;
    const centaur = document.querySelector('#chars [data-char="centaur"]').getAttribute('aria-pressed') === 'true';
    menu.querySelector('[data-act="char"]').textContent = centaur ? 'Cambiar a guerrero sin caballo' : 'Cambiar a centauro';
  }
  function hideMenu() { menu.hidden = true; }
  canvas.addEventListener('contextmenu', e => {
    e.preventDefault();
    syncMenu();
    menu.hidden = false;
    const w = menu.offsetWidth, h = menu.offsetHeight;
    menu.style.left = Math.max(4, Math.min(e.clientX, window.innerWidth - w - 4)) + 'px';
    menu.style.top = Math.max(4, Math.min(e.clientY, window.innerHeight - h - 4)) + 'px';
  });
  menu.addEventListener('click', e => {
    const b = e.target.closest('button'); if (!b) return;
    const act = b.dataset.act;
    hideMenu();
    if (act === 'rewards') M.acceptAll();
    else if (act === 'big') setBig(true);
    else if (act === 'pet') setBig(false);
    else if (act === 'walk') { walkEnabled = !walkEnabled; M.walk.on = false; }
    else if (act === 'screen') askScreenAddress();
    else if (act === 'demo') { autoBox.checked = !autoBox.checked; }
    else if (act === 'char') {
      const centaur = document.querySelector('#chars [data-char="centaur"]').getAttribute('aria-pressed') === 'true';
      document.querySelector(`#chars [data-char="${centaur ? 'nord' : 'centaur'}"]`).click();
    }
    else if (act === 'hide') setVisible(false);
    else if (act === 'exit') { if (inNeu) Neutralino.app.exit(); else window.close(); }
  });
  document.addEventListener('keydown', e => { if (e.key === 'Escape') { hideMenu(); if (!M.petMode) setBig(false); } });
  window.addEventListener('blur', hideMenu);

  // ---- Wandering: walks a little along the desktop now and then ----
  let walkOffset = 0;
  async function wander() {
    if (!inNeu) return;
    for (;;) {
      await sleep(7000 + Math.random() * 10000);
      const st = M.state;
      if (!walkEnabled || !M.petMode || talking || down || !menu.hidden) continue;
      if (!['thinking', 'happy'].includes(st)) continue;
      const dir = walkOffset > 160 ? -1 : walkOffset < -160 ? 1 : (Math.random() < 0.5 ? -1 : 1);
      let pos;
      try { pos = await Neutralino.window.getPosition(); } catch (e) { continue; }
      M.walk.facing = dir; M.walk.on = true;
      const steps = 30 + Math.floor(Math.random() * 40);
      for (let i = 0; i < steps && M.walk.on; i++) {
        pos.x += dir * 3; walkOffset += dir * 3;
        try { await Neutralino.window.move(pos.x, pos.y); } catch (e) { break; }
        await sleep(40);
      }
      M.walk.on = false;
    }
  }
  wander();

  // ---- Real data: reads estado.json next to the app (or in %USERPROFILE%\.mascota-claude) ----
  let paths = null, activePath = null;
  async function poll() {
    if (!inNeu) return;
    if (!paths) {
      paths = [window.NL_PATH + '/estado.json'];
      try { paths.push((await Neutralino.os.getEnv('USERPROFILE')) + '\\.mascota-claude\\estado.json'); } catch (e) {}
    }
    for (const path of paths) {
      try {
        const data = JSON.parse((await Neutralino.filesystem.readFile(path)).replace(/^\uFEFF/, ''));
        activePath = path;
        if (Array.isArray(data.projects) && data.projects.length) { window.__realData = true; M.loadReal(data.projects); }
        return;
      } catch (e) { /* missing or half-written file: try the next one, or keep the demo */ }
    }
  }
  poll();

  // ---- Collected rewards: the pet already shows them asleep; save that in estado.json too ----
  window.__onAccept = async names => {
    if (!inNeu || !activePath) return;
    try {
      const data = JSON.parse((await Neutralino.filesystem.readFile(activePath)).replace(/^\uFEFF/, ''));
      let changed = false;
      (data.projects || []).forEach(p => {
        if (names.includes(p.name) && p.state === 'happy') {
          Object.assign(p, { state: 'sleeping', status: 'Recompensa recogida', updatedAt: new Date().toISOString() });
          changed = true;
        }
      });
      if (changed) await Neutralino.filesystem.writeFile(activePath, JSON.stringify(data, null, 1));
    } catch (e) { /* the file catches up on the next update */ }
  };

  // ---- Round screen (Centauri board) over Wi-Fi: sends it the missions every few seconds ----
  // The board answers at http://centauri.local; if the network doesn't resolve that name,
  // right-click > "Pantalla redonda" lets you type the address shown on the board.
  let screenHost = 'centauri.local', screenOk = false, lastSent = '', lastSentAt = 0;
  if (inNeu) Neutralino.storage.getData('pantalla').then(v => { if (v && v.trim()) screenHost = v.trim(); }).catch(() => {});
  async function sendToScreen() {
    if (!inNeu) return;
    const body = JSON.stringify({ projects: M.snapshot });
    if (screenOk && body === lastSent && Date.now() - lastSentAt < 30000) return;
    const ctl = new AbortController();
    const t = setTimeout(() => ctl.abort(), 4000);
    try {
      // text/plain keeps it a "simple" request, so the browser doesn't need an extra permission round-trip
      const r = await fetch(`http://${screenHost}/estado`, { method: 'POST', body, headers: { 'Content-Type': 'text/plain' }, signal: ctl.signal });
      screenOk = r.ok;
      if (r.ok) {
        lastSent = body; lastSentAt = Date.now();
        // rewards collected by tapping the round screen: collect them here too (saves estado.json)
        const res = await r.json().catch(() => ({}));
        if (Array.isArray(res.accepted) && res.accepted.length) { res.accepted.forEach(n => M.accept(n)); lastSent = ''; }
      }
    } catch (e) { screenOk = false; }
    finally { clearTimeout(t); }
  }
  // Timers in a hidden window are slowed down by the browser engine (down to once a minute),
  // so the beat comes from a small worker, which isn't throttled.
  let beat = 0;
  try {
    const w = new Worker(URL.createObjectURL(new Blob(['setInterval(() => postMessage(0), 1000)'], { type: 'text/javascript' })));
    w.onmessage = () => { beat++; if (beat % 5 === 0) poll(); if (beat % 3 === 0) sendToScreen(); };
  } catch (e) { setInterval(poll, 5000); setInterval(sendToScreen, 3000); }
  function askScreenAddress() {
    const v = window.prompt('Dirección de la pantalla redonda (la que sale en la pantalla, por ejemplo 192.168.1.40).\nDéjalo vacío para buscarla sola como centauri.local.', screenHost === 'centauri.local' ? '' : screenHost);
    if (v === null) return;
    screenHost = v.trim().replace(/^https?:\/\//, '').replace(/\/.*$/, '') || 'centauri.local';
    screenOk = false; lastSent = '';
    if (inNeu) Neutralino.storage.setData('pantalla', screenHost === 'centauri.local' ? '' : screenHost).catch(() => {});
    sendToScreen();
  }

  // ---- Hidden by default: the pet keeps feeding the round screen from the background.
  // An icon next to the clock shows or hides it; the choice is remembered.
  async function setVisible(v) {
    if (!inNeu) return;
    try { if (v) { await Neutralino.window.show(); await Neutralino.window.focus(); } else await Neutralino.window.hide(); } catch (e) {}
    Neutralino.storage.setData('visible', v ? '1' : '0').catch(() => {});
  }
  if (inNeu) {
    Neutralino.os.setTray({
      icon: '/resources/icon.png',
      menuItems: [
        { id: 'show', text: 'Mostrar la mascota' },
        { id: 'hide', text: 'Esconder la mascota' },
        { text: '-' },
        { id: 'quit', text: 'Cerrar la mascota' },
      ],
    }).catch(() => {});
    Neutralino.events.on('trayMenuItemClicked', e => {
      const id = e.detail && e.detail.id;
      if (id === 'show') setVisible(true);
      else if (id === 'hide') setVisible(false);
      else if (id === 'quit') Neutralino.app.exit();
    });
    // the window starts hidden (see neutralino.config.json); show it only if it was left visible
    Neutralino.storage.getData('visible').then(v => { if (v === '1') setVisible(true); }).catch(() => {});
  }
})();
