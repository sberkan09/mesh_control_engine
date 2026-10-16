// Drives the web probes of a web build (M-WEB) in headless Chromium and checks what they log.
//
//   NODE_PATH=$(npm root -g) node scripts/web/probe_smoke.cjs <dir> input
//   NODE_PATH=$(npm root -g) node scripts/web/probe_smoke.cjs <dir> relay <path/to/native/mce_relay>
//
// input: mce_input_web_probe (src/engine/platform/web_probe). Audio must start suspended (the
//   browser's autoplay rule) and run after the first click; the click must ask for pointer lock; a
//   key press, a tap and a gamepad (a fake one behind navigator.getGamepads) must reach the engine.
// relay: starts the native mce_relay on a free WebSocket port and opens mce_relay_web_probe, whose
//   two browser RelayClients make a room, join it and swap a message (M-NET2 over WebSocket).
//
// Needs Playwright (npm -g) and a Chromium; PLAYWRIGHT_BROWSERS_PATH or /opt/pw-browsers/chromium.
const http = require('http');
const net = require('net');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const { chromium } = require('playwright');

const [dir, mode, relayExe] = process.argv.slice(2);
if (!dir || !['input', 'relay'].includes(mode) || (mode === 'relay' && !relayExe)) {
  console.error('usage: probe_smoke.cjs <dir> input | relay <mce_relay>');
  process.exit(2);
}
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const root = path.resolve(dir);
const server = http.createServer((req, res) => {
  const file = path.join(root, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404).end();
    return;
  }
  res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});

const freePort = () => new Promise((resolve) => {
  const s = net.createServer();
  s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); });
});

// A standard-mapping pad the page sees through navigator.getGamepads; window.mcePad presses South.
const fakeGamepad = () => {
  const pad = {
    id: 'MCE Fake Pad (STANDARD GAMEPAD Vendor: 045e Product: 028e)', index: 0, connected: true,
    mapping: 'standard', timestamp: 1, axes: [0, 0, 0, 0],
    buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })),
  };
  let plugged = false;
  navigator.getGamepads = () => (plugged ? [pad, null, null, null] : [null, null, null, null]);
  window.mcePad = {
    plug() {
      plugged = true;
      const e = new Event('gamepadconnected');
      Object.defineProperty(e, 'gamepad', { value: pad });
      window.dispatchEvent(e);
    },
    press(down) {
      pad.buttons[0] = { pressed: down, touched: down, value: down ? 1 : 0 };
      pad.timestamp += 1;
    },
    // SDL takes the first value it sees on an axis as the stick's rest position, so a real pad's
    // first report never moves anything: tilt the stick a little before pushing it all the way.
    stick(x) {
      pad.axes[0] = x;
      pad.timestamp += 1;
    },
  };
};

// Headless Chromium lets audio start without a gesture whatever its autoplay flag says, so the page
// gets the strict rule (Safari's, and Chrome's on a site the user never touched): an AudioContext
// made before the first gesture starts suspended.
const strictAutoplay = () => {
  const Real = window.AudioContext;
  window.AudioContext = class extends Real {
    constructor(...a) {
      super(...a);
      if (!navigator.userActivation.hasBeenActive) this.suspend();
    }
  };
};

