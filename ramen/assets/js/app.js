/* ==========================================================================
   NOODLE-9 // app.js
   Router · menu · outposts · cart · checkout · pickup-pass generator
   Front-end only. State lives in memory + localStorage.
   ========================================================================== */
(function () {
  "use strict";

  const D = window.DATA;
  const $  = (s, c) => (c || document).querySelector(s);
  const $$ = (s, c) => Array.from((c || document).querySelectorAll(s));
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));

  const PICKUP_FEE = 150;
  const REBATE     = 0.02;
  const MAX_QTY    = 20;

  /* ── STATE ────────────────────────────────────────────────────────────── */
  const LS_KEY = "noodle9.state.v1";
  let state = {
    cart: [],          // [{ id, qty }]
    outpost: null,     // outpost id
    sector: "all",
    query: "",
    sort: "fast",
    pay: "card",
    pass: null,        // { no, at, expires, name, outpostId, total }
  };

  function save() {
    try {
      localStorage.setItem(LS_KEY, JSON.stringify({
        cart: state.cart, outpost: state.outpost, pay: state.pay, pass: state.pass,
      }));
    } catch (e) { /* private mode — carry on without persistence */ }
  }
  function load() {
    try {
      const raw = localStorage.getItem(LS_KEY);
      if (raw) Object.assign(state, JSON.parse(raw));
    } catch (e) { /* corrupt payload — start clean */ }
  }

  /* ── HELPERS ──────────────────────────────────────────────────────────── */
  const yen = n => "¥" + n.toLocaleString("ja-JP");
  const itemById = id => D.ALL_ITEMS.find(i => i.id === id);
  const outpostById = id => D.OUTPOSTS.find(o => o.id === id);
  const cartCount = () => state.cart.reduce((n, l) => n + l.qty, 0);
  const subtotal = () => state.cart.reduce((n, l) => {
    const it = itemById(l.id); return it ? n + it.price * l.qty : n;
  }, 0);
  const rebate  = () => state.pay === "crypto" ? Math.round(subtotal() * REBATE) : 0;
  const total   = () => subtotal() + PICKUP_FEE - rebate();

  const esc = s => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
    .replace(/>/g, "&gt;").replace(/"/g, "&quot;");

  /* deterministic PRNG so a pass number always renders the same QR/barcode */
  function seeded(seedStr) {
    let h = 2166136261;
    for (let i = 0; i < seedStr.length; i++) {
      h ^= seedStr.charCodeAt(i);
      h = Math.imul(h, 16777619);
    }
    return function () {
      h += 0x6D2B79F5;
      let t = h;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  /* ══════════════════════════════════════════════════════════════════════
     TOASTS
     ══════════════════════════════════════════════════════════════════════ */
  const ICONS = { ok: "◉", hot: "⚡", warn: "!", info: "▚" };
  function toast(title, sub, kind, icon) {
    const host = $("#toasts");
    if (!host) return;
    const el = document.createElement("div");
    el.className = "toast toast--" + (kind || "info");
    el.innerHTML =
      `<span class="toast__ico">${icon || ICONS[kind] || ICONS.info}</span>` +
      `<span><span class="toast__t">${esc(title)}</span>` +
      (sub ? `<span class="toast__s">${esc(sub)}</span>` : "") + `</span>`;
    host.appendChild(el);
    setTimeout(() => {
      el.classList.add("is-out");
      setTimeout(() => el.remove(), 400);
    }, 3000);
  }
  window.FX && (window.FX.toast = toast);

  /* ══════════════════════════════════════════════════════════════════════
     ROUTER
     ══════════════════════════════════════════════════════════════════════ */
  const VIEWS = ["home", "menu", "outposts", "checkout", "pass"];

  function route() {
    let h = (location.hash || "#home").replace("#", "").split("?")[0];
    if (!VIEWS.includes(h)) h = "home";

    // guard rails run BEFORE the view swap so a redirect never flashes
    if (h === "checkout" && !state.cart.length) {
      toast("BUFFER EMPTY", "Add a bowl before trying to check out.", "warn");
      return go("menu");
    }
    if (h === "checkout" && !state.outpost) {
      toast("NO OUTPOST LOCKED", "Choose where you will collect this order.", "warn");
      return go("outposts");
    }
    if (h === "pass" && !state.pass) return go("home");

    VIEWS.forEach(v => { const el = $("#view-" + v); if (el) el.hidden = v !== h; });
    $$(".nav__link").forEach(a => a.classList.toggle("is-on", a.dataset.nav === h));

    if (h === "menu")     renderMenu();
    if (h === "outposts") renderOutposts();
    if (h === "checkout") renderCheckout();
    if (h === "pass")     renderPass();

    window.scrollTo({ top: 0, behavior: h === "home" ? "smooth" : "auto" });
  }

  function go(h) {
    if (location.hash === "#" + h) route();
    else location.hash = "#" + h;
  }

  /* ══════════════════════════════════════════════════════════════════════
     3D TILT (uses vanilla-tilt if it loaded)
     ══════════════════════════════════════════════════════════════════════ */
  function tiltIn(root) {
    if (!window.VanillaTilt) return;
    $$("[data-tilt]", root || document).forEach(el => {
      if (el.__tilted) return;
      el.__tilted = true;
      VanillaTilt.init(el, {
        max: 7, speed: 0.6, perspective: 900, gyroscope: false,
        glare: true, "max-glare": 0.22, "glare-color": "#00e5ff",
        "transition": true, scale: 1.012,
      });
    });
  }

  /* ══════════════════════════════════════════════════════════════════════
     MENU
     ══════════════════════════════════════════════════════════════════════ */
  function renderSectors() {
    const host = $("#sectorTabs");
    if (!host) return;
    host.innerHTML = D.SECTORS.map(s => `
      <button class="sector ${s.id === state.sector ? "is-on" : ""}" data-sector="${s.id}" type="button">
        <span>${s.jp}</span>${s.label}<i>${s.code}</i>
      </button>`).join("");
    $$(".sector", host).forEach(b =>
      b.addEventListener("click", () => {
        state.sector = b.dataset.sector;
        renderSectors(); renderMenu();
        if (window.FX && FX.sound) FX.sound.add();
      }));
  }

  function heatBar(heat) {
    let out = "";
    for (let i = 0; i < 10; i++) {
      const on = i < heat;
      out += `<i class="${on ? "on" : ""} ${heat >= 8 && on ? "chi" : ""}"></i>`;
    }
    return `<span class="heatbar" title="thermal ${heat}/10">${out}</span>`;
  }

  function bowlCard(it, i) {
    const low  = it.stock <= 5;
    const out  = it.stock <= 0;
    return `
    <article class="bowl" data-tilt style="--i:${i || 0}">
      <div class="bowl__media">
        <img class="bowl__img" src="${it.img}" alt="${esc(it.name)}" loading="lazy" decoding="async" />
        <span class="bowl__sku">${it.sku}</span>
        <span class="bowl__stock ${out ? "out" : low ? "low" : ""}">${out ? "SOLD OUT" : it.stock + " LEFT"}</span>
        <span class="bowl__kanji">${esc(it.kanji)}</span>
        <div class="bowl__heat">${heatBar(it.heat)}<span>${it.heat}/10 HEAT</span></div>
      </div>
      <div class="bowl__body">
        <h3 class="bowl__name scramble">${esc(it.name)}</h3>
        <div class="bowl__tags">${it.tags.map(t => `<span>${esc(t)}</span>`).join("")}</div>
        <p class="bowl__desc">${esc(it.desc)}</p>
        <div class="bowl__specs">
          <span><b>${it.kcal}</b> KCAL</span>
          <span><b>${it.mins}</b> MIN</span>
          <span>HEAT <b class="${it.heat >= 8 ? "low" : ""}">${it.heat}/10</b></span>
        </div>
      </div>
      <div class="bowl__foot">
        <span class="bowl__price">${yen(it.price)}<small> JPY</small></span>
        <button class="bowl__add" data-add="${it.id}" ${out ? "disabled" : ""} type="button">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 5v14M5 12h14"/></svg>
          <span class="addn">${out ? "UNAVAILABLE" : "ADD"}</span>
        </button>
      </div>
    </article>`;
  }

  function renderMenu() {
    const grid = $("#menuGrid");
    const empty = $("#menuEmpty");
    if (!grid) return;

    const q = state.query.trim().toLowerCase();
    let list = state.sector === "extras" ? [] : D.MENU;
    if (state.sector !== "all" && state.sector !== "extras") {
      list = D.MENU.filter(m => m.cat === state.sector);
    }
    if (q) {
      list = list.filter(m =>
        (m.name + " " + m.kanji + " " + m.desc + " " + m.tags.join(" ") + " " + m.sku)
          .toLowerCase().includes(q));
    }

    grid.innerHTML = list.map((m, i) => bowlCard(m, i)).join("");
    empty.hidden = list.length > 0;
    grid.hidden = list.length === 0;

    const addonSection = $("#addonSection");
    if (addonSection) addonSection.hidden = state.sector === "signature" || state.sector === "heat" || state.sector === "clear";

    $$("[data-add]", grid).forEach(b =>
      b.addEventListener("click", e => { e.stopPropagation(); addToCart(b.dataset.add); }));

    tiltIn(grid);
    if (window.FX && FX.scramble) {
      $$(".scramble", grid).forEach(el => {
        const t = el.textContent.trim(); el.dataset.text = t;
        el.onpointerenter = () => FX.scramble(el, t);
      });
    }
  }

  function renderAddons() {
    const row = $("#addonRow");
    if (!row) return;
    row.innerHTML = D.EXTRAS.map(x => `
      <div class="addon" data-tilt>
        <span class="addon__jp">${esc(x.kanji.trim())}</span>
        <div class="addon__t">
          <div class="addon__name">${esc(x.name)}</div>
          <div class="addon__desc">${esc(x.desc)}</div>
        </div>
        <div class="addon__r">
          <span class="addon__p">${yen(x.price)}</span>
          <button class="addon__x" data-add="${x.id}" type="button" aria-label="Add ${esc(x.name)}">+</button>
        </div>
      </div>`).join("");
    $$("[data-add]", row).forEach(b =>
      b.addEventListener("click", () => addToCart(b.dataset.add)));
    tiltIn(row);
  }

  /* ══════════════════════════════════════════════════════════════════════
     OUTPOSTS
     ══════════════════════════════════════════════════════════════════════ */
  const ACCENT = { cyan: "var(--cyan)", magenta: "var(--magenta)", acid: "var(--acid)", violet: "var(--violet)" };

  function sortedOutposts() {
    const l = D.OUTPOSTS.slice();
    if (state.sort === "fast")  l.sort((a, b) => a.wait - b.wait);
    if (state.sort === "near")  l.sort((a, b) => a.walk - b.walk);
    if (state.sort === "stock") l.sort((a, b) => b.stock - a.stock);
    return l;
  }

  function renderMapNodes() {
    const host = $("#mapNodes");
    if (!host) return;
    // spread nodes across the canvas so they never collide
    const POS = [[14, 71], [32, 55], [52, 63], [72, 38], [88, 47]];
    const list = sortedOutposts();
    host.innerHTML = list.map((o, i) => {
      const p = POS[i % POS.length];
      return `<button class="mapnode ${o.id === state.outpost ? "is-sel" : ""}" data-node="${o.id}"
        style="left:${p[0]}%;top:${p[1]}%;--nc:${ACCENT[o.accent]}" type="button">
        <span class="mapnode__pulse"></span>
        <span class="mapnode__dot"></span>
        <span class="mapnode__lbl">${o.code} ${o.name}</span>
      </button>`;
    }).join("");
    $$(".mapnode", host).forEach(b =>
      b.addEventListener("click", () => selectOutpost(b.dataset.node, true)));
  }

  function outCard(o, i) {
    const sel  = o.id === state.outpost;
    const acc  = ACCENT[o.accent];
    const waitCls = o.wait > 12 ? "warn" : "";
    const stockPct = clamp(Math.round((o.stock / 60) * 100), 3, 100);
    return `
    <article class="out ${sel ? "is-sel" : ""}" data-tilt style="--oc:${acc};--i:${i || 0}">
      <div class="out__top">
        <span class="out__code">${o.code}</span>
        <div class="out__id">
          <h3 class="out__name scramble">${esc(o.name)}</h3>
          <span class="out__jp">${esc(o.jp)} · ${esc(o.area)}</span>
        </div>
        <span class="out__hours">${esc(o.hours)}</span>
      </div>
      <div class="out__body">
        <p class="out__addr">${esc(o.address)}</p>
        <p class="out__addrjp">${esc(o.addressJp)}</p>
        <p class="out__note">▸ ${esc(o.note)}</p>
        <div class="out__meters">
          <div class="meter"><b class="${waitCls}">${o.wait} MIN</b><span>EST. WAIT</span></div>
          <div class="meter"><b>${o.walk} MIN</b><span>DISTANCE</span></div>
          <div class="meter"><b>${o.seats}</b><span>${esc(o.deck)}</span></div>
        </div>
        <div class="out__stockbar">
          <div><span>UNIT STOCK</span><span>${o.stock} BOWLS</span></div>
          <div class="out__stocktrack"><i style="width:${stockPct}%"></i></div>
        </div>
      </div>
      <div class="out__foot">
        <button class="out__sel" data-out="${o.id}" type="button">
          ${sel ? "✓ LOCKED — <b>CHANGE</b>" : "LOCK THIS OUTPOST"}
        </button>
      </div>
    </article>`;
  }

  function renderOutposts() {
    const grid = $("#outpostGrid");
    if (!grid) return;
    grid.innerHTML = sortedOutposts().map((o, i) => outCard(o, i)).join("");
    $$("[data-out]", grid).forEach(b =>
      b.addEventListener("click", () => selectOutpost(b.dataset.out, true)));
    renderLockedBar();
    renderMapNodes();
    tiltIn(grid);
  }

  /* once an outpost is locked, offer the next step right here */
  function renderLockedBar() {
    const bar = $("#lockedBar");
    if (!bar) return;
    const o = outpostById(state.outpost);
    if (!o) { bar.hidden = true; return; }
    const ready = state.cart.length > 0;
    bar.hidden = false;
    bar.innerHTML = `
      <span class="lockedbar__pulse"></span>
      <span class="lockedbar__t">
        <b>OUTPOST ${o.code} LOCKED · ${esc(o.name)}</b>
        <small>${esc(o.addressJp)} — ${o.wait} MIN WAIT · ${esc(o.hours)}</small>
      </span>
      ${ready
        ? `<a class="btn btn--primary lockedbar__cta" href="#checkout" data-link>
             <span class="btn__label">PROCEED TO CHECKOUT</span><span class="btn__arrow">→</span>
           </a>`
        : `<a class="btn btn--ghost lockedbar__cta" href="#menu" data-link>
             <span class="btn__label">PICK A BOWL FIRST</span><span class="btn__arrow">→</span>
           </a>`}`;
  }

  function selectOutpost(id, announce) {
    const o = outpostById(id);
    if (!o) return;
    state.outpost = state.outpost === id ? null : id;
    save();
    renderOutposts();
    renderCheckoutSummary();
    renderDrawerOutpost();
    if (announce && state.outpost) {
      toast("OUTPOST LOCKED", o.name + " · " + o.wait + " min wait · " + o.hours, "ok", "◉");
      if (window.FX && FX.sound) FX.sound.ok();
    } else if (announce) {
      toast("OUTPOST RELEASED", "You can still change this before you burn the pass.", "warn", "◌");
    }
  }

  /* ══════════════════════════════════════════════════════════════════════
     CART
     ══════════════════════════════════════════════════════════════════════ */
  function addToCart(id) {
    const it = itemById(id);
    if (!it) return;
    const line = state.cart.find(l => l.id === id);
    if (!line) {
      state.cart.push({ id, qty: 1 });
    } else if (line.qty < MAX_QTY) {
      line.qty++;
    } else {
      toast("BUFFER LIMIT", "Max " + MAX_QTY + " of that item. This is a demo.", "warn");
      return;
    }
    save(); syncAll();
    toast("ADDED TO BUFFER", it.name + " ×" + (line ? line.qty : 1), "ok", "◈");
    if (window.FX && FX.sound) FX.sound.add();
    bumpCart();
  }

  function setQty(id, delta) {
    const line = state.cart.find(l => l.id === id);
    if (!line) return;
    line.qty += delta;
    if (line.qty <= 0) state.cart = state.cart.filter(l => l.id !== id);
    save(); syncAll();
  }

  function clearCart() {
    state.cart = [];
    state.pass = null;
    save(); syncAll();
    toast("BUFFER PURGED", "Cart emptied. The reactors are disappointed.", "warn", "◌");
  }

  function bumpCart() {
    const btn = $("#cartBtn");
    if (!btn) return;
    btn.classList.add("is-bump");
    setTimeout(() => btn.classList.remove("is-bump"), 320);
  }

  /* ── drawer ───────────────────────────────────────────────────────────── */
  function openDrawer() {
    const d = $("#drawer");
    if (!d) return;
    d.classList.add("is-open");
    d.setAttribute("aria-hidden", "false");
    document.body.classList.add("no-scroll");
  }
  function closeDrawer() {
    const d = $("#drawer");
    if (!d) return;
    d.classList.remove("is-open");
    d.setAttribute("aria-hidden", "true");
    document.body.classList.remove("no-scroll");
  }

  function renderDrawerOutpost() {
    const el = $("#drawerOutpost");
    if (!el) return;
    const o = outpostById(state.outpost);
    el.innerHTML = o
      ? `<div class="summary__ok">
           <span class="panel__dot" style="background:${ACCENT[o.accent]};box-shadow:0 0 8px ${ACCENT[o.accent]}"></span>
           <span><b>OUTPOST ${o.code} · ${esc(o.name)}</b><small>${esc(o.area)} — ${o.wait} MIN</small></span>
           <a class="summary__chg" href="#outposts" data-link>CHANGE</a>
         </div>`
      : `<span class="summary__need">▸ NO OUTPOST LOCKED — <a href="#outposts" data-link style="color:var(--cyan)">CHOOSE ONE</a></span>`;
  }

  function renderDrawer() {
    const body = $("#drawerBody");
    if (!body) return;
    if (!state.cart.length) {
      body.innerHTML = `<div class="drawer__empty">
        <span>空</span>
        <p>THE BUFFER IS EMPTY.<br>NOTHING IS QUEUED. NOBODY IS WAITING.</p>
        <a class="btn btn--primary" href="#menu" data-link><span class="btn__label">FEED ME</span><span class="btn__arrow">→</span></a>
      </div>`;
    } else {
      body.innerHTML = state.cart.map(l => {
        const it = itemById(l.id);
        if (!it) return "";
        return `<div class="citem">
          <img class="citem__img" src="${it.img}" alt="" loading="lazy" />
          <div class="citem__t">
            <div class="citem__n">${esc(it.name)}</div>
            <div class="citem__m">${it.sku} · ${yen(it.price)} ea</div>
          </div>
          <div class="citem__r">
            <span class="citem__p">${yen(it.price * l.qty)}</span>
            <div class="citem__q">
              <button data-dec="${it.id}" type="button" aria-label="Remove one">−</button>
              <span>${l.qty}</span>
              <button data-inc="${it.id}" type="button" aria-label="Add one">+</button>
            </div>
          </div>
        </div>`;
      }).join("");
      $$("[data-inc]", body).forEach(b => b.addEventListener("click", () => setQty(b.dataset.inc, 1)));
      $$("[data-dec]", body).forEach(b => b.addEventListener("click", () => setQty(b.dataset.dec, -1)));
    }
    const t = $("#drawerTotal");
    if (t) t.textContent = yen(subtotal());
    const co = $("#drawerCheckout");
    if (co) {
      const o = outpostById(state.outpost);
      co.disabled = !state.cart.length;
      co.title = o ? "" : "Lock an outpost first";
    }
    renderDrawerOutpost();
  }

  /* ══════════════════════════════════════════════════════════════════════
     CHECKOUT
     ══════════════════════════════════════════════════════════════════════ */
  function buildSlots() {
    const sel = $("#fSlot");
    if (!sel) return;
    const o = outpostById(state.outpost);
    const lead = o ? o.wait : 8;
    const now = new Date();
    const opts = [];
    for (let i = 0; i < 6; i++) {
      const t = new Date(now.getTime() + (lead + i * 15) * 60000);
      const hh = String(t.getHours()).padStart(2, "0");
      const mm = String(t.getMinutes()).padStart(2, "0");
      const tag = i === 0 ? "ASAP" : i === 1 ? "+15 · RECOMMENDED" : "+" + (i * 15) + " MIN";
      opts.push(`<option value="${hh}:${mm}">${tag} — ${hh}:${mm}</option>`);
    }
    sel.innerHTML = opts.join("");
    sel.selectedIndex = 1;
  }

  function renderCheckoutSummary() {
    const lines = $("#sumLines");
    if (!lines) return;

    if (!state.cart.length) {
      lines.innerHTML = `<div class="drawer__empty" style="padding:34px 20px">
        <span style="font-size:34px">空</span>
        <p style="font-size:10.5px">NOTHING QUEUED YET.</p></div>`;
    } else {
      lines.innerHTML = state.cart.map(l => {
        const it = itemById(l.id); if (!it) return "";
        return `<div class="sline">
          <span class="sline__n">${l.qty}</span>
          <span class="sline__t">
            <span class="sline__name">${esc(it.name)}</span>
            <span class="sline__meta">${it.sku} · ${yen(it.price)} ea</span>
          </span>
          <span class="sline__r">
            <span class="sline__q">
              <button data-dec="${it.id}" type="button" aria-label="Remove one">−</button>
              <span>${l.qty}</span>
              <button data-inc="${it.id}" type="button" aria-label="Add one">+</button>
            </span>
            <span class="sline__p">${yen(it.price * l.qty)}</span>
          </span>
        </div>`;
      }).join("");
      $$("[data-inc]", lines).forEach(b => b.addEventListener("click", () => setQty(b.dataset.inc, 1)));
      $$("[data-dec]", lines).forEach(b => b.addEventListener("click", () => setQty(b.dataset.dec, -1)));
    }

    const o = outpostById(state.outpost);
    const box = $("#sumOutpost");
    if (box) {
      box.innerHTML = o
        ? `<div class="summary__ok">
             <span class="panel__dot" style="background:${ACCENT[o.accent]};box-shadow:0 0 8px ${ACCENT[o.accent]}"></span>
             <span><b>OUTPOST ${o.code} · ${esc(o.name)}</b><small>${esc(o.area)} — ${esc(o.hours)}</small></span>
             <a class="summary__chg" href="#outposts" data-link>CHANGE</a>
           </div>`
        : `<a href="#outposts" data-link class="summary__need">▸ LOCK AN OUTPOST TO CONTINUE</a>`;
    }

    $("#sumCount").textContent = cartCount() + (cartCount() === 1 ? " ITEM" : " ITEMS");
    $("#tSub").textContent    = yen(subtotal());
    $("#tFee").textContent    = yen(PICKUP_FEE);
    $("#tRebate").textContent = "−" + yen(rebate());
    const gt = $("#tTotal");
    if (gt) { gt.textContent = yen(total()); gt.dataset.text = yen(total()); }
    const burn = $("#burnBtn");
    if (burn) {
      const ready = state.cart.length && o;
      burn.disabled = !ready;
      burn.title = ready ? "" : (state.cart.length ? "Lock an outpost first" : "Cart is empty");
    }
  }

  function renderCheckout() {
    renderCheckoutSummary();
    buildSlots();
  }

  /* ── validation ───────────────────────────────────────────────────────── */
  const RULES = {
    name:  v => v.trim().length >= 2 || "Enter at least 2 characters.",
    email: v => /^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/.test(v.trim()) || "That uplink address is malformed.",
    phone: v => v.replace(/[^\d]/g, "").length >= 8 || "At least 8 digits, please.",
    slot:  v => !!v || "Pick a pickup window.",
  };

  function validate() {
    const form = $("#checkoutForm");
    if (!form) return null;
    const data = {};
    let ok = true;
    let firstBad = null;
    ["name", "email", "phone", "slot"].forEach(k => {
      const input = form.elements[k];
      if (!input) return;
      const res = RULES[k](input.value || "");
      const wrap = input.closest(".field");
      const errEl = $(`[data-err="${k}"]`);
      const msg = res === true ? "" : res;
      if (msg) {
        ok = false;
        if (wrap) wrap.classList.add("is-bad");
        if (errEl) errEl.textContent = "▸ " + msg;
        if (!firstBad) firstBad = input;
      } else {
        if (wrap) wrap.classList.remove("is-bad");
        if (errEl) errEl.textContent = "";
      }
      data[k] = input.value;
    });
    if (firstBad) firstBad.focus();
    if (!ok && window.FX && FX.sound) FX.sound.bad();
    return ok ? data : null;
  }

  function clearErrorOnInput() {
    $$("#checkoutForm .field").forEach(f => {
      f.classList.remove("is-bad");
      const e = $(".field__err", f); if (e) e.textContent = "";
    });
  }

  /* ══════════════════════════════════════════════════════════════════════
     PICKUP PASS
     ══════════════════════════════════════════════════════════════════════ */
  function makePassNo() {
    const d = new Date();
    const stamp = String(d.getFullYear()).slice(2) + String(d.getMonth() + 1).padStart(2, "0");
    const r = Math.floor(Math.random() * 1e8).toString().padStart(8, "0");
    return `N9-${stamp}-${r.slice(0, 4)}-${r.slice(4)}`;
  }

  /* Pseudo-QR: seeded module matrix with real finder + timing patterns. */
  function qrSvg(seed) {
    const N = 25;
    const rnd = seeded(seed);
    const g = Array.from({ length: N }, () => Array(N).fill(0));
    // data region
    for (let y = 0; y < N; y++) {
      for (let x = 0; x < N; x++) g[y][x] = rnd() > 0.52 ? 1 : 0;
    }
    // finder patterns (7x7) at 3 corners
    const finder = (ox, oy) => {
      for (let y = 0; y < 7; y++) {
        for (let x = 0; x < 7; x++) {
          const edge = x === 0 || y === 0 || x === 6 || y === 6;
          const core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
          g[oy + y][ox + x] = edge || core ? 1 : 0;
        }
      }
      // separator
      for (let k = -1; k <= 7; k++) {
        if (oy - 1 >= 0 && ox + k >= 0 && ox + k < N) g[oy - 1][ox + k] = 0;
        if (oy + 7 < N && ox + k >= 0 && ox + k < N) g[oy + 7][ox + k] = 0;
        if (ox - 1 >= 0 && oy + k >= 0 && oy + k < N) g[oy + k][ox - 1] = 0;
        if (ox + 7 < N && oy + k >= 0 && oy + k < N) g[oy + k][ox + 7] = 0;
      }
    };
    finder(0, 0); finder(N - 7, 0); finder(0, N - 7);
    // timing patterns
    for (let i = 8; i < N - 8; i++) { g[6][i] = i % 2 === 0 ? 1 : 0; g[i][6] = i % 2 === 0 ? 1 : 0; }
    // alignment block
    for (let y = 0; y < 5; y++) {
      for (let x = 0; x < 5; x++) {
        const e = x === 0 || y === 0 || x === 4 || y === 4;
        const c = x === 2 && y === 2;
        g[N - 9 + y][N - 9 + x] = e || c ? 1 : 0;
      }
    }
    // merge into a single path
    let d = "";
    for (let y = 0; y < N; y++) {
      for (let x = 0; x < N; x++) {
        if (g[y][x]) d += `M${x} ${y}h1v1h-1z`;
      }
    }
    return `<svg viewBox="0 0 ${N} ${N}" width="100%" role="img" aria-label="Pass matrix code">
      <rect width="${N}" height="${N}" fill="#eafcff"/>
      <path d="${d}" fill="#04060b"/>
    </svg>`;
  }

  /* Code 39 — real character table, narrow=1 wide=3. */
  const C39 = {
    "0":"000110100","1":"100100001","2":"001100001","3":"101100000","4":"000110001",
    "5":"100110000","6":"001110000","7":"000100101","8":"100100100","9":"001100100",
    "A":"100001001","B":"001001001","C":"101001000","D":"000011001","E":"100011000",
    "F":"001011000","G":"000001101","H":"100001100","I":"001001100","J":"000011100",
    "K":"100000101","L":"001000101","M":"101000100","N":"000010101","O":"100010100",
    "P":"001010100","Q":"000000111","R":"100000110","S":"001000110","T":"000010110",
    "U":"110000001","V":"011000001","W":"010100000","X":"110010000","Y":"011010000",
    "Z":"011000100","-":"010000100","*":"010001000",
  };

  function barcodeSvg(text) {
    const clean = (text.match(/[0-9A-Z\-*]/g) || []).join("") || "*N9*";
    const s = "*" + clean + "*";
    const unit = 1, wide = 3;
    const GAP = 1;
    const bars = [];
    let x = 0;
    for (const ch of s) {
      const pat = C39[ch] || C39["-"];
      for (let i = 0; i < 9; i++) {
        const w = pat[i] === "1" ? wide * unit : unit;
        if (i % 2 === 0) bars.push({ x, w });
        x += w;
      }
      x += GAP * unit;
    }
    const W = x;
    const rects = bars.map(b => `<rect x="${b.x}" y="0" width="${b.w}" height="100"/>`).join("");
    return `<svg viewBox="0 0 ${W} 100" preserveAspectRatio="none"
      role="img" aria-label="Pass barcode ${esc(clean)}"><g fill="#eafcff">${rects}</g></svg>`;
  }

  function burnPass(data) {
    const no   = makePassNo();
    const now  = new Date();
    state.pass = {
      no,
      at: now.toISOString(),
      expires: now.getTime() + 45 * 60000,   // 45 min to collect
      name: data.name.trim().toUpperCase(),
      email: data.email.trim(),
      phone: data.phone.trim(),
      slot: data.slot,
      pay: state.pay,
      outpostId: state.outpost,
      items: state.cart.map(l => ({ id: l.id, qty: l.qty })),
      total: total(),
    };
    save();
    if (window.FX && FX.sound) FX.sound.pass();
    go("pass");
  }

  function renderPass() {
    const host = $("#passHost");
    if (!host || !state.pass) return;
    const p = state.pass;
    const o = outpostById(p.outpostId);
    const at = new Date(p.at);

    const payLabel = p.pay === "card" ? "NEON CARD · INSTANT" : p.pay === "crypto" ? "CHAIN CREDIT · 2% REBATE" : "PAY AT COUNTER";
    const slotH = p.slot.split(":")[0], slotM = p.slot.split(":")[1];

    const digits = p.no.split("").map((c, i) =>
      `<span style="animation-delay:${0.35 + i * 0.055}s">${esc(c)}</span>`).join("");

    host.innerHTML = `
      <div class="pass" data-tilt>
        <div class="pass__corners"><i></i><i></i><i></i><i></i></div>
        <div class="pass__stamp">BURNED</div>

        <div class="pass__head">
          <span class="pass__brand">
            <svg viewBox="0 0 48 48" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2.2">
              <path d="M6 26h36a18 18 0 0 1-36 0Z"/><path d="M12 20h24"/>
            </svg>
            <b>NOODLE-9</b><i>第九麺場</i>
          </span>
          <span class="pass__state">AWAITING PICKUP</span>
          <span class="pass__kinds">ORDER PICKUP PASS</span>
        </div>

        <div class="pass__no">
          <div class="pass__nolbl">PICKUP PASS NUMBER</div>
          <div class="pass__num"><span class="pass__digits">${digits}</span></div>
          <div class="pass__sub">SHOW THIS NUMBER AT THE COUNTER · <b>${esc(payLabel)}</b></div>
        </div>

        <div class="pass__grid">
          <div class="pass__info">
            <div class="pass__row"><span class="pass__k">OPERATOR</span><span class="pass__v"><b>${esc(p.name)}</b></span></div>
            <div class="pass__row"><span class="pass__k">OUTPOST</span><span class="pass__v">${o ? esc(o.code + " · " + o.name) : "—"}<br><span class="pass__v jp">${o ? esc(o.addressJp) : ""}</span></span></div>
            <div class="pass__row"><span class="pass__k">PICKUP WINDOW</span><span class="pass__v mono">${esc(p.slot)}<br><span style="color:var(--text-mute)">${at.getDate()}/${at.getMonth() + 1}/${at.getFullYear()} — ${o ? esc(o.hours) : ""}</span></span></div>
            <div class="pass__row"><span class="pass__k">TABLES IN</span><span class="pass__v mono" id="countdown">—</span></div>
          </div>
          <div class="pass__codes">
            <div class="qrbox">${qrSvg(p.no)}<i>SCAN AT KIOSK</i></div>
            <div>
              ${barcodeSvg(p.no.replace(/-/g, ""))}
              <div class="barcode-lbl">${esc(p.no)}</div>
            </div>
          </div>
        </div>

        <div class="pass__items">
          <div class="pass__itemshead">MANIFEST · ${p.items.reduce((n, i) => n + i.qty, 0)} UNIT(S)</div>
          ${p.items.map(l => {
            const it = itemById(l.id); if (!it) return "";
            return `<div class="pass__item">
              <b>×${l.qty}</b>
              <span>${esc(it.name)} <em>· ${it.sku}</em></span>
              <em>${yen(it.price)} ea</em>
              <i>${yen(it.price * l.qty)}</i>
            </div>`;
          }).join("")}
          <div class="pass__item" style="border-top:1px solid rgba(198,255,0,.28);margin-top:6px;padding-top:10px">
            <b></b><span style="color:var(--acid);font-weight:600">TOTAL PAID AT PICKUP</span><em></em>
            <i style="font-size:16px">${yen(p.total)}</i>
          </div>
        </div>

        <div class="pass__foot">
          <span>PASS ${esc(p.no)}</span>
          <span>·</span>
          <span>THIS PASS IS VOID IF ALTERED</span>
          <span style="margin-left:auto">AWAIT ${slotH}:${slotM} · NOODLE-9 © 2077</span>
        </div>
      </div>

      <div class="pass__actions">
        <button class="btn btn--primary magnetic" id="passPrint" type="button">
          <span class="btn__label">PRINT / SAVE PASS</span><span class="btn__arrow">⎙</span>
        </button>
        <button class="btn btn--ghost magnetic" id="passAgain" type="button">
          <span class="btn__label">ORDER AGAIN</span><span class="btn__arrow">↻</span>
        </button>
        <a class="btn btn--ghost" href="#outposts" data-link><span class="btn__label">ALL OUTPOSTS</span></a>
      </div>`;

    const printBtn = $("#passPrint");
    if (printBtn) printBtn.addEventListener("click", () => window.print());

    const again = $("#passAgain");
    if (again) again.addEventListener("click", () => {
      state.cart = []; state.pass = null; save(); syncAll();
      toast("NEW BUFFER", "Order wiped. Start a fresh run.", "info");
      go("menu");
    });

    tiltIn(host);
    startCountdown(p.expires);
    if (window.FX && FX.magnetic) FX.magnetic();

    if (window.FX && FX.scramble) {
      $$(".scramble", host).forEach(el => {
        const t = el.textContent.trim(); el.dataset.text = t;
        el.onpointerenter = () => FX.scramble(el, t);
      });
    }
  }

  let cdTimer = null;
  function startCountdown(until) {
    const el = $("#countdown");
    if (!el) return;
    clearInterval(cdTimer);
    const tickFn = () => {
      if (!el.isConnected) { clearInterval(cdTimer); return; }
      const ms = until - Date.now();
      if (ms <= 0) { el.innerHTML = '<b style="color:var(--hot)">EXPIRED — ASK STAFF</b>'; clearInterval(cdTimer); return; }
      const m = Math.floor(ms / 60000), s = Math.floor((ms % 60000) / 1000);
      el.textContent = `${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")} REMAINING`;
    };
    tickFn();
    cdTimer = setInterval(tickFn, 1000);
  }

  /* ══════════════════════════════════════════════════════════════════════
     SYNC — one place that refreshes every cart-dependent surface
     ══════════════════════════════════════════════════════════════════════ */
  function syncAll() {
    const n = cartCount();
    const btn = $("#cartBtn"), cnt = $("#cartCount");
    if (cnt) cnt.textContent = n;
    if (btn) btn.classList.toggle("is-empty", n === 0);
    renderDrawer();
    renderCheckoutSummary();
    // patch the visible menu in place rather than re-rendering (keeps tilt + scroll)
    syncMenuButtons();
  }

  /* reflect cart contents on the add buttons without a full re-render */
  function syncMenuButtons() {
    $$(".bowl__add").forEach(b => {
      const line = state.cart.find(l => l.id === b.dataset.add);
      const label = $(".addn", b);
      if (label) label.textContent = line ? `ADD ×${line.qty}` : "ADD";
    });
  }

  /* ══════════════════════════════════════════════════════════════════════
     FEATURED ROW (home)
     ══════════════════════════════════════════════════════════════════════ */
  function renderFeatured() {
    const row = $("#featuredRow");
    if (!row) return;
    const picks = ["n9-k02", "n9-h02", "n9-c01", "n9-c05"]
      .map(id => D.MENU.find(m => m.id === id))
      .filter(Boolean);
    row.innerHTML = picks.map((m, i) => bowlCard(m, i + 1)).join("");
    $$("[data-add]", row).forEach(b =>
      b.addEventListener("click", () => addToCart(b.dataset.add)));
    tiltIn(row);
  }

  /* ══════════════════════════════════════════════════════════════════════
     HERO STAGGER
     ══════════════════════════════════════════════════════════════════════ */
  function applyRevealOrder() {
    $$("[data-d]").forEach(el => el.style.setProperty("--d", el.dataset.d));
  }

  /* ══════════════════════════════════════════════════════════════════════
     INIT
     ══════════════════════════════════════════════════════════════════════ */
  function init() {
    load();

    // global delegated navigation
    document.addEventListener("click", e => {
      if (!e.target || !e.target.closest) return;
      const a = e.target.closest("a[data-link]");
      if (!a) return;
      e.preventDefault();
      closeDrawer();
      go(a.getAttribute("href").replace("#", ""));
    });

    // nav state
    const nav = $("#nav");
    addEventListener("scroll", () => nav && nav.classList.toggle("is-stuck", scrollY > 12), { passive: true });

    // cart drawer
    $("#cartBtn").addEventListener("click", () => { openDrawer(); if (window.FX && FX.sound) FX.sound.add(); });
    $("#drawerClose").addEventListener("click", closeDrawer);
    $("#drawerScrim").addEventListener("click", closeDrawer);
    $("#drawerCheckout").addEventListener("click", () => { closeDrawer(); go("checkout"); });
    $("#drawerClear").addEventListener("click", clearCart);
    addEventListener("keydown", e => {
      if (e.key === "Escape") closeDrawer();
      const tag = document.activeElement ? document.activeElement.tagName : "";
      if (e.key === "/" && tag !== "INPUT" && tag !== "TEXTAREA" && tag !== "SELECT") {
        e.preventDefault();
        go("menu");
        setTimeout(() => { const i = $("#searchInput"); if (i) i.focus(); }, 60);
      }
    });

    // menu
    renderSectors();
    renderAddons();
    renderFeatured();
    applyRevealOrder();

    const search = $("#searchInput");
    if (search) {
      search.addEventListener("input", e => { state.query = e.target.value; renderMenu(); });
      search.addEventListener("keydown", e => { if (e.key === "Escape") { search.value = ""; state.query = ""; renderMenu(); search.blur(); } });
    }
    const reset = $("#resetSearch");
    if (reset) reset.addEventListener("click", () => {
      state.query = ""; state.sector = "all";
      if (search) search.value = "";
      renderSectors(); renderMenu();
    });

    // outposts sort
    $$(".sortbar__b").forEach(b => b.addEventListener("click", () => {
      state.sort = b.dataset.sort;
      $$(".sortbar__b").forEach(x => x.classList.toggle("is-on", x === b));
      renderOutposts();
    }));

    // checkout
    const form = $("#checkoutForm");
    if (form) {
      form.addEventListener("input", clearErrorOnInput);
      form.addEventListener("change", clearErrorOnInput);
    }
    $$('input[name="pay"]').forEach(r => {
      r.checked = r.value === state.pay;          // restore a persisted choice
      r.addEventListener("change", e => {
        state.pay = e.target.value; save(); renderCheckoutSummary();
      });
    });
    const burn = $("#burnBtn");
    if (burn) burn.addEventListener("click", () => {
      const data = validate();
      if (!data) { toast("FORM INCOMPLETE", "Some uplink fields did not validate.", "warn", "!"); return; }
      if (!state.outpost) { toast("NO OUTPOST LOCKED", "Pick a collection point first.", "warn", "!"); return; }
      burnPass(data);
    });

    // hash routing
    addEventListener("hashchange", route);
    if (location.hash) route();
    else location.hash = "#home";   // hashchange will route
    syncAll();
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
