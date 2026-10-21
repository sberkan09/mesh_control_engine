// Drives the web probes of a web build (M-WEB) in headless Chromium and checks what they log.
//
//   NODE_PATH=$(npm root -g) node scripts/web/probe_smoke.cjs <dir> input
//   NODE_PATH=$(npm root -g) node scripts/web/probe_smoke.cjs <dir> relay <path/to/native/mce_relay>
//   NODE_PATH=$(npm root -g) node scripts/web/probe_smoke.cjs <dir> rtc <path/to/native/mce_relay>
//   NODE_PATH=$(npm root -g) node scripts/web/probe_smoke.cjs <dir> shell
//
// input: mce_input_web_probe (src/engine/platform/web_probe). Audio must start suspended (the
//   browser's autoplay rule) and run after the first click; the click must ask for pointer lock; a
//   key press, a tap and a gamepad (a fake one behind navigator.getGamepads) must reach the engine.
// relay: starts the native mce_relay on a free WebSocket port and opens mce_relay_web_probe, whose
//   two browser RelayClients make a room, join it and swap a message (M-NET2 over WebSocket).
// rtc: starts the native mce_relay with its echo member (-echo_mode rtc_probe, a native RtcHost on
//   libdatachannel) and opens mce_rtc_web_probe, whose browser RtcHost joins it and connects
//   directly over RTCPeerConnection (M-NET3 step 4); messages must come back over the direct link.
// shell: a shipped web folder (scripts/ship.py --target web): on a desktop-sized page and on a phone-sized
//   touch one, the cover shows the icon and "Click/Tap to play" once the game runs, the click or tap
//   hides it and starts the audio, and the canvas fills the page and follows a resize.
//
// Needs Playwright (npm -g) and a Chromium; PLAYWRIGHT_BROWSERS_PATH or /opt/pw-browsers/chromium.
const http = require('http');
const net = require('net');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const { chromium } = require('playwright');