(async () => {
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  let relay = null;
  const exe = fs.existsSync('/opt/pw-browsers/chromium') ? '/opt/pw-browsers/chromium' : undefined;
  const browser = await chromium.launch({
    executablePath: exe,
    args: ['--headless=new'],
  });
  const lines = [];
  let failed = false;
  const check = (what, ok) => {
    console.log(`probe: ${ok ? 'ok  ' : 'FAIL'} ${what}`);
    if (!ok) failed = true;
  };
  const waitFor = async (tab, re, ms = 10000) => {
    for (let t = 0; t < ms; t += 50) {
      if (lines.some((l) => re.test(l))) return true;
      await tab.waitForTimeout(50);
    }
    return false;
  };
  try {
    const context = await browser.newContext({ viewport: { width: 1280, height: 800 }, hasTouch: true });
    const tab = await context.newPage();
    tab.on('console', (m) => { lines.push(m.text()); console.log(m.text()); });
    tab.on("pageerror", (e) => { console.log(`pageerror: ${e.message}\n${e.stack}`); failed = true; });
    const base = `http://127.0.0.1:${server.address().port}`;
    if (mode === 'relay') {
      const port = await freePort();
      relay = spawn(relayExe, ['-bind', '127.0.0.1', '-udp_port', '0', '-ws_port', String(port), '-seconds', '120'],
                    { stdio: ['ignore', 'pipe', 'pipe'] });
      relay.stdout.on('data', (d) => process.stdout.write(`relay| ${d}`));
      relay.stderr.on('data', (d) => process.stdout.write(`relay| ${d}`));
      await new Promise((r) => setTimeout(r, 500));
      await tab.goto(`${base}/mce_relay_web_probe.html?port=${port}`);
      check('relay web probe reports OK', await waitFor(tab, /relay web probe: (OK|FAILED)/, 30000) &&
                                          lines.some((l) => /relay web probe: OK/.test(l)));
    } else {
      await tab.addInitScript(fakeGamepad);
      await tab.addInitScript(strictAutoplay);
      await tab.goto(`${base}/mce_input_web_probe.html`);
      check('probe runs', await waitFor(tab, /input web probe: running/, 30000));
      check('audio starts suspended', await waitFor(tab, /probe: audio suspended/));
      const box = await tab.locator('#canvas').boundingBox();
      const cdp = await context.newCDPSession(tab);
      const at = [{ x: box.x + box.width / 4, y: box.y + box.height / 2, id: 1 }];
      await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: at });
      await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
      check('a tap reaches the engine', await waitFor(tab, /probe: touch down 0 at 0\.2/) &&
                                        await waitFor(tab, /probe: touch up 0/));
      check('the first tap resumes audio', await waitFor(tab, /probe: audio running/));
      await tab.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
      check('a click reaches the engine', await waitFor(tab, /probe: mouse button 1/));
      check('the click asks for pointer lock', await waitFor(tab, /probe: pointer lock request accepted/));
      await tab.keyboard.press('KeyW');
      check('a key press reaches the engine', await waitFor(tab, /probe: key 26/));
      await tab.evaluate(() => window.mcePad.plug());
      check('a gamepad connects', await waitFor(tab, /probe: gamepad added 0/));
      await tab.evaluate(() => window.mcePad.press(true));
      check('a gamepad button reaches the engine', await waitFor(tab, /probe: gamepad button 0 on 0/));
      await tab.evaluate(() => window.mcePad.stick(0.1));
      await tab.waitForTimeout(200);
      await tab.evaluate(() => window.mcePad.stick(1));
      check('a gamepad stick reaches the engine', await waitFor(tab, /probe: gamepad axis 0 1\.00/));
      await tab.keyboard.press('KeyF');
      check('F asks for full screen', await waitFor(tab, /probe: fullscreen request accepted/));
      await tab.mouse.click(box.x + 10, box.y + 10);
      await tab.waitForTimeout(300);
      // Both requests wait for the next gesture (the browser's rule); that click grants them.
      const held = await tab.evaluate(() => [document.fullscreenElement, document.pointerLockElement].map((e) => e && e.id));
      check('the canvas goes full screen', held[0] === 'canvas');
      check('the canvas holds the pointer lock', held[1] === 'canvas');
      await tab.keyboard.press('Escape');
      check('the probe ends', await waitFor(tab, /input web probe: done/));
    }
    if (lines.some((l) => /\[(error|critical)\]/.test(l))) check('no errors logged', false);
  } finally {
    await browser.close();
    server.close();
    if (relay) relay.kill();
  }
  console.log(failed ? 'probe: FAIL' : 'probe: PASS');
  process.exit(failed ? 1 : 0);
})();
