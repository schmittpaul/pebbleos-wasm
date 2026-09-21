#!/usr/bin/env node
/**
 * Boot a firmware directory in the wasm emulator and screenshot the result.
 *
 * Firmware built from a PebbleOS checkout drops into firmware/<name>/ and is
 * selected with ?fw=<name>, so a custom build runs on a stock machine with no
 * board definition of its own.
 *
 *   node scripts/boot-firmware.mjs [--fw pebbleos-dev] [--board emery]
 *                                  [--settle 45000] [--out shot.png]
 */

import { chromium } from 'playwright';
import { spawn } from 'child_process';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const PORT = 8141;

function arg(name, fallback) {
  const i = process.argv.indexOf('--' + name);
  if (i < 0) return fallback;
  const next = process.argv[i + 1];
  return next && !next.startsWith('--') ? next : true;
}

const opts = {
  fw: String(arg('fw', 'pebbleos-dev')),
  board: String(arg('board', 'emery')),
  settle: Number(arg('settle', 45000)) || 45000,
  boot: Number(arg('boot', 240000)) || 240000,
  out: String(arg('out', 'fw-boot.png')),
  press: arg('press') === true ? '' : String(arg('press', '')),
  hold: Number(arg('hold', 250)) || 250,
};

const server = spawn('python3', ['server.py', String(PORT)], { cwd: ROOT, stdio: 'ignore' });
const browser = await chromium.launch({
  headless: true, channel: 'chrome', args: ['--enable-features=SharedArrayBuffer'],
});
const page = await (await browser.newContext()).newPage();

const logs = [];
page.on('console', (m) => {
  const t = m.text();
  logs.push(t);
  // The firmware's own serial output is the only signal that the image is
  // alive before anything reaches the screen.
  if (/panic|HALT|assert|\[err\]|launcher|Ready/i.test(t)) console.log('  ·', t.slice(0, 160));
});
page.on('pageerror', (e) => console.error('  [page error]', e.message));

const out = path.resolve(ROOT, opts.out);
try {
  const url = `http://localhost:${PORT}/?board=${opts.board}&fw=${opts.fw}`;
  console.log(`booting ${url}`);
  await page.goto(url, { timeout: 60_000 });
  await page.waitForFunction(() => !!window.__emu, null,
                             { timeout: opts.boot, polling: 2000 });
  console.log('emulator API up; settling');
  await page.waitForTimeout(opts.settle);

  for (const name of opts.press.split(',').map((x) => x.trim()).filter(Boolean)) {
    console.log(`press ${name}`);
    await page.evaluate((n) => window.__emu.pressButton(n, true), name);
    await page.waitForTimeout(opts.hold);
    await page.evaluate((n) => window.__emu.pressButton(n, false), name);
    await page.waitForTimeout(2500);
  }

  await page.locator('#canvas').screenshot({ path: out });
  console.log('screenshot:', out);
} catch (e) {
  console.error('failed:', e.message);
  try {
    await page.locator('#canvas').screenshot({ path: out });
    console.log('screenshot (partial):', out);
  } catch (_) { /* nothing rendered */ }
  process.exitCode = 1;
} finally {
  fs.writeFileSync(out + '.log', logs.join('\n'));
  console.log(`log: ${out}.log (${logs.length} lines)`);
  await browser.close();
  server.kill();
}