const [dir, mode, relayExe] = process.argv.slice(2);
if (!dir || !['input', 'relay', 'rtc', 'shell'].includes(mode) || (['relay', 'rtc'].includes(mode) && !relayExe)) {
  console.error('usage: probe_smoke.cjs <dir> input | relay <mce_relay> | rtc <mce_relay> | shell');
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

// The shipped page on a desktop browser and on a phone.
async function shellChecks(chromium, exe, url) {
  const browser = await chromium.launch({
    executablePath: exe,
    args: ['--headless=new', '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
           '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader'],
  });
  let ok = true;
  const check = (what, pass) => {
    console.log(`probe: ${pass ? 'ok  ' : 'FAIL'} ${what}`);
    if (!pass) ok = false;
  };
  const devices = [
    { label: 'desktop', options: { viewport: { width: 1280, height: 800 } }, gesture: 'click', resize: { width: 900, height: 600 } },
    { label: 'phone', options: { viewport: { width: 390, height: 844 }, deviceScaleFactor: 3, isMobile: true, hasTouch: true },
      gesture: 'tap', resize: { width: 844, height: 390 } },
  ];
  try {
    for (const d of devices) {
      const context = await browser.newContext(d.options);
      const tab = await context.newPage();
      const errors = [];
      tab.on('console', (m) => { if (/\[(error|critical)\]/.test(m.text())) errors.push(m.text()); });
      tab.on('pageerror', (e) => errors.push(e.message));
      await tab.addInitScript(strictAutoplay);
      await tab.goto(url);
      const play = tab.locator('#mce-play');
      let shown = false;
      for (let t = 0; !shown && t < 60000; t += 200) {
        shown = await play.isVisible();
        if (!shown) await tab.waitForTimeout(200);
      }
      check(`${d.label}: the cover offers "${d.gesture === 'tap' ? 'Tap' : 'Click'} to play" once the game runs`,
            shown && (await play.textContent()).startsWith(d.gesture === 'tap' ? 'Tap' : 'Click'));
      const icon = await tab.evaluate(() => { const i = document.querySelector('#mce-cover img'); return !!i && i.naturalWidth > 0; });
      if (fs.existsSync(path.join(root, 'icon.png'))) check(`${d.label}: the cover shows the game's icon`, icon);
      else console.log(`probe: --   ${d.label}: no icon (the game has no ship/icon.png)`);
      const audioBefore = await tab.evaluate(() => (Module.mceAudioState ? Module.mceAudioState() : 'none'));
      if (d.gesture === 'tap') await play.tap(); else await play.click();
      await tab.waitForTimeout(500);
      check(`${d.label}: the ${d.gesture} hides the cover`, !(await tab.locator('#mce-cover').isVisible()));
      const audioAfter = await tab.evaluate(() => (Module.mceAudioState ? Module.mceAudioState() : 'none'));
      // (Before it, headless Chromium may already count the page as activated; the input probe checks the
      // suspended start on its own.)
      if (audioBefore === 'none' && audioAfter === 'none') console.log(`probe: --   ${d.label}: no audio (the game opens no audio device)`);
      else check(`${d.label}: the audio runs after the ${d.gesture} (${audioBefore} -> ${audioAfter})`, audioAfter === 'running');
      const size = () => tab.evaluate(() => {
        const c = document.getElementById('canvas');
        return { css: [c.clientWidth, c.clientHeight], px: [c.width, c.height], view: [innerWidth, innerHeight], dpr: devicePixelRatio };
      });
      let s = await size();
      check(`${d.label}: the canvas fills the page (${s.css} css, ${s.px} px)`,
            s.css[0] === s.view[0] && s.css[1] === s.view[1] && s.px[0] >= s.css[0] && s.px[1] >= s.css[1]);
      await tab.setViewportSize(d.resize);
      await tab.waitForTimeout(800);
      s = await size();
      check(`${d.label}: the canvas follows a resize to ${d.resize.width}x${d.resize.height} (${s.css} css, ${s.px} px)`,
            s.css[0] === d.resize.width && s.css[1] === d.resize.height && s.px[0] >= s.css[0] && s.px[1] >= s.css[1]);
      await tab.waitForTimeout(500);
      await tab.screenshot({ path: path.join(root, `..`, `shell-${d.label}.png`) });
      check(`${d.label}: no errors (${errors.slice(0, 2).join(' | ')})`, errors.length === 0);
      await context.close();
    }
  } finally {
    await browser.close();
  }
  return ok;
}

(async () => {
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  let relay = null;
  const exe = fs.existsSync('/opt/pw-browsers/chromium') ? '/opt/pw-browsers/chromium' : undefined;
  const browser = await chromium.launch({
    executablePath: exe,
    // Host candidates as addresses, not mDNS names: the native peer resolves no .local names.
    args: ['--headless=new', '--disable-features=WebRtcHideLocalIpsWithMdns'],
  });
  const lines = [];
  let failed = false;
  if (mode === 'shell') {
    browser.close();
    failed = !(await shellChecks(chromium, exe, `http://127.0.0.1:${server.address().port}/index.html`));
    server.close();
    console.log(failed ? 'probe: FAIL' : 'probe: PASS');
    process.exit(failed ? 1 : 0);
  }
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
    if (mode === 'relay' || mode === 'rtc') {
      const port = await freePort();
      const args = ['-bind', '127.0.0.1', '-udp_port', '0', '-ws_port', String(port), '-seconds', '120'];
      // The echo member reaches the relay over UDP (a free port), and connects to the page over WebRTC.
      if (mode === 'rtc') args.splice(2, 2, '-udp_port', String(await freePort()), '-echo_mode', 'rtc_probe');
      relay = spawn(relayExe, args, { stdio: ['ignore', 'pipe', 'pipe'] });
      relay.stdout.on('data', (d) => process.stdout.write(`relay| ${d}`));
      relay.stderr.on('data', (d) => process.stdout.write(`relay| ${d}`));
      await new Promise((r) => setTimeout(r, 500));
      const probe = mode === 'rtc' ? 'rtc' : 'relay';
      await tab.goto(`${base}/mce_${probe}_web_probe.html?port=${port}`);
      const done = new RegExp(`${probe} web probe: (OK|FAILED)`);
      check(`${probe} web probe reports OK`, await waitFor(tab, done, 40000) &&
                                            lines.some((l) => new RegExp(`${probe} web probe: OK`).test(l)));
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
