#!/usr/bin/env node
/**
 * Build a local Pebble app project and install it on the emulated watch.
 *
 * There is no Pebble SDK or arm-none-eabi toolchain on this machine — the
 * only compiler is appbuild/, which runs clang.wasm in the browser. So the
 * dev loop is: zip the project, boot index.html under Playwright, hand the
 * zip to the page's build worker (window.__buildZip), and screenshot the
 * result.
 *
 *   node scripts/dev-app.mjs [--app apps/radial-eink] [--board emery]
 *                            [--eink] [--headless] [--keep]
 *                            [--press up,down,select] [--crown 3]
 *                            [--out /tmp/shot.png]
 */

import { chromium } from 'playwright';
import { spawn } from 'child_process';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import JSZip from 'jszip';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const PORT = 8137;
const BOOT_TIMEOUT = 240_000;
const BUILD_TIMEOUT = 300_000;

function arg(name, fallback = null) {
  const i = process.argv.indexOf('--' + name);
  if (i < 0) return fallback;
  const next = process.argv[i + 1];
  return next && !next.startsWith('--') ? next : true;
}

const opts = {
  app: String(arg('app', 'apps/radial-eink')),
  board: String(arg('board', 'emery')),
  eink: !!arg('eink'),
  headless: !!arg('headless'),
  keep: !!arg('keep'),
  press: arg('press') === true ? '' : String(arg('press', '')),
  crown: Number(arg('crown', 0)) || 0,
  out: String(arg('out', 'apps/radial-eink/shot.png')),
  browser: String(arg('browser', 'auto')),  // auto | chromium | chrome
  holdMs: Number(arg('hold', 250)) || 250,
};

// Zip the project the way the build worker expects a repo zip: everything
// under one top-level directory, skipping build output and VCS noise.
async function zipProject(dir) {
  const zip = new JSZip();
  const top = path.basename(dir);
  const skip = new Set(['node_modules', 'build', '.git', '.venv']);
  let count = 0;
  (function walk(abs, rel) {
    for (const entry of fs.readdirSync(abs, { withFileTypes: true })) {
      if (skip.has(entry.name) || entry.name.startsWith('.')) continue;
      const a = path.join(abs, entry.name);
      const r = rel ? `${rel}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(a, r);
      else { zip.file(`${top}/${r}`, fs.readFileSync(a)); count++; }
    }
  })(path.resolve(dir), '');
  if (!count) throw new Error(`no files found under ${dir}`);
  console.log(`zipped ${count} files from ${dir}`);
  return zip.generateAsync({ type: 'nodebuffer', compression: 'DEFLATE' });
}

async function main() {
  const server = spawn('python3', ['server.py', String(PORT)], {
    cwd: ROOT, stdio: 'ignore',
  });
  const stopServer = () => { try { server.kill(); } catch (e) {} };
  process.on('exit', stopServer);

  const zipBuf = await zipProject(path.join(ROOT, opts.app));

  // Prefer Playwright's own chromium, but fall back to an installed Chrome
  // rather than blocking on a ~170 MB download. Either way the profile is a
  // throwaway temp dir, so the user's real Chrome session is untouched.
  // channel:'chromium' keeps the full build in headless mode; the default
  // headless path wants a chrome-headless-shell download that
  // `playwright install chromium` does not provide.
  const bundled = (() => {
    try { return fs.existsSync(chromium.executablePath()); } catch (e) { return false; }
  })();
  const channel = opts.browser !== 'auto' ? opts.browser
                : bundled ? (opts.headless ? 'chromium' : undefined)
                : 'chrome';
  console.log(`browser: ${channel || 'bundled chromium'}`);
  const browser = await chromium.launch({
    headless: opts.headless,
    channel,
    args: ['--enable-features=SharedArrayBuffer'],
  });
  const page = await (await browser.newContext()).newPage();

  page.on('console', (m) => {
    const t = m.text();
    if (/\[build\]|\[install\]|\[fatal\]|error/i.test(t)) console.log('  ·', t);
  });
  page.on('pageerror', (e) => console.error('  [page error]', e.message));

  let ok = false;
  try {
    const url = `http://localhost:${PORT}/?board=${opts.board}` +
                (opts.eink ? '&eink=1' : '');
    console.log(`booting ${url}`);
    await page.goto(url, { timeout: 60_000 });

    await page.waitForFunction(() => !!window.__emu,
                               null, { timeout: BOOT_TIMEOUT, polling: 1000 });
    console.log('watch booted');

    // The first build downloads ~39 MB of toolchain; after that it is cached.
    console.log('building…');
    await page.evaluate(async (bytes) => {
      await window.__buildZip(new Uint8Array(bytes));
    }, Array.from(zipBuf));

    const status = await page.waitForFunction(() => {
      const t = document.getElementById('install-status').textContent;
      if (t.includes('Installed and launching')) return t;
      if (t.includes('failed')) return t;
      return null;
    }, null, { timeout: BUILD_TIMEOUT, polling: 1000 }).then((h) => h.jsonValue());

    console.log(`status: ${status}`);
    ok = status.includes('Installed and launching');

    // Let the app get a first frame up before touching anything.
    await page.waitForTimeout(6000);

    for (const name of opts.press.split(',').map((s) => s.trim()).filter(Boolean)) {
      console.log(`press ${name}`);
      await page.evaluate((n) => window.__emu.pressButton(n, true), name);
      // Short enough to stay under the app's auto-repeat interval, so one
      // --press is one step; the guest polls buttons every 16 ms.
      await page.waitForTimeout(opts.holdMs);
      await page.evaluate((n) => window.__emu.pressButton(n, false), name);
      await page.waitForTimeout(1500);
    }

    if (opts.crown) {
      console.log(`crown ${opts.crown} detents`);
      await page.evaluate((n) => window.__crown.turn(n), opts.crown);
      await page.waitForTimeout(1000 + Math.abs(opts.crown) * 400);
    }

    if (opts.eink) {
      // Changed-pixel fraction per partial refresh: the basis for choosing
      // the page-turn threshold rather than guessing it.
      const hist = await page.evaluate(() => window.__eink.history());
      const big = hist.filter((f) => f > 0.005).sort((a, b) => b - a);
      console.log(`eink refreshes: ${hist.length}, ` +
                  `top changed fractions: ${big.slice(0, 12).join(' ')}`);
    }

    const out = path.resolve(ROOT, opts.out);
    fs.mkdirSync(path.dirname(out), { recursive: true });
    await page.locator('#canvas').screenshot({ path: out });
    console.log(`screenshot: ${out}`);

    if (opts.keep) {
      console.log('--keep: leaving the browser open, Ctrl-C to quit');
      await new Promise(() => {});
    }
  } catch (e) {
    console.error('failed:', e.message);
    try { await page.screenshot({ path: path.resolve(ROOT, 'dev-app-error.png') }); } catch (_) {}
  } finally {
    if (!opts.keep) await browser.close();
    stopServer();
  }
  process.exitCode = ok ? 0 : 1;
}

main().catch((e) => {
  console.error('fatal:', e.message);
  process.exitCode = 1;
});
