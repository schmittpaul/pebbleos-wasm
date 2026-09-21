# Radial — a clock-face menu for an e-ink Pebble

An experiment in what a Pebble would be if the panel were e-paper and the
menu were laid out on the watch face instead of in a list.

Two ideas, one screen:

* **The face is analog.** Hour ticks, hour and minute hands, no digits.
* **The menu lives on the face.** Menu items do not stack vertically — item
  one sits at 12 o'clock, item two at 1 o'clock, and so on clockwise. Up and
  Down step by one hour position; so does a crown detent. The selected item
  is the only solid mass on the dial, and its name sits in the middle where
  the hands were.

Stepping by hour positions is the whole point: it is the interaction a crown
wants, and it gives every item a fixed, memorable place on the dial rather
than a position that depends on how far you have scrolled.

## Why it is a watchapp and not a watchface

Pebble watchfaces do not receive button events — the system keeps Up, Down
and Back for the app switcher and the launcher. Since the whole idea is
driven by buttons, this is registered as a watchapp (`watchface: false`), so
it shows up under the app menu rather than in the watchface carousel. The
face still renders as a watchface would.

The public SDK also has no API for one app to launch another, so activating
an item pushes a detail screen inside this app rather than opening the real
Timeline or Music app. Making the radial menu *be* the system launcher means
forking PebbleOS, which is a separate piece of work.

## Controls

| Input | Effect |
|-------|--------|
| Up / Down | Open the menu; then step one hour position counter-clockwise / clockwise |
| Select | Open the menu at 12 o'clock; then activate the selected item |
| Back | Close the menu; from the face, exit the app |
| Crown | One detent = one hour position (the host turns detents into Up/Down presses) |

The menu closes itself after 8 seconds of no input, so an idle watch settles
back to the face instead of holding a menu on the panel.

## Building and running

There is no Pebble SDK on this machine; `appbuild/` (clang compiled to wasm,
running in the browser) is the only compiler. `scripts/dev-app.mjs` drives
the whole loop — it zips this directory, boots the emulator under Playwright,
hands the zip to the page's build worker, installs the result and
screenshots the watch:

```sh
node scripts/dev-app.mjs --eink --press select --crown 3
```

Useful flags: `--board <emery|flint|gabbro>`, `--eink` to boot with the
e-ink filter on, `--keep` to leave the browser open for poking at it by
hand, `--press up,down,select`, `--crown <detents>`, `--out <png>`.

The first run downloads about 39 MB of toolchain; later runs hit the Cache
API and take seconds.

To iterate by hand instead, run `python3 server.py 8080`, open
`http://localhost:8080/?eink=1`, and drag a built `.pbw` onto the screen.

## Layout

All radii come from one base radius in `dial_make()`, so the same layout
holds on emery (200×228), flint (144×168) and gabbro (260×260 round).
Twelve slots always exist; the menu simply uses the first N of them.
