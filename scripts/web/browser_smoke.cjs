// Browser smoke for a web build (M-WEB, ADR-0039): serves a build directory, opens <page>.html in
// headless Chromium with WebGPU on SwiftShader (no GPU needed), waits for the app's "running" log
// line, lets it draw for a while, saves a screenshot and fails when the canvas is one flat colour.
//
//   NODE_PATH=$(npm root -g) node scripts/web/browser_smoke.cjs <dir> <page> [out.png] [ms] [--persist]
//
// --persist also checks saves survive a reload: it writes a probe file under /persistent, asks the
// page to store it in IndexedDB, reloads, and expects the file back once the app runs again.
//
// Needs Playwright (npm -g) and a Chromium; PLAYWRIGHT_BROWSERS_PATH or /opt/pw-browsers/chromium.
const http = require('http');
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const persist = process.argv.includes('--persist');
const [dir, page, out = 'smoke.png', ms = '3000'] = process.argv.slice(2).filter((a) => a !== '--persist');
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
    tab.on('console', (m) => {
      console.log(m.text());
      if (/: running \(/.test(m.text())) running = true;
      if (/\[(error|critical)\]/.test(m.text())) failed = true;
    });
    tab.on('pageerror', (e) => { console.log(`pageerror: ${e.message}`); failed = true; });
    const url = `http://127.0.0.1:${server.address().port}/${page}.html`;
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
    await tab.waitForTimeout(Number(ms));
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
  } finally {
    await browser.close();
    server.close();
  }
  console.log(failed ? 'smoke: FAIL' : 'smoke: PASS');
  process.exit(failed ? 1 : 0);
})();
