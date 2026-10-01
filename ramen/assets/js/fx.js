/* ==========================================================================
   NOODLE-9 // fx.js
   Atmosphere & motion systems. Everything here is progressive enhancement:
   if GSAP / vanilla-tilt fail to load, the site still works — it just gets
   quieter.
   ========================================================================== */
(function () {
  "use strict";

  const D = window.DATA || {};
  const $  = (s, c) => (c || document).querySelector(s);
  const $$ = (s, c) => Array.from((c || document).querySelectorAll(s));
  const rand  = (a, b) => a + Math.random() * (b - a);
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
  const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  window.FX = { rand, clamp, reduceMotion, toast: null };

  /* ══════════════════════════════════════════════════════════════════════
     1 · BOOT SEQUENCE
     ══════════════════════════════════════════════════════════════════════ */
  const Boot = (() => {
    const LINES = [
      ["u", "nooodle-9 bios v2.077.11 — cold start"],
      ["b", "mounting /imgs .................... ", "ok"],
      ["b", "pressure vessel A ................ ", "ok"],
      ["b", "pressure vessel B ................ ", "ok"],
      ["b", "pressure vessel C ................ ", "0x7F · degraded"],
      ["b", "calibrating thermal probes ........ ", "ok"],
      ["b", "loading broth kernel .............. ", "ok"],
      ["i", "torched-tare module offline. continuing without."],
      ["b", "syncing outpost clocks (5) ........ ", "ok"],
      ["b", "assembling pickup-pass generator .. ", "ok"],
      ["", "▚ all reactors nominal. welcome, operator."],
    ];

    function run() {
      const el    = $("#boot");
      const log   = $("#bootLog");
      const bar   = $("#bootBar");
      const pct   = $("#bootPct");
      const skip  = $("#bootSkip");
      if (!el) {
        document.body.classList.remove("is-booting");
        document.body.classList.add("is-ready");
        document.dispatchEvent(new CustomEvent("n9:ready"));
        return;
      }

      // reduced motion → straight to the site
      if (reduceMotion) { finish(); return; }

      let li = 0, ci = 0, cur = "", p = 0, raf = null, bail = null;
      let box = null;               // the line currently being written

      function newLine() {
        const s = document.createElement("span");
        s.innerHTML = "<b>› </b>";
        log.appendChild(s);
        return s;
      }

      function step() {
        if (li >= LINES.length) return finish();
        const [kind, text, note] = LINES[li];

        if (kind === "") {
          box = newLine();
          box.innerHTML = `<b>› </b><b>${esc(text)}</b>`;
          box.style.animation = "revealIn .6s var(--ease) both";
          li++;
          p = 100; bar.style.width = "100%"; pct.textContent = "100";
          return raf = setTimeout(finish, 700);
        }

        // b / i / u lines stream in character by character
        if (ci === 0) { cur = ""; box = newLine(); }
        if (ci < text.length) {
          cur += text[ci++];
          let tail = "";
          if (ci >= text.length && kind === "b") {
            tail = note === "ok" ? `<b>[ ok ]</b>` : `<i>[ ${note} ]</i>`;
          }
          box.innerHTML = `<b>› </b>${esc(cur)}${tail}`;
          p = (li + ci / text.length) / LINES.length * 100;
          bar.style.width = p + "%";
          pct.textContent = String(Math.floor(p)).padStart(2, "0");
          return raf = requestAnimationFrame(step);
        }

        // line complete
        if (kind === "b") {
          box.innerHTML = `<b>› </b>${esc(cur)}${note === "ok" ? `<b>[ ok ]</b>` : `<i>[ ${note} ]</i>`}`;
        }
        li++; ci = 0;
        return raf = setTimeout(step, 55);
      }

      function esc(s) { return s.replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

      function finish() {
        if (raf) { cancelAnimationFrame(raf); clearTimeout(raf); }
        if (bail) clearTimeout(bail);
        el.classList.add("is-done");
        document.body.classList.remove("is-booting");
        document.body.classList.add("is-ready");
        setTimeout(() => el.remove(), 900);
        document.dispatchEvent(new CustomEvent("n9:ready"));
      }

      skip.addEventListener("click", finish);
      // safety net — never trap anyone behind the terminal
      bail = setTimeout(finish, 9000);

      setTimeout(step, 120);
      return { finish };
    }

    return { run };
  })();

  /* ══════════════════════════════════════════════════════════════════════
     2 · PARTICLE FIELD — steam, sparks and falling data glyphs
     ══════════════════════════════════════════════════════════════════════ */
  function Particles() {
    const cv = $("#particles");
    if (!cv) return;
    const cx = cv.getContext("2d", { alpha: true });
    let W = 0, H = 0, dpr = 1, raf = null, t = 0;
    let mx = 0, my = 0, tmx = 0, tmy = 0;

    const PALETTE = ["0,229,255", "255,45,149", "198,255,0", "139,92,255"];
    const GLYPHS = "アイウエオカキクケコサシスセソタチツテトナニヌネノ0123456789ABCDEFGHJKLMNPQRSTUVWXYZ<>/\\[]{}#@$%&*+=";
    let steam = [], sparks = [], glyphs = [];

    function size() {
      dpr = Math.min(window.devicePixelRatio || 1, 2);
      W = cv.clientWidth; H = cv.clientHeight;
      cv.width = Math.floor(W * dpr); cv.height = Math.floor(H * dpr);
      cx.setTransform(dpr, 0, 0, dpr, 0, 0);
      build();
    }

    function build() {
      const density = clamp((W * H) / 26000, 26, 92);
      steam = Array.from({ length: Math.round(density * 0.45) }, () => ({
        x: rand(0, W), y: rand(-H, H),
        r: rand(26, 92), v: rand(0.10, 0.34), a: rand(0.03, 0.1),
        d: rand(0, 7), c: PALETTE[0], rise: 0,
      }));
      sparks = Array.from({ length: Math.round(density) }, () => ({
        x: rand(0, W), y: rand(0, H),
        r: rand(0.6, 1.9), v: rand(0.12, 0.5), a: rand(0.25, 0.85),
        sw: rand(0.3, 1.1), ph: rand(0, 6.3),
        c: PALETTE[(Math.random() * PALETTE.length) | 0],
      }));
      glyphs = Array.from({ length: Math.round(density * 0.16) }, () => ({
        x: rand(0, W), y: rand(-H, H), v: rand(0.22, 0.62),
        ch: GLYPHS[(Math.random() * GLYPHS.length) | 0],
        a: rand(0.06, 0.22), c: PALETTE[(Math.random() * 3) | 0],
      }));
    }

    function frame() {
      t++;
      mx += (tmx - mx) * 0.045;
      my += (tmy - my) * 0.045;

      cx.clearRect(0, 0, W, H);

      // soft steam
      cx.globalCompositeOperation = "lighter";
      for (const s of steam) {
        s.y -= s.v; s.x += Math.sin(t * 0.012 + s.d) * 0.28; s.rise += 0.004;
        if (s.y < -s.r * 2.2) { s.y = H + s.r; s.x = rand(0, W); }
        const g = cx.createRadialGradient(s.x, s.y, 0, s.x, s.y, s.r);
        g.addColorStop(0, `rgba(${s.c},${s.a})`);
        g.addColorStop(1, `rgba(${s.c},0)`);
        cx.fillStyle = g;
        cx.beginPath(); cx.arc(s.x, s.y, s.r, 0, 6.284); cx.fill();
      }

      // neon sparks with parallax
      for (const s of sparks) {
        s.y -= s.v; s.x += Math.sin(t * 0.02 + s.ph) * s.sw;
        if (s.y < -8) { s.y = H + 8; s.x = rand(0, W); }
        const px = (s.x + mx * (18 + s.r * 9)) % (W + 40) - 20;
        const py = s.y + my * 8;
        const tw = 0.55 + 0.45 * Math.sin(t * 0.09 + s.ph * 3);
        cx.fillStyle = `rgba(${s.c},${s.a * tw})`;
        cx.shadowBlur = 9; cx.shadowColor = `rgba(${s.c},.8)`;
        cx.beginPath(); cx.arc(px, py, s.r, 0, 6.284); cx.fill();
        cx.shadowBlur = 0;
      }

      // falling matrix glyphs
      cx.font = '13px "Share Tech Mono", monospace';
      cx.textAlign = "center";
      for (const g of glyphs) {
        g.y += g.v;
        if (g.y > H + 20) {
          g.y = -20; g.x = rand(0, W);
          g.ch = GLYPHS[(Math.random() * GLYPHS.length) | 0];
        }
        cx.fillStyle = `rgba(${g.c},${g.a})`;
        cx.fillText(g.ch, g.x, g.y);
      }

      cx.globalCompositeOperation = "source-over";
      raf = requestAnimationFrame(frame);
    }

    size();
    window.addEventListener("resize", size, { passive: true });
    if (!reduceMotion) {
      window.addEventListener("pointermove", e => {
        tmx = (e.clientX / window.innerWidth - 0.5) * 2;
        tmy = (e.clientY / window.innerHeight - 0.5) * 2;
      }, { passive: true });
      frame();
    }

    // pause when tab is hidden — don't burn CPU in the background
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) { cancelAnimationFrame(raf); raf = null; }
      else if (!reduceMotion && !raf) frame();
    });
  }

  /* ══════════════════════════════════════════════════════════════════════
     3 · CUSTOM CURSOR
     ══════════════════════════════════════════════════════════════════════ */
  function Cursor() {
    if (reduceMotion || !window.matchMedia("(hover:hover) and (pointer:fine)").matches) return;
    const wrap = $("#cursor");
    if (!wrap) return;
    const ring = $(".cursor__ring", wrap);
    const dot  = $(".cursor__dot", wrap);
    const lbl  = $(".cursor__label", wrap);
    let tx = innerWidth / 2, ty = innerHeight / 2, rx = tx, ry = ty, raf;

    addEventListener("pointermove", e => { tx = e.clientX; ty = e.clientY; }, { passive: true });
    addEventListener("pointerdown", () => wrap.style.scale = ".82");
    addEventListener("pointerup",   () => wrap.style.scale = "1");
    wrap.style.transition = "scale .18s var(--ease)";

    (function loop() {
      rx += (tx - rx) * 0.17; ry += (ty - ry) * 0.17;
      ring.style.transform = `translate(${rx}px,${ry}px)`;
      dot.style.left = tx + "px"; dot.style.top = ty + "px";
      raf = requestAnimationFrame(loop);
    })();
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) cancelAnimationFrame(raf);
      else loop();
    });

    // hover states
    const HOT = "a,button,.bowl,.out,.addon,.sector,.mapnode,input,select,label.pay";
    document.addEventListener("pointerover", e => {
      const t = e.target.closest(HOT);
      if (!t) return;
      document.body.classList.add("cursor-hot");
      let label = "";
      if (t.classList.contains("bowl__add") || t.closest(".bowl__add")) label = "ADD TO BUFFER";
      else if (t.classList.contains("out__sel"))              label = "LOCK OUTPOST";
      else if (t.classList.contains("drawer__panel") || t.closest(".drawer__panel")) label = "";
      lbl.textContent = label;
    });
    document.addEventListener("pointerout", e => {
      if (!e.relatedTarget || !e.relatedTarget.closest) return;
      if (e.relatedTarget.closest(HOT)) return;
      document.body.classList.remove("cursor-hot");
    });
  }

  /* ══════════════════════════════════════════════════════════════════════
     4 · JST CLOCK
     ══════════════════════════════════════════════════════════════════════ */
  function Clock() {
    const el = $("#clock");
    if (!el) return;
    const tick = () => {
      const d = new Date();
      const j = new Date(d.getTime() + (9 * 60 + d.getTimezoneOffset()) * 60000);
      el.textContent = j.toTimeString().slice(0, 8);
    };
    tick(); setInterval(tick, 1000);
  }

  /* ══════════════════════════════════════════════════════════════════════
     5 · TICKER
     ══════════════════════════════════════════════════════════════════════ */
  function Ticker() {
    const track = $("#tickerTrack");
    if (!track || !D.TICKER) return;
    const run = () => {
      const seg = D.TICKER.map(s => `<span>${s}</span>`).join("");
      track.innerHTML = seg + seg + seg; // tripled for a seamless wrap
    };
    run();
    if (reduceMotion) return;
    let x = 0, paused = false, raf;
    track.addEventListener("pointerenter", () => paused = true);
    track.addEventListener("pointerleave", () => paused = false);
    (function loop() {
      if (!paused) {
        x -= 0.42;
        if (x <= -track.scrollWidth / 3) x = 0;
        track.style.transform = `translateX(${x}px)`;
      }
      raf = requestAnimationFrame(loop);
    })();
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) cancelAnimationFrame(raf);
      else loop();
    });
  }

  /* ══════════════════════════════════════════════════════════════════════
     6 · MAGNETIC BUTTONS
     ══════════════════════════════════════════════════════════════════════ */
  function Magnetic() {
    if (reduceMotion || !window.matchMedia("(hover:hover) and (pointer:fine)").matches) return;
    $$(".magnetic").forEach(el => {
      const strength = 0.32;
      el.addEventListener("pointermove", e => {
        const r = el.getBoundingClientRect();
        const x = (e.clientX - r.left - r.width / 2) * strength;
        const y = (e.clientY - r.top - r.height / 2) * strength;
        el.style.transform = `translate(${x}px,${y}px)`;
      });
      el.addEventListener("pointerleave", () => { el.style.transform = ""; });
    });
  }
  window.FX.magnetic = Magnetic;

  /* ══════════════════════════════════════════════════════════════════════
     7 · TEXT SCRAMBLE (on hover)
     ══════════════════════════════════════════════════════════════════════ */
  const SCRAM = "!<>-_\\/[]{}—=+*^?#________ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  function scramble(el, text) {
    if (reduceMotion) { el.textContent = text; return; }
    const end = text.length;
    let frame = 0;
    const queue = [];
    for (let i = 0; i < end; i++) {
      queue.push({ from: SCRAM[(Math.random() * SCRAM.length) | 0], to: text[i], start: Math.floor(i * 3), end: Math.floor(i * 3) + 8 });
    }
    const tick = () => {
      let out = "", done = 0;
      for (const q of queue) {
        if (frame >= q.end) { out += q.to; done++; }
        else if (frame >= q.start) out += SCRAM[(Math.random() * SCRAM.length) | 0];
        else out += q.to;
      }
      el.textContent = out;
      if (done === queue.length) { el.textContent = text; return; }
      frame++; requestAnimationFrame(tick);
    };
    tick();
  }
  window.FX.scramble = scramble;

  function initScramble() {
    $$(".scramble").forEach(el => {
      const t = el.textContent.trim();
      el.dataset.text = t;
      el.addEventListener("pointerenter", () => scramble(el, t));
    });
  }

  /* ══════════════════════════════════════════════════════════════════════
     8 · COUNT-UP NUMBERS
     ══════════════════════════════════════════════════════════════════════ */
  function countUp(el) {
    const target = parseFloat(el.dataset.count);
    const dec    = parseInt(el.dataset.dec || "0", 10);
    if (reduceMotion) { el.textContent = target.toFixed(dec); return; }
    const dur = 1500, t0 = performance.now();
    (function step(now) {
      const p = clamp((now - t0) / dur, 0, 1);
      const eased = 1 - Math.pow(1 - p, 3);
      el.textContent = (target * eased).toFixed(dec);
      if (p < 1) requestAnimationFrame(step);
    })(t0);
  }
  function initCounters() {
    $$("[data-count]").forEach(el => {
      el.textContent = "0";
      if (!("IntersectionObserver" in window)) { countUp(el); return; }
      const io = new IntersectionObserver((entries, obs) => {
        entries.forEach(en => { if (en.isIntersecting) { countUp(el); obs.unobserve(el); } });
      }, { threshold: 0.4 });
      io.observe(el);
    });
  }

  /* ══════════════════════════════════════════════════════════════════════
     9 · AMBIENT AUDIO — synthesised, no asset files
     ══════════════════════════════════════════════════════════════════════ */
  const Sound = (() => {
    let ctx = null, master = null, on = false, blipOsc = null;

    function build() {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) return false;
      ctx = new AC();
      master = ctx.createGain();
      master.gain.value = 0;
      master.connect(ctx.destination);

      // low reactor drone: two detuned saws + a sub sine, heavily filtered
      const filter = ctx.createBiquadFilter();
      filter.type = "lowpass"; filter.frequency.value = 320; filter.Q.value = 6;
      filter.connect(master);

      [55, 55.6, 82.5].forEach((f, i) => {
        const o = ctx.createOscillator();
        o.type = i === 2 ? "sine" : "sawtooth";
        o.frequency.value = f;
        const g = ctx.createGain();
        g.gain.value = i === 2 ? 0.16 : 0.05;
        o.connect(g); g.connect(filter); o.start();
      });

      // slow filter sweep for "atmospheric"
      const lfo = ctx.createOscillator();
      const lfoG = ctx.createGain();
      lfo.frequency.value = 0.055; lfoG.gain.value = 150;
      lfo.connect(lfoG); lfoG.connect(filter.frequency); lfo.start();

      // faint pink-ish noise bed
      const len = ctx.sampleRate * 3;
      const buf = ctx.createBuffer(1, len, ctx.sampleRate);
      const ch = buf.getChannelData(0);
      let last = 0;
      for (let i = 0; i < len; i++) {
        const w = Math.random() * 2 - 1;
        last = (last + 0.02 * w) / 1.02;
        ch[i] = last * 3.2;
      }
      const src = ctx.createBufferSource();
      src.buffer = buf; src.loop = true;
      const ng = ctx.createGain(); ng.gain.value = 0.07;
      const nf = ctx.createBiquadFilter(); nf.type = "bandpass"; nf.frequency.value = 900; nf.Q.value = 0.6;
      src.connect(nf); nf.connect(ng); ng.connect(master); src.start();

      return true;
    }

    function toggle() {
      if (!ctx && !build()) return false;
      if (ctx.state === "suspended") ctx.resume();
      on = !on;
      master.gain.cancelScheduledValues(ctx.currentTime);
      master.gain.linearRampToValueAtTime(on ? 0.16 : 0, ctx.currentTime + 0.7);
      return on;
    }

    function blip(freq, dur, type, vol) {
      if (!on || !ctx) return;
      const o = ctx.createOscillator(); o.type = type || "square";
      const g = ctx.createGain();
      o.frequency.setValueAtTime(freq, ctx.currentTime);
      o.frequency.exponentialRampToValueAtTime(freq * 0.55, ctx.currentTime + dur);
      g.gain.setValueAtTime(vol || 0.09, ctx.currentTime);
      g.gain.exponentialRampToValueAtTime(0.0001, ctx.currentTime + dur);
      o.connect(g); g.connect(master);
      o.start(); o.stop(ctx.currentTime + dur + 0.02);
      blipOsc = o;
    }

    return {
      toggle,
      add:  () => blip(760, 0.12, "square", 0.10),
      ok:   () => { blip(520, 0.1, "triangle", 0.09); setTimeout(() => blip(880, 0.16, "triangle", 0.08), 90); },
      bad:  () => blip(180, 0.22, "sawtooth", 0.10),
      pass: () => { [440, 660, 880, 1320].forEach((f, i) => setTimeout(() => blip(f, 0.28, "triangle", 0.08), i * 130)); },
    };
  })();
  window.FX.sound = Sound;

  /* ══════════════════════════════════════════════════════════════════════
     10 · REVEAL ON SCROLL (ScrollTrigger optional, IO fallback)
     ══════════════════════════════════════════════════════════════════════ */
  function initReveal() {
    const items = $$("[data-reveal]");
    if (!items.length) return;
    if (window.gsap && window.ScrollTrigger) {
      gsap.registerPlugin(ScrollTrigger);
      items.forEach(el => {
        gsap.fromTo(el,
          { opacity: 0, y: 34 },
          { opacity: 1, y: 0, duration: 0.85, ease: "power3.out",
            scrollTrigger: { trigger: el, start: "top 88%" } });
      });
      return;
    }
    const io = new IntersectionObserver(es => es.forEach(en => {
      if (en.isIntersecting) { en.target.style.animation = "revealIn .85s var(--ease) both"; io.unobserve(en.target); }
    }), { threshold: 0.15 });
    items.forEach(el => io.observe(el));
  }

  /* ══════════════════════════════════════════════════════════════════════
     11 · HOME NETWORK PANEL
     ══════════════════════════════════════════════════════════════════════ */
  function renderNetList() {
    const ul = $("#netList");
    if (!ul || !D.OUTPOSTS) return;
    const max = Math.max(...D.OUTPOSTS.map(o => o.wait));
    ul.innerHTML = D.OUTPOSTS.map(o => {
      const pct = Math.round((o.wait / max) * 100);
      const cls = o.wait <= 6 ? "ok" : "";
      return `<li>
        <span class="netlist__n">${o.code}</span>
        <span class="netlist__name">${o.name}</span>
        <span class="netlist__wait ${cls}">${o.wait} MIN</span>
        <span class="netlist__bar"><i style="width:${pct}%"></i></span>
      </li>`;
    }).join("");
  }

  /* ══════════════════════════════════════════════════════════════════════
     12 · HERO BOWL PARALLAX
     ══════════════════════════════════════════════════════════════════════ */
  function heroParallax() {
    const bowl = $("#heroBowl");
    if (!bowl) return;
    // steam puffs live in the markup-emptied container
    const steam = $(".hero__steam", bowl);
    if (steam && !steam.children.length) {
      for (let i = 0; i < 5; i++) steam.appendChild(document.createElement("span"));
    }
    if (reduceMotion) return;
    if (!window.matchMedia("(hover:hover) and (pointer:fine)").matches) return;
    let rx = 0, ry = 0, tx = 0, ty = 0, raf;
    addEventListener("pointermove", e => {
      tx = (e.clientX / innerWidth - 0.5) * 2;
      ty = (e.clientY / innerHeight - 0.5) * 2;
    }, { passive: true });
    (function loop() {
      rx += (tx - rx) * 0.05; ry += (ty - ry) * 0.05;
      bowl.style.marginLeft = (-rx * 22) + "px";
      bowl.style.marginTop  = (-ry * 16) + "px";
      bowl.style.rotate = (rx * 3).toFixed(2) + "deg";
      raf = requestAnimationFrame(loop);
    })();
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) cancelAnimationFrame(raf); else loop();
    });
  }

  /* ══════════════════════════════════════════════════════════════════════
     BOOT
     ══════════════════════════════════════════════════════════════════════ */
  function start() {
    Clock();
    Ticker();
    Particles();
    Cursor();
    renderNetList();
    heroParallax();
    initCounters();
    initScramble();
    initReveal();
    document.addEventListener("n9:ready", () => { Magnetic(); });

    // sound button
    const sb = $("#soundBtn");
    if (sb) sb.addEventListener("click", () => {
      const on = Sound.toggle();
      sb.setAttribute("aria-pressed", on ? "true" : "false");
      if (window.FX.toast) window.FX.toast(on ? "AMBIENT LINK ESTABLISHED" : "AMBIENT LINK SEVERED",
        on ? "Reactor drone online — toggle with the speaker icon." : "Silence restored.",
        on ? "ok" : "warn", on ? "◉" : "◌");
    });

    Boot.run();
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start);
  else start();
})();
