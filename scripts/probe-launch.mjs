#!/usr/bin/env node
/**
 * Find which UUIDs launch which system app, by trying them on a live watch.
 *
 * PebbleOS's system apps are compiled into the firmware, so their UUIDs are
 * not in any database we can read — and reciting them from memory is how you
 * end up mislabelling Alarms as Health. Instead: extract candidates from the
 * firmware image, ask the watch to launch each one (Pebble Protocol endpoint
 * 0x34, the same call app-install.js makes), and keep the ones that visibly
 * change the screen.
 *
 *   node scripts/probe-launch.mjs [--candidates file.json] [--board emery]
 *                                 [--settle 4000] [--outdir DIR]
 */

import { chromium } from 'playwright';
import { spawn } from 'child_process';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const PORT = 8139;

function arg(name, fallback = null) {
  const i = process.argv.indexOf('--' + name);
  if (i < 0) return fallback;
  const next = process.argv[i + 1];
  return next && !next.startsWith('--') ? next : true;
}

const opts = {
  candidates: String(arg('candidates', 'candidates.json')),
  board: String(arg('board', 'emery')),
  settle: Number(arg('settle', 4000)) || 4000,
  outdir: String(arg('outdir', 'probe-out')),
};

const uuidBytes = (u) => Uint8Array.from(
  u.replace(/-/g, '').match(/../g).map((h) => parseInt(h, 16)));

async function main() {
  const cands = JSON.parse(fs.readFileSync(path.resolve(opts.candidates), 'utf8'));
  console.log(`${cands.length} candidate UUIDs`);
  fs.mkdirSync(path.resolve(ROOT, opts.outdir), { recursive: true });

  const server = spawn('python3', ['server.py', String(PORT)], { cwd: ROOT, stdio: 'ignore' });
  const stop = () => { try { server.kill(); } catch (e) {} };
  process.on('exit', stop);

  const browser = await chromium.launch({
    headless: true, channel: 'chrome',
    args: ['--enable-features=SharedArrayBuffer'],
  });
  const page = await (await browser.newContext()).newPage();
  page.on('pageerror', (e) => console.error('  [page error]', e.message));

  const hits = [];
  try {
    // E-ink stays off: its ghosting and refresh flash make consecutive
    // frames differ on their own, which would drown the signal.
    await page.goto(`http://localhost:${PORT}/?board=${opts.board}`, { timeout: 60_000 });
    await page.waitForFunction(() => !!window.__emu && !!window.__phone,
                               null, { timeout: 240_000, polling: 1000 });
    console.log('watch booted; settling');
    await page.waitForTimeout(15_000);

    let prev = await page.evaluate(() => window.__emu.screenshot());
    for (const [i, c] of cands.entries()) {
      await page.evaluate(({ bytes }) => {
        const run = new Uint8Array(17);
        run[0] = 0x01;
        run.set(Uint8Array.from(bytes), 1);
        window.__phone.sendPP(0x34, run);
      }, { bytes: Array.from(uuidBytes(c.uuid)) });

      await page.waitForTimeout(opts.settle);
      const shot = await page.evaluate(() => window.__emu.screenshot());
      if (shot !== prev) {
        const file = path.resolve(ROOT, opts.outdir, `${c.uuid}.png`);
        fs.writeFileSync(file, Buffer.from(shot.split(',')[1], 'base64'));
        hits.push(c);
        console.log(`  [${i + 1}/${cands.length}] LAUNCHED ${c.uuid} (near "${c.hint}")`);
        prev = shot;
      } else if ((i + 1) % 10 === 0) {
        console.log(`  [${i + 1}/${cands.length}] …`);
      }
    }
  } catch (e) {
    console.error('failed:', e.message);
  } finally {
    await browser.close();
    stop();
  }

  fs.writeFileSync(path.resolve(ROOT, opts.outdir, 'hits.json'),
                   JSON.stringify(hits, null, 2));
  console.log(`\n${hits.length} UUIDs changed the screen; PNGs in ${opts.outdir}/`);
}

main().catch((e) => { console.error('fatal:', e.message); process.exitCode = 1; });
