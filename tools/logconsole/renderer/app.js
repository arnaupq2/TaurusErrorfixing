'use strict';
(() => {
  const $ = (id) => document.getElementById(id);
  const logEl = $('log');
  const MAX_ENTRIES = 20000;
  const WARN = /\b(error|errors|fail|failed|failure|cannot|could not|not found|exception|invalid)\b/i;

  const state = { lines: 0, crashes: 0, devices: new Map(), entries: [], showTimes: true, wrap: false, crashOnly: false, lastCrash: null, status: null };

  const pad = (n, w = 2) => String(n).padStart(w, '0');
  function clock(ts) {
    const d = new Date(ts);
    return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`;
  }
  function dateTime(ts) {
    const d = new Date(ts);
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${clock(ts)}`;
  }

  // --- appending -------------------------------------------------------------------------------
  let pending = [];
  let scheduled = false;
  function enqueue(node, entry) {
    pending.push([node, entry]);
    if (!scheduled) { scheduled = true; requestAnimationFrame(flush); }
  }
  function flush() {
    scheduled = false;
    const frag = document.createDocumentFragment();
    for (const [node, entry] of pending) { frag.appendChild(node); state.entries.push(entry); }
    pending = [];
    const empty = $('empty');
    if (empty) empty.remove();
    logEl.appendChild(frag);
    while (state.entries.length > MAX_ENTRIES) {
      const old = state.entries.shift();
      old.node.remove();
    }
    if ($('autoscroll').checked) logEl.scrollTop = logEl.scrollHeight;
    $('st-lines').textContent = `Lines: ${state.lines}`;
  }

  function deviceName(clientId) {
    const d = state.devices.get(clientId);
    if (!d) return '';
    return d.info ? d.info.device : d.remote;
  }

  function addLine(ev, kind) {
    const node = document.createElement('div');
    node.className = 'line' + (kind === 'sys' ? ' sys' : WARN.test(ev.text) ? ' warn' : '');
    const t = document.createElement('span');
    t.className = 't';
    t.textContent = clock(ev.ts) + ' ';
    node.appendChild(t);
    if (state.devices.size > 1 && ev.clientId) {
      const d = document.createElement('span');
      d.className = 'd';
      d.textContent = `[${deviceName(ev.clientId)}] `;
      node.appendChild(d);
    }
    node.appendChild(document.createTextNode(ev.text));
    const entry = { kind: 'line', text: `${clock(ev.ts)} ${ev.text}`, node };
    state.lines++;
    node.hidden = !matchesFilter(entry);
    enqueue(node, entry);
  }

  function addCrash(ev) {
    const node = document.createElement('div');
    node.className = 'crash';
    const head = document.createElement('div');
    head.className = 'crash-head';
    const tag = document.createElement('span');
    tag.textContent = '[CRASH]';
    const what = document.createElement('span');
    what.textContent = ev.summary || ev.kind;
    const when = document.createElement('span');
    when.className = 'when';
    when.textContent = `${deviceName(ev.clientId)} · ${dateTime(ev.ts)}`;
    head.append(tag, what, when);
    const body = document.createElement('pre');
    body.className = 'crash-body';
    body.textContent = ev.body;
    node.append(head, body);
    const entry = { kind: 'crash', text: `[CRASH] ${ev.summary}\n${ev.body}`, node };
    state.crashes++;
    state.lastCrash = node;
    enqueue(node, entry);
    $('st-crashes').textContent = `Crashes: ${state.crashes}`;
    $('st-crashes').classList.add('bad');
    $('btn-crash').classList.add('has-crash');
    document.body.classList.add('crashed');
    $('autoscroll').checked = true;
    requestAnimationFrame(() => requestAnimationFrame(() => { node.classList.add('flash'); node.scrollIntoView({ block: 'center' }); }));
  }

  function system(text) { addLine({ ts: Date.now(), text }, 'sys'); }

  // --- state shown around the log --------------------------------------------------------------
  function renderDevices() {
    const ul = $('devices');
    ul.textContent = '';
    if (!state.devices.size) {
      const li = document.createElement('li');
      li.className = 'muted';
      li.textContent = 'No phone yet';
      ul.appendChild(li);
    }
    let online = 0;
    for (const d of state.devices.values()) {
      if (d.connected) online++;
      const li = document.createElement('li');
      const dot = document.createElement('span');
      dot.className = 'dot' + (d.connected ? ' on' : '');
      li.append(dot, document.createTextNode(d.info ? d.info.device : d.remote));
      const sub = document.createElement('span');
      sub.className = 'dev-sub';
      sub.textContent = d.info
        ? `iOS ${d.info.os} · ${d.info.app} ${d.info.version}\nmod ${d.info.mod} · ${d.remote}`
        : d.remote;
      sub.style.whiteSpace = 'pre-line';
      li.appendChild(sub);
      ul.appendChild(li);
    }
    $('st-devices').textContent = `Phones: ${online}`;
  }

  function renderStatus(s) {
    state.status = s;
    const listen = $('listening');
    const field = $('st-listen');
    if (s.listening) {
      listen.textContent = `Listening on port ${s.port}`;
      field.textContent = `Listening on :${s.port}`;
      field.classList.remove('bad');
    } else {
      const inUse = s.error === 'EADDRINUSE';
      listen.textContent = inUse ? `Port ${s.port} is already in use. Close the other Taurus Log Console.` : `Not listening (${s.error})`;
      field.textContent = inUse ? `Port ${s.port} in use` : 'Not listening';
      field.classList.add('bad');
    }
    const ul = $('addresses');
    ul.textContent = '';
    for (const a of s.addresses || []) {
      const li = document.createElement('li');
      const b = document.createElement('button');
      b.textContent = a.address;
      b.title = `${a.name} - click to copy`;
      b.addEventListener('click', () => { navigator.clipboard?.writeText(a.address).catch(() => {}); b.textContent = 'copied'; setTimeout(() => { b.textContent = a.address; }, 900); });
      li.appendChild(b);
      ul.appendChild(li);
    }
  }

  // --- events from the main process ------------------------------------------------------------
  window.taurus.onEvent((ev) => {
    switch (ev.type) {
      case 'status': renderStatus(ev); break;
      case 'client': {
        const was = state.devices.get(ev.clientId);
        state.devices.set(ev.clientId, { ...ev });
        renderDevices();
        if (!was) system(`phone connected from ${ev.remote}`);
        else if (!ev.connected) system(`phone disconnected (${ev.info ? ev.info.device : ev.remote})`);
        else if (ev.info && !was.info) system(`${ev.info.device}, iOS ${ev.info.os}, ${ev.info.app} ${ev.info.version}, mod ${ev.info.mod}, pid ${ev.info.pid}`);
        break;
      }
      case 'log': addLine(ev); break;
      case 'crash': addCrash(ev); break;
      case 'maximized': $('btn-max').setAttribute('aria-label', ev.value ? 'Restore' : 'Maximize'); break;
    }
  });

  // --- actions ---------------------------------------------------------------------------------
  function matchesFilter(entry) {
    const q = $('filter').value.trim().toLowerCase();
    return !q || entry.text.toLowerCase().includes(q);
  }
  function applyFilter() {
    for (const e of state.entries) if (e.kind === 'line') e.node.hidden = !matchesFilter(e);
    for (const [node, e] of pending) if (e.kind === 'line') node.hidden = !matchesFilter(e);
  }
  function clearLog() {
    state.entries.length = 0;
    pending = [];
    logEl.textContent = '';
    state.lines = 0;
    $('st-lines').textContent = 'Lines: 0';
  }
  function allText() { return state.entries.map((e) => e.text).join('\n') + '\n'; }

  function dialog(title, html) {
    $('dialog-title').textContent = title;
    $('dialog-body').innerHTML = html;
    $('dialog').hidden = false;
    $('dialog-ok').focus();
  }
  function closeDialog() { $('dialog').hidden = true; }

  function setToggle(id, on, label) {
    const el = $(id);
    el.textContent = label;
    el.classList.toggle('on', on);
  }
  function refreshToggles() {
    document.body.classList.toggle('notimes', !state.showTimes);
    document.body.classList.toggle('wrap', state.wrap);
    document.body.classList.toggle('crashonly', state.crashOnly);
    setToggle('m-times', state.showTimes, 'Show times');
    setToggle('m-wrap', state.wrap, 'Wrap long lines');
    setToggle('m-crash', state.crashOnly, 'Crashes only');
  }

  const actions = {
    clear: clearLog,
    save: () => window.taurus.saveLog(allText()),
    folder: () => window.taurus.openLogFolder(),
    exit: () => window.taurus.windowAction('close'),
    'toggle-times': () => { state.showTimes = !state.showTimes; refreshToggles(); },
    'toggle-wrap': () => { state.wrap = !state.wrap; refreshToggles(); },
    'toggle-crashonly': () => { state.crashOnly = !state.crashOnly; refreshToggles(); },
    'last-crash': () => {
      if (state.lastCrash) state.lastCrash.scrollIntoView({ block: 'center' });
      else dialog('Taurus Log Console', '<p>No crash has been received yet.</p>');
    },
    howto: () => {
      const addr = ((state.status && state.status.addresses) || []).map((a) => a.address)[0] || '<this PC\'s address>';
      dialog('How to connect', `<ol>
        <li>Build the phone side with the flag on: <code>make install IPLOG=1</code>, or run the GitHub build with <b>iplog</b> ticked.</li>
        <li>Keep this window open on a computer on the <b>same Wi-Fi</b> as the phone.</li>
        <li>Open Spotify. Allow <b>Local Network</b> when iOS asks, once.</li>
      </ol>
      <p>The phone finds this window through Bonjour. If your network blocks that, pin the address: <code>IPLOG_HOST=${addr}</code> (port ${state.status ? state.status.port : 9000}).</p>
      <p>Allow this app through the Windows firewall (private networks) when it asks.</p>`);
    },
    about: () => dialog('About Taurus Log Console', '<p><b>Taurus Log Console</b><br>Live logs and crash reports from Taurus IPLOG builds.<br>Wire format: see <code>Core/SGRemoteLogCore.h</code>.</p><p>Skin: XP.css (MIT).</p>'),
  };

  document.addEventListener('click', (e) => {
    const t = e.target.closest('[data-act],[data-win],[data-dialog-close],.menu-label');
    const menus = document.querySelectorAll('.menu');
    if (t && t.classList.contains('menu-label')) {
      const m = t.parentElement, open = m.classList.contains('open');
      menus.forEach((x) => x.classList.remove('open'));
      if (!open) m.classList.add('open');
      return;
    }
    menus.forEach((x) => x.classList.remove('open'));
    if (!t) return;
    if (t.dataset.act && actions[t.dataset.act]) actions[t.dataset.act]();
    else if (t.dataset.win) window.taurus.windowAction(t.dataset.win);
    else if (t.hasAttribute('data-dialog-close')) closeDialog();
  });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeDialog();
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'l') { e.preventDefault(); clearLog(); }
  });
  $('filter').addEventListener('input', applyFilter);
  // Scrolling away from the bottom stops the follow; coming back resumes it.
  logEl.addEventListener('scroll', () => {
    const atBottom = logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 24;
    $('autoscroll').checked = atBottom;
  });

  refreshToggles();
  window.taurus.ready().then((info) => {
    renderStatus(info.status);
    for (const d of info.devices) state.devices.set(d.clientId, d);
    renderDevices();
  });
})();
