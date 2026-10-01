# NOODLE-9 // 第九麺場

A cyberpunk-styled front-end for a fictional pickup-only ramen chain.
Pure static HTML + CSS + JavaScript. **No backend, no build step, no tracking.**

---

## Run it

Because the app uses `fetch`-free ES modules-free scripts, it works straight off
the filesystem — but `localStorage` and some browser APIs behave better over
HTTP. Any static server will do:

```bash
# Python
python -m http.server 8000

# Node
npx serve .
```

Then open <http://localhost:8000>.

Opening `index.html` directly with `file://` also works.

---

## The flow

| # | View | Route | What happens |
|---|------|-------|--------------|
| 00 | Home | `#home` | Hero, live network panel, the 4-step ritual, featured bowls |
| 01 | Menu | `#menu` | 13 bowls + 8 add-ons, sector filters, live search (`/` to focus) |
| 02 | Outposts | `#outposts` | 5 invented branches, sortable by wait / distance / stock, network map |
| 03 | Checkout | `#checkout` | Operator ident, pickup window, payment node, live order manifest |
| 04 | Pass | `#pass` | The generated **pickup pass** with number, QR matrix and barcode |

Browsing → picking an outpost → adding to cart → checking out issues a numbered
pickup pass you present at the counter. Print styles are included, so the pass
prints as a clean black-on-white ticket.

Cart, selected outpost and the last pass persist in `localStorage`.

---

## Design system

| Token | Value | Role |
|-------|-------|------|
| `--cyan` | `#00e5ff` | primary signal |
| `--magenta` | `#ff2d95` | secondary / brand kanji |
| `--acid` | `#c6ff00` | price, success, the pass number |
| `--violet` | `#8b5cff` | depth |

Type: **Orbitron** + **Zen Dots** (display), **Chakra Petch** (UI),
**Share Tech Mono** (data), **Noto Sans JP** (Japanese accents).

The "digital noisy decay" comes from four stacked layers — an SVG
`feTurbulence` grain that steps through 6 offsets, CRT scanlines with a rolling
band, a perspective floor grid, and a chromatic vignette. All of it is
disabled automatically under `prefers-reduced-motion`.

---

## Motion & interaction

Built with GSAP + ScrollTrigger and vanilla-tilt, both treated as
**progressive enhancement** — if either CDN fails the site still works.

- Terminal **boot sequence** with typed log, progress bar and skip
- Canvas **particle field** — drifting steam, neon sparks with pointer
  parallax, and falling katakana/hex glyphs
- **Custom crosshair cursor** with contextual labels (`ADD TO BUFFER`, …)
- **3D tilt** + glare on cards; magnetic buttons
- **Text scramble** on hover for headings
- **Synthesised ambient audio** via WebAudio (no asset files) — off by default
- Count-up stats, staggered card entrances, a laser sweep across the pass

---

## Files

```
index.html
imgs/                    6 ramen photos (reused across the menu)
assets/css/main.css      design system + all components (~1670 lines)
assets/js/data.js        all mock data: bowls, add-ons, outposts
assets/js/fx.js          atmosphere: boot, particles, cursor, ticker, audio
assets/js/app.js         router, cart, checkout, pass generator
```

### External dependencies

| Library | Version | URL |
|---------|---------|-----|
| GSAP + ScrollTrigger | 3.15.0 | `cdn.jsdelivr.net/npm/gsap@3.15.0/dist/…` |
| vanilla-tilt | 1.8.1 | `cdn.jsdelivr.net/npm/vanilla-tilt@1.8.1/dist/…` |
| Google Fonts | — | `fonts.googleapis.com/css2?family=Orbitron…` |

---

## Notes

- Every bowl, price, address and the brand itself are **invented**.
- The QR matrix and Code 39 barcode on the pass are generated in-browser from a
  seeded PRNG keyed on the pass number — deterministic, and **not scannable**
  (they exist to look right in the demo).
- The pickup pass counts down 45 minutes from issue, then reads `EXPIRED`.
