// Browser smoke for a web build (M-WEB, ADR-0039): serves a build directory, opens <page>.html in
// headless Chromium with WebGPU on SwiftShader (no GPU needed), waits for the app's "running" log
// line, lets it draw for a while, saves a screenshot and fails when the canvas is one flat colour.
//
//   NODE_PATH=$(npm root -g) node scripts/web/browser_smoke.cjs <dir> <page> [out.png] [ms] [--persist] [--ticks N]
//
// --ticks N (a player-based game: a project, or a sample on mce_player) runs the game N sim ticks drawn,
// fast-forwarded (-run_ticks N -sim.fast_forward 1, through the page's query), screenshots its last
// frame and prints the "state_hash" line it ends with. A second page runs the same build headless
// (-headless 1 -ticks N) in the browser; the two hashes must match.
//
// --persist also checks saves survive a reload: it writes a probe file under /persistent, asks the
// page to store it in IndexedDB, reloads, and expects the file back once the app runs again.
//
// Needs Playwright (npm -g) and a Chromium; PLAYWRIGHT_BROWSERS_PATH or /opt/pw-browsers/chromium.
const http = require('http');
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const argv = process.argv.slice(2);
const persist = argv.includes('--persist');
const ticksAt = argv.indexOf('--ticks');
const ticks = ticksAt >= 0 ? Number(argv[ticksAt + 1]) : 0;
const [dir, page, out = 'smoke.png', ms = '3000'] =
  argv.filter((a, i) => a !== '--persist' && !(ticksAt >= 0 && (i === ticksAt || i === ticksAt + 1)));
if (ticksAt >= 0 && !(ticks > 0)) {
  console.error('browser_smoke: --ticks wants a positive tick count');
  process.exit(2);
}
if (!dir || !page) {
  console.error('usage: browser_smoke.cjs <dir> <page> [out.png] [ms]');
  process.exit(2);
}
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.mce': 'application/octet-stream' };

const server = http.createServer((req, res) => {
  const file = path.join(path.resolve(dir), decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!file.startsWith(path.resolve(dir)) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404).end();
    return;
  }
  res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});

(async () => {
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const exe = fs.existsSync('/opt/pw-browsers/chromium') ? '/opt/pw-browsers/chromium' : undefined;
  const browser = await chromium.launch({
    executablePath: exe,
    args: ['--headless=new', '--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
           '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader'],
  });
  let failed = false;
  try {
    const tab = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    let running = false;
    let hash = null;
    tab.on('console', (m) => {
      console.log(m.text());
      if (/: running \(/.test(m.text())) running = true;
      const h = /state_hash ([0-9a-f]{16})/.exec(m.text());
      if (h) hash = h[1];
      if (/\[(error|critical)\]/.test(m.text())) failed = true;
    });
    tab.on('pageerror', (e) => { console.log(`pageerror: ${e.message}`); failed = true; });
    const query = ticks ? `?run_ticks=${ticks}&sim.fast_forward=1&sim.max_catchup=4` : '';
    const url = `http://127.0.0.1:${server.address().port}/${page}.html${query}`;
    await tab.goto(url);
    for (let t = 0; !running && !failed && t < 60000; t += 100) await tab.waitForTimeout(100);
    if (!running) { console.log('smoke: the app never reported running'); failed = true; }
    if (persist && running) {
      const probe = `probe ${Date.now()}`;
      await tab.evaluate((text) => {
        Module.FS.writeFile('/persistent/smoke_probe.txt', text);
        Module.mcePersist();
      }, probe);
      await tab.waitForTimeout(1000);
      running = false;
      await tab.reload();
      for (let t = 0; !running && !failed && t < 60000; t += 100) await tab.waitForTimeout(100);
      const back = await tab.evaluate(() => {
        try { return Module.FS.readFile('/persistent/smoke_probe.txt', { encoding: 'utf8' }); } catch (e) { return null; }
      });
      console.log(`smoke: persistent probe after reload: ${back === probe ? 'kept' : 'LOST'}`);
      if (back !== probe) failed = true;
    }
    let release = false;
    if (ticks && running) {
      // The game prints its hash after the last tick and keeps its last frame on the page
      // (web::linger) until released, so the screenshot below shows it.
      for (let t = 0; hash === null && !failed && t < 300000; t += 100) await tab.waitForTimeout(100);
      console.log(hash ? `smoke: state_hash ${hash} after ${ticks} ticks` : 'smoke: the game never printed its state hash');
      if (!hash) failed = true;
      for (let t = 0; hash && t < 5000 && !(await tab.evaluate(() => !!Module.mceLingering)); t += 50) await tab.waitForTimeout(50);
      release = true;
    } else {
      await tab.waitForTimeout(Number(ms));
    }
    // The shipped page's cover (scripts/web/shell.html) would hide the game; the smoke presses nothing.
    await tab.evaluate(() => { const c = document.getElementById('mce-cover'); if (c) c.style.display = 'none'; });
    const canvas = tab.locator('#canvas');
    const png = await canvas.screenshot({ path: out });
    // A flat canvas means nothing drew: decode the screenshot (a WebGPU canvas cannot be read back
    // after it is presented), sample a grid and require more than one colour.
    const colours = await tab.evaluate(async (url) => {
      const img = new Image();
      img.src = url;
      await img.decode();
      const s = document.createElement('canvas');
      s.width = img.width; s.height = img.height;
      const g = s.getContext('2d', { willReadFrequently: true });
      g.drawImage(img, 0, 0);
      const seen = new Set();
      for (let y = 8; y < s.height; y += 16)
        for (let x = 8; x < s.width; x += 16) seen.add(g.getImageData(x, y, 1, 1).data.join(','));
      return seen.size;
    }, `data:image/png;base64,${png.toString('base64')}`);
    console.log(`smoke: ${colours} distinct colours on the canvas, screenshot ${out}`);
    if (colours < 2) failed = true;
    if (release) await tab.evaluate(() => Module.mceRelease && Module.mceRelease());
    if (ticks && hash) {
      // The same build headless in the browser: no window, no renderer, the same N ticks.
      const head = await browser.newPage();
      let headHash = null;
      head.on('console', (m) => { const h = /state_hash ([0-9a-f]{16})/.exec(m.text()); if (h) headHash = h[1]; });
      head.on('pageerror', (e) => { console.log(`pageerror (headless): ${e.message}`); failed = true; });
      await head.goto(`http://127.0.0.1:${server.address().port}/${page}.html?headless=1&ticks=${ticks}`);
      for (let t = 0; headHash === null && !failed && t < 300000; t += 100) await head.waitForTimeout(100);
      console.log(`smoke: headless state_hash ${headHash} (${headHash === hash ? 'matches the drawn run' : 'DIFFERS from the drawn run'})`);
      if (headHash !== hash) failed = true;
    }
  } finally {
    await browser.close();
    server.close();
  }
  console.log(failed ? 'smoke: FAIL' : 'smoke: PASS');
  process.exit(failed ? 1 : 0);
})();
