# Turns the prototype (prototipo/mascota.html) into the desktop app page (app/resources/index.html).
# Usage, from the repo root: python3 app/build_index.py prototipo/mascota.html app/resources/index.html
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
src=open(sys.argv[1], encoding='utf-8').read()
lines=src.split('\n')
hidden='\n'.join(lines[122:175])
script='\n'.join(lines[176:])
def rep(a,b):
    global script
    assert script.count(a)==1, a[:70]
    script=script.replace(a,b)
rep("""  const W = 800, H = 480;""", """  let W = 320, H = 260;""")
rep("""  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  canvas.width = W * dpr; canvas.height = H * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.imageSmoothingEnabled = false;""", """  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  let petMode = true;
  function applySize() {
    W = petMode ? 320 : 800; H = petMode ? 260 : 480;
    canvas.width = W * dpr; canvas.height = H * dpr;
    canvas.style.width = W + 'px'; canvas.style.height = H + 'px';
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.imageSmoothingEnabled = false;
  }
  applySize();""")
rep("""    p.step = state === 'working'""", """    if (walk.on) { p.step = Math.floor(t * 8) % 2; return p; }
    p.step = state === 'working'""")
rep("""  let last = now();
  function frame() {""", """  const walk = { on: false, dir: 1, facing: 1 };
  function fit(text, maxW) {
    if (ctx.measureText(text).width <= maxW) return text;
    while (text.length > 1 && ctx.measureText(text + '…').width > maxW) text = text.slice(0, -1);
    return text + '…';
  }
  function drawDesktopPet(t, dt) {
    const p = projects[idx];
    const state = currentState();
    const pose = poseFor(state, t);
    const U = 4, fig = FIG[character], spriteW = 47 * U, ground = 196;
    const ox = Math.round((W - spriteW) / 2), oy = ground - fig.rows * U + (pose.bob || 0) * U;
    ctx.fillStyle = 'rgba(0,0,0,.3)'; ctx.fillRect(ox + 3 * U, ground - 2, spriteW - 6 * U, 5);
    ctx.save();
    if (walk.facing < 0) { ctx.translate(ox * 2 + spriteW, 0); ctx.scale(-1, 1); }
    drawNord(ox + (pose.shake ? 2 : 0), oy, U, pose, 1);
    ctx.restore();
    drawEffects(state, pose, ox, oy, U, t, dt, 1);
    const fi = forgeInfo(p);
    if (fi) {
      drawForge(W - 46, ground, 3, p, t, state);
      const cx = W - 28, cy = ground - fi.item.length * 3 - 9, txt = `${fi.done}/${fi.total}`;
      ctx.font = `400 18px ${PIX}`; ctx.textAlign = 'center'; ctx.textBaseline = 'alphabetic';
      ctx.fillStyle = '#0b0a10'; ctx.fillRect(cx - ctx.measureText(txt).width / 2 - 4, cy - 14, ctx.measureText(txt).width + 8, 18);
      ctx.fillStyle = fi.cur === -1 ? '#fff1a8' : '#ffb07a'; ctx.fillText(txt, cx, cy);
      ctx.textAlign = 'left';
    }

    // project markers
    projects.forEach((q, i) => {
      const blink = q.state === 'waiting' && Math.floor(t * 3) % 2;
      ctx.globalAlpha = i === idx || blink ? 1 : 0.55;
      ctx.fillStyle = '#0b0a10'; ctx.fillRect(8 + i * 16, 6, 14, 14);
      ctx.fillStyle = STATES[q.state].color; ctx.fillRect(10 + i * 16, 8, 10, 10);
      if (i === idx) { ctx.fillStyle = '#fff6e0'; ctx.fillRect(10 + i * 16, 21, 10, 2); }
    });
    ctx.globalAlpha = 1;

    // label
    const lx = 6, ly = 202, lw = W - 12, lh = 54, s = STATES[p.state];
    ctx.fillStyle = '#e9dcc0'; ctx.fillRect(lx, ly, lw, lh);
    ctx.fillStyle = '#5d4a33'; ctx.fillRect(lx + 3, ly + 3, lw - 6, lh - 6);
    ctx.fillStyle = '#141827'; ctx.fillRect(lx + 5, ly + 5, lw - 10, lh - 10);
    ctx.fillStyle = s.color; ctx.fillRect(lx + 12, ly + 12, 10, 10);
    ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic';
    ctx.font = `400 15px ${PIX}`;
    ctx.fillStyle = s.color; ctx.textAlign = 'right'; ctx.fillText(s.label, lx + lw - 12, ly + 22);
    const lab = ctx.measureText(s.label).width;
    ctx.textAlign = 'left'; ctx.fillStyle = '#fff6e0';
    ctx.fillText(fit(p.name, lw - 50 - lab), lx + 28, ly + 22);
    let line = p.status, col = p.state === 'waiting' ? '#ffe28a' : '#d9d2c4';
    if (listening) { line = 'Te escucho…'; col = '#62d0f5'; }
    else if (answer && now() < answer.until) {
      col = '#f4ecd8';
      if (now() < answer.thinkUntil) line = '…';
      else {
        const shown = answer.text.slice(0, Math.floor((now() - answer.thinkUntil) * 45));
        ctx.font = `400 20px ${TXT}`;
        const ls = wrap(shown, lw - 26, ctx.font);
        line = ls[ls.length - 1] || '';
      }
    }
    // Finished mission: a button to collect the reward and put it to sleep
    acceptBtn = null;
    let room = lw - 26;
    if (p.state === 'happy' && !listening && !(answer && now() < answer.until)) {
      ctx.font = `400 15px ${PIX}`;
      const bt = 'Aceptar recompensa', bw = ctx.measureText(bt).width + 16, bh = 20;
      const bx = lx + lw - 10 - bw, by = ly + 29;
      const glow = Math.floor(t * 2) % 2;
      ctx.fillStyle = '#5d4a33'; ctx.fillRect(bx, by + 2, bw, bh);
      ctx.fillStyle = glow ? '#ffe28a' : '#f6c945'; ctx.fillRect(bx, by, bw, bh);
      ctx.fillStyle = '#2a1d05'; ctx.textAlign = 'center'; ctx.fillText(bt, bx + bw / 2, by + 15); ctx.textAlign = 'left';
      acceptBtn = { x: bx, y: by, w: bw, h: bh + 2, name: p.name };
      room = bx - lx - 18;
    }
    ctx.font = `400 20px ${TXT}`; ctx.fillStyle = col;
    ctx.fillText(fit(line, room), lx + 12, ly + 44);
  }
  let acceptBtn = null;

  let last = now();
  function frame() {""")
