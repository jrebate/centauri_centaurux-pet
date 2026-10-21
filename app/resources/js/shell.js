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

  // ---- Mouse: scroll = wheel, click = open/press, hold = talk, drag = move ----
  canvas.addEventListener('wheel', e => {
    e.preventDefault();
    wheel.dispatchEvent(new WheelEvent('wheel', { deltaY: e.deltaY * 3, cancelable: true }));
  }, { passive: false });

  let holdTimer = null, talking = false, down = null, dragged = false;
  canvas.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    hideMenu();
    M.walk.on = false;
    down = { x: e.screenX, y: e.screenY }; dragged = false;
    holdTimer = setTimeout(() => { if (!dragged) { talking = true; M.startTalk(); } }, 450);
  });
  window.addEventListener('pointermove', e => {
    if (down && Math.hypot(e.screenX - down.x, e.screenY - down.y) > 5) { dragged = true; clearTimeout(holdTimer); }
  });
  window.addEventListener('pointerup', e => {
    if (e.button !== 0 || !down) return;
    clearTimeout(holdTimer);
    if (talking) { talking = false; M.endTalk(); }
    else if (dragged) walkOffset = 0;
    else if (M.petMode && M.hitAccept(e.clientX, e.clientY)) acceptReward(M.hitAccept(e.clientX, e.clientY));
    else if (M.petMode) setBig(true);
    else M.press();
    down = null;
  });

  async function setBig(on) {
    M.setPetMode(!on);
    if (inNeu) await Neutralino.window.setSize({ width: on ? 800 : 320, height: on ? 480 : 260 }).catch(() => {});
    syncMenu();
  }

  // ---- Right-click menu ----
  let walkEnabled = true;
  function syncMenu() {
    menu.querySelector('[data-act="big"]').hidden = !M.petMode;
    menu.querySelector('[data-act="pet"]').hidden = M.petMode;
    menu.querySelector('[data-act="walk"]').textContent = `Pasear: ${walkEnabled ? 'sí' : 'no'}`;
    menu.querySelector('[data-act="demo"]').textContent = window.__realData ? 'Datos reales de Claude' : `Demo automática: ${autoBox.checked ? 'sí' : 'no'}`;
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
    if (act === 'big') setBig(true);
    else if (act === 'pet') setBig(false);
    else if (act === 'walk') { walkEnabled = !walkEnabled; M.walk.on = false; }
    else if (act === 'demo') { autoBox.checked = !autoBox.checked; }
    else if (act === 'char') {
      const centaur = document.querySelector('#chars [data-char="centaur"]').getAttribute('aria-pressed') === 'true';
      document.querySelector(`#chars [data-char="${centaur ? 'nord' : 'centaur'}"]`).click();
    }
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
  poll(); setInterval(poll, 5000);

  // ---- "Aceptar recompensa": puts a finished mission to sleep, in the pet and in estado.json ----
  async function acceptReward(name) {
    M.accept(name);
    if (!inNeu || !activePath) return;
    try {
      const data = JSON.parse((await Neutralino.filesystem.readFile(activePath)).replace(/^\uFEFF/, ''));
      const p = (data.projects || []).find(x => x.name === name);
      if (!p || p.state !== 'happy') return;
      Object.assign(p, { state: 'sleeping', status: 'Recompensa recogida', updatedAt: new Date().toISOString() });
      await Neutralino.filesystem.writeFile(activePath, JSON.stringify(data, null, 1));
    } catch (e) { /* the pet already shows it asleep; the file catches up on the next update */ }
  }
})();