rep("""    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#0d0f17'; ctx.fillRect(0, 0, W, H);""", """    ctx.clearRect(0, 0, W, H);
    if (petMode) { drawDesktopPet(t, dt); setTimeout(frame, 83); return; }  // ~12 fps: light on the GPU
    ctx.fillStyle = '#0d0f17'; ctx.fillRect(0, 0, W, H);""")
rep("""const dt = Math.min(0.05, t - last);""", """const dt = Math.min(0.1, t - last);""")
rep("""    if (!autoBox.checked || listening) return;""", """    if (!autoBox.checked || listening || window.__realData) return;""")
rep("""  const start = () => requestAnimationFrame(frame);""", """  // Hooks for the desktop shell
  window.__mascota = {
    rotate, press, startTalk, endTalk, back,
    get petMode() { return petMode; },
    setPetMode(v) { petMode = v; particles = []; view = 'pet'; applySize(); },
    walk,
    get state() { return currentState(); },
    // Desktop pet: returns the mission name when (x, y) is on the "Aceptar recompensa" button
    hitAccept(x, y) {
      const b = acceptBtn;
      return b && x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h ? b.name : null;
    },
    accept(name) {
      const p = projects.find(q => q.name === name);
      if (p) setState(p, 'sleeping', 'Recompensa recogida');
    },
    loadReal(list) {
      // list: [{ name, state, status, updatedAt, forja? }]
      const known = new Map(projects.map(p => [p.name, p]));
      // A task that stopped reporting mid-work (no update in 20 min) is shown asleep
      const STALE_MS = 20 * 60 * 1000;
      list = list.map(x => {
        const at = Date.parse(x.updatedAt);
        if (['thinking', 'working'].includes(x.state) && at && Date.now() - at > STALE_MS) {
          const d = new Date(at);
          return { ...x, state: 'sleeping', status: `Sin noticias desde las ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}` };
        }
        return x;
      });
      const next = list.filter(x => STATES[x.state]).map(x => {
        const p = known.get(x.name) || { name: x.name, state: x.state, status: x.status || '', since: now(), log: [] };
        if (p.state !== x.state || p.status !== x.status) setState(p, x.state, x.status || POOL[x.state][0]);
        p.forja = x.forja && Array.isArray(x.forja.piezas) ? x.forja : null;
        return p;
      });
      if (!next.length) return;
      const cur = projects[idx] && projects[idx].name;
      projects.splice(0, projects.length, ...next);
      idx = Math.max(0, projects.findIndex(p => p.name === cur));
      syncPanel();
    },
  };
  const start = () => requestAnimationFrame(frame);""")
fontcss=open(os.path.join(HERE, 'resources', 'fonts.css'), encoding='utf-8').read()
html=f'''<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<title>Mascota de Claude</title>
<style>
{fontcss}
html, body {{ margin: 0; padding: 0; background: transparent; overflow: hidden; user-select: none; }}
#stage {{ position: fixed; inset: 0; }}
canvas#screen {{ display: block; image-rendering: pixelated; cursor: grab; }}
#menu {{ position: fixed; z-index: 5; background: #141827; border: 3px solid #e9dcc0; padding: 4px; font: 400 20px "VT323", monospace; color: #f4ecd8; min-width: 190px; box-shadow: 0 4px 0 #0b0a10; }}
#menu button {{ display: block; width: 100%; text-align: left; background: none; border: 0; color: inherit; font: inherit; padding: 4px 10px; cursor: pointer; }}
#menu button:hover, #menu button:focus-visible {{ background: #2a2238; outline: none; }}
[hidden] {{ display: none !important; }}
</style>
</head>
<body>
<div id="stage"></div>
<div id="menu" hidden>
  <button type="button" data-act="big">Abrir pantalla grande</button>
  <button type="button" data-act="pet">Volver a modo mascota</button>
  <button type="button" data-act="walk">Pasear: sí</button>
  <button type="button" data-act="char">Cambiar a guerrero sin caballo</button>
  <button type="button" data-act="demo">Demo automática: sí</button>
  <button type="button" data-act="exit">Cerrar la mascota</button>
</div>
<div id="hiddenUi" hidden>
{hidden}
</div>
{script}
<script src="js/neutralino.js"></script>
<script src="js/shell.js"></script>
</body>
</html>
'''
open(sys.argv[2], 'w', encoding='utf-8').write(html)