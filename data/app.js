/* Rubbish Console
   Talks to the same API as before: /api/config, /api/schedule, /api/devices,
   /api/status, /api/version, /api/lookup/postcode, /api/refresh, /api/test,
   /api/device/config. No firmware changes required. */

const POLL_MS = 5000;
const RETRY_LIMIT = 5;

/* The four streams the council returns, in the order we always show them.
   `key` is matched against the uppercased Service string. */
const STREAMS = [
    { key: 'RUBBISH', name: 'Rubbish',      short: 'RUBBISH', colour: 'rubbish',   note: 'Black wheelie bin' },
    { key: 'RECYCL',  name: 'Recycling',    short: 'RECYCLE', colour: 'recycling', note: 'Blue wheelie bin' },
    { key: 'FOOD',    name: 'Food waste',   short: 'FOOD',    colour: 'food',      note: 'Food box' },
    { key: 'GARDEN',  name: 'Garden waste', short: 'GARDEN',  colour: 'garden',    note: 'Subscription bin' }
];

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const DAYS_LONG = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
const DAYS_SHORT = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

let pollInterval = null;
let isPolling = false;
let lastKnownUpdate = 0;

/* This file serves both the dashboard and console.html, which carry
   different halves of the markup. Every render bails out if the thing it
   draws into is not on this page. */
function el(id) { return document.getElementById(id); }

/* Latest server state, kept so any single poll can re-render everything. */
let schedule = [];
let devices = [];
let status = {};

document.addEventListener('DOMContentLoaded', () => {
    applyStoredTheme();
    wireTheme();
    wirePanel();

    loadConfig();
    loadVersion();
    loadSchedule();
    loadDevices();
    loadStatus();
    startPolling();

    const modal = el('add-device-modal');
    if (modal) {
        modal.addEventListener('click', (e) => {
            if (e.target.id === 'add-device-modal') closeModal();
        });
    }

    // Fetch the raw payload the first time the panel is opened, not on load.
    const raw = el('raw-details');
    if (raw) raw.addEventListener('toggle', () => { if (raw.open) loadRaw(); }, { once: false });
});

/* ---------- theme ---------- */

function applyTheme(mode) {
    const root = document.documentElement;
    if (mode === 'auto') root.removeAttribute('data-theme');
    else root.setAttribute('data-theme', mode);

    document.querySelectorAll('[data-theme-set]').forEach(b => {
        b.setAttribute('aria-pressed', String(b.dataset.themeSet === mode));
    });
    try { localStorage.setItem('rc-theme', mode); } catch (e) { /* private mode */ }
}

function applyStoredTheme() {
    let mode = 'auto';
    try { mode = localStorage.getItem('rc-theme') || 'auto'; } catch (e) { /* ignore */ }
    applyTheme(mode);
}

function wireTheme() {
    document.querySelectorAll('[data-theme-set]').forEach(b => {
        b.addEventListener('click', () => applyTheme(b.dataset.themeSet));
    });
}

/* ---------- polling ---------- */

function startPolling() {
    if (pollInterval) return;
    pollInterval = setInterval(pollStatus, POLL_MS);
}

async function pollStatus() {
    if (isPolling) return;
    isPolling = true;
    try {
        const [statusRes, devicesRes] = await Promise.all([
            fetch('/api/status'),
            fetch('/api/devices')
        ]);

        if (statusRes.ok) {
            status = await statusRes.json();
            renderStatus();

            // Server fetched new council data: pull the schedule again.
            if (status.lastUpdate !== lastKnownUpdate && status.lastUpdate > 0) {
                lastKnownUpdate = status.lastUpdate;
                loadSchedule();
            }
        }

        if (devicesRes.ok) {
            devices = await devicesRes.json();
            renderDevices();
            renderPanelBar();
        }
    } catch (e) {
        // Network blip — the next tick picks it up.
    } finally {
        isPolling = false;
    }
}

/* ---------- loaders ---------- */

async function loadVersion() {
    try {
        const res = await fetch('/api/version');
        const data = await res.json();
        if (el('fw-version')) el('fw-version').textContent = data.version;
    } catch (e) {
        if (el('fw-version')) el('fw-version').textContent = 'unknown';
    }
}

async function loadConfig() {
    try {
        const res = await fetch('/api/config');
        if (!res.ok) return;
        const data = await res.json();
        const name = data.propertyName || '';

        if (!el('current-uprn')) return;
        el('current-uprn').textContent = data.uprn || 'Not set';
        el('current-address').textContent = name || (data.uprn ? 'UPRN only' : 'Not set');
        el('brand-sub').textContent = name || 'No property set — open Property & system';
    } catch (e) {
        console.error('Failed to load config', e);
    }
}

async function loadSchedule() {
    try {
        const res = await fetch('/api/schedule');
        if (!res.ok) return;
        schedule = parseSchedule(await res.json());
        renderHero();
        renderFortnight();
        renderStreamList();
        renderPanel();
        renderDevices();
    } catch (e) {
        console.error('Failed to load schedule', e);
    }
}

async function loadDevices() {
    try {
        const res = await fetch('/api/devices');
        if (!res.ok) throw new Error('bad status');
        devices = await res.json();
        // Called after pair/remove/rotate, so this one really must redraw.
        renderDevices(true);
        renderPanelBar();
    } catch (e) {
        if (el('device-list')) {
            el('device-list').innerHTML = '<p class="hint-sm">Could not load labels.</p>';
        }
    }
}

async function loadStatus() {
    try {
        const res = await fetch('/api/status');
        status = await res.json();
    } catch (e) {
        status = {};
    }
    renderStatus();
}

/* ---------- schedule parsing ---------- */

function parseDate(str) {
    // Council returns dd/mm/yyyy, sometimes repeated: "23/03/2026,23/03/2026"
    const first = String(str || '').split(',')[0].trim();
    const m = first.match(/^(\d{2})\/(\d{2})\/(\d{4})$/);
    if (!m) return null;
    return new Date(Number(m[3]), Number(m[2]) - 1, Number(m[1]));
}

function midnight(d) {
    return new Date(d.getFullYear(), d.getMonth(), d.getDate());
}

function daysBetween(from, to) {
    return Math.round((midnight(to) - midnight(from)) / 86400000);
}

/* How often a stream repeats, read from collectionSchedule:
     "FOOD5 Mon"        -> weekly
     "REC8 MonFort1"    -> fortnightly
   Returns 0 when the field tells us nothing, in which case we plot only the
   one date we were actually given rather than inventing collections. */
function cadenceDays(collectionSchedule) {
    const s = String(collectionSchedule || '').toUpperCase();
    if (!s) return 0;
    if (s.indexOf('FORT') >= 0) return 14;
    if (/\b(MON|TUE|WED|THU|FRI|SAT|SUN)\b/.test(s)) return 7;
    return 0;
}

/* One entry per stream, earliest date only, soonest first. The council only
   ever returns the NEXT date for each service, so the cadence is what lets
   us project repeats across the fortnight. */
function parseSchedule(json) {
    if (!json || !Array.isArray(json.param2)) return [];

    const out = [];
    STREAMS.forEach(def => {
        let best = null;
        let cadence = 0;
        json.param2.forEach(item => {
            const service = String(item.Service || '').toUpperCase();
            if (service.indexOf(def.key) < 0) return;
            const date = parseDate(item.collectionDateString);
            if (!date) return;
            if (!best || date < best) {
                best = date;
                cadence = cadenceDays(item.collectionSchedule);
            }
        });
        if (best) out.push({ def: def, date: best, cadence: cadence });
    });

    out.sort((a, b) => a.date - b.date);
    return out;
}

/* Every occurrence of `s` falling on `day`, including projected repeats. */
function collectsOn(s, day) {
    const gap = daysBetween(s.date, day);
    if (gap === 0) return true;
    if (gap < 0 || !s.cadence) return false;
    return gap % s.cadence === 0;
}

/* The distinct collection days coming up, projected from each stream's
   cadence. Used to offer "send me next week's label" previews. */
function upcomingCollectionDays(limit) {
    if (!schedule.length) return [];
    const today = midnight(new Date());
    const days = [];
    for (let i = 0; i < 56 && days.length < (limit || 4); i++) {
        const d = new Date(today.getFullYear(), today.getMonth(), today.getDate() + i);
        const due = schedule.filter(s => collectsOn(s, d));
        if (due.length) days.push({ date: d, due: due });
    }
    return days;
}

/* The state as it will be on a given collection day, so a preview shows that
   day's real streams rather than today's with different wording. */
function stateForDay(day) {
    const due = schedule.filter(s => collectsOn(s, day));
    if (!due.length) return null;
    const diff = daysBetween(new Date(), day);
    const later = schedule.filter(s => !collectsOn(s, day));

    return {
        diff: diff,
        next: day,
        due: due,
        later: later,
        tagAct: diff === 0 ? 'COLLECTION TODAY'
            : diff === 1 ? 'BINS OUT TONIGHT'
            : diff === 2 ? 'BINS OUT TOMORROW' : 'NEXT COLLECTION',
        tearRed: diff === 1,
        actRed: diff === 2
    };
}

/* Everything the three surfaces need, derived once from the schedule.
   The wording is action-framed: the tag says what to DO, not when the
   lorry arrives, because a tag reading "TODAY" on the kerb is ambiguous. */
function currentState() {
    if (!schedule.length) return null;

    const today = new Date();
    const next = schedule[0].date;
    const diff = daysBetween(today, next);
    const due = schedule.filter(s => s.date.getTime() === next.getTime());
    const later = schedule.filter(s => s.date.getTime() !== next.getTime());

    let chip, action, note, tagAct, tearRed = false, actRed = false, calm = true;

    if (diff === 0) {
        // Collection day. Deliberately no red: if the bins are not already
        // out the moment has gone, and shouting only devalues the red.
        chip = 'Today';
        action = 'Collection today';
        note = 'Crews started at 6:00 am';
        tagAct = 'COLLECTION TODAY';
    } else if (diff === 1) {
        chip = 'Tomorrow';
        action = 'Bins out tonight';
        note = 'Crews start at 6:00 am tomorrow';
        tagAct = 'BINS OUT TONIGHT';
        tearRed = true;
        calm = false;
    } else if (diff === 2) {
        chip = 'In 2 days';
        action = 'Bins out tomorrow';
        note = joinList(due.map(s => s.def.name)) + ' together';
        tagAct = 'BINS OUT TOMORROW';
        actRed = true;
        calm = false;
    } else {
        chip = diff < 0 ? 'Overdue' : 'In ' + diff + ' days';
        action = 'Next collection';
        note = 'Nothing to put out yet';
        tagAct = 'NEXT COLLECTION';
    }

    return { diff, next, due, later, chip, action, note, tagAct, tearRed, actRed, calm };
}

/* ---------- small formatters ---------- */

function joinList(items) {
    if (items.length <= 1) return items.join('');
    return items.slice(0, -1).join(', ') + ' and ' + items[items.length - 1];
}

function shortDate(d) {
    return DAYS_SHORT[d.getDay()] + ' ' + d.getDate() + ' ' + MONTHS[d.getMonth()];
}

function pad2(n) { return n < 10 ? '0' + n : String(n); }

function ddmm(d) { return pad2(d.getDate()) + '/' + pad2(d.getMonth() + 1); }

/* Relative time from a millisecond timestamp. */
function relTime(ms) {
    const mins = Math.round((Date.now() - ms) / 60000);
    if (mins < 1) return 'just now';
    if (mins < 60) return mins + ' min ago';
    const hrs = Math.floor(mins / 60);
    if (hrs < 24) return hrs + ' hr ago';
    const days = Math.floor(hrs / 24);
    return days + (days === 1 ? ' day ago' : ' days ago');
}

function relTimeSec(sec) {
    return sec > 0 ? relTime(sec * 1000) : 'never';
}

function escapeHtml(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

/* ---------- hero ---------- */

function renderHero() {
    const tear = el('hero-tear');
    if (!tear) return;
    const st = currentState();
    const actbar = document.getElementById('hero-actbar');

    if (!st) {
        tear.classList.remove('act');
        actbar.classList.add('calm');
        document.getElementById('hero-chip').textContent = '—';
        document.getElementById('hero-num').textContent = '–';
        document.getElementById('hero-dow').textContent = '—';
        document.getElementById('hero-mon').textContent = '';
        document.getElementById('act-h').textContent = 'Waiting for schedule';
        document.getElementById('hero-note').textContent =
            'No collection data yet. Set a property, then check the council.';
        document.getElementById('hero-streams').innerHTML = '';
        document.getElementById('hero-then').textContent = '';
        return;
    }

    tear.classList.toggle('act', st.tearRed);
    actbar.classList.toggle('calm', st.calm);

    document.getElementById('hero-chip').textContent = st.chip;
    document.getElementById('hero-num').textContent = st.next.getDate();
    document.getElementById('hero-dow').textContent = DAYS_LONG[st.next.getDay()];
    document.getElementById('hero-mon').textContent = MONTHS[st.next.getMonth()];
    document.getElementById('act-h').textContent = st.action;
    document.getElementById('hero-note').textContent = st.note;

    document.getElementById('hero-streams').innerHTML = st.due.map(s => `
        <li>
            <span class="swatch" style="--s:var(--stream-${s.def.colour})"></span>
            <div>
                <h3>${s.def.name}</h3>
                <p>${s.def.note}</p>
            </div>
        </li>`).join('');

    const then = document.getElementById('hero-then');
    if (st.later.length) {
        const nextDate = st.later[0].date;
        const names = st.later
            .filter(s => s.date.getTime() === nextDate.getTime())
            .map(s => '<strong>' + s.def.name + '</strong>');
        then.innerHTML = 'Then ' + joinList(names) + ' on ' + shortDate(nextDate);
    } else {
        then.textContent = '';
    }
}

/* ---------- fortnight strip ---------- */

function renderFortnight() {
    const strip = el('fortnight');
    if (!strip) return;
    const today = midnight(new Date());
    const st = currentState();

    let html = '';
    for (let i = 0; i < 14; i++) {
        const d = new Date(today.getFullYear(), today.getMonth(), today.getDate() + i);
        const weekend = d.getDay() === 0 || d.getDay() === 6;
        const due = schedule.filter(s => collectsOn(s, d));
        const dots = due
            .map(s => `<i style="--s:var(--stream-${s.def.colour})"></i>`)
            .join('');

        const cls = ['day'];
        if (i === 0) cls.push('today');
        if (weekend) cls.push('weekend');
        if (due.length) cls.push('coll');

        // Saying TODAY is clearer than a marker you have to decode, and it
        // frees the cell from needing a highlight of its own.
        const label = i === 0 ? 'Today' : DAYS_SHORT[d.getDay()];

        html += `<div class="${cls.join(' ')}" role="listitem">
            <span class="dl">${label}</span>
            <span class="dn">${d.getDate()}</span>
            <span class="dots">${dots}</span>
        </div>`;
    }
    strip.innerHTML = html;

    document.getElementById('fn-hint').textContent =
        st ? DAYS_LONG[st.next.getDay()] + ' round' : '';
}

/* ---------- all streams ---------- */

function renderStreamList() {
    const list = el('stream-list');
    if (!list) return;
    if (!schedule.length) {
        list.innerHTML = '<div class="srow"><span></span><span class="nm">No schedule yet</span><span></span><span></span></div>';
        return;
    }

    const today = new Date();
    list.innerHTML = schedule.map(s => {
        const diff = daysBetween(today, s.date);
        let when;
        if (diff === 0) when = 'Today';
        else if (diff === 1) when = 'Tomorrow';
        else if (diff < 0) when = Math.abs(diff) + ' days ago';
        else when = 'in ' + diff + ' days';

        const soon = diff >= 0 && diff <= 2;
        return `<div class="srow${soon ? ' soon' : ''}">
            <span class="bar" style="--s:var(--stream-${s.def.colour})"></span>
            <span class="nm">${s.def.name}</span>
            <span class="when">${when}</span>
            <span class="dt mono">${ddmm(s.date)}/${s.date.getFullYear()}</span>
        </div>`;
    }).join('');
}

/* ---------- the label frame, shared by tag previews and the LCD ---------- */

/* Three full names clear the footer rule; four do not, so the fourth case
   collapses the tail onto a shared line of short names. */
function tagLines(due) {
    if (due.length <= 3) return due.map(s => s.def.name.toUpperCase());
    return [due[0].def.name.toUpperCase(), due.slice(1).map(s => s.def.short).join(' / ')];
}

/* Matches the "%d/%m %H:%M" stamp the firmware draws on the tag. */
function fmtStamp(sec) {
    const d = new Date(sec * 1000);
    return pad2(d.getDate()) + '/' + pad2(d.getMonth() + 1) + ' ' +
        pad2(d.getHours()) + ':' + pad2(d.getMinutes());
}

function renderTagInner(st, actOverride, forceRed, stamp) {
    if (!st) return '<div class="tag-stale">NO SCHEDULE</div>';

    const lines = tagLines(st.due).map(l => `<b>${l}</b>`).join('');
    const foot = st.later.map(s => `<span>${s.def.short} ${ddmm(s.date)}</span>`).join('');
    const act = actOverride || st.tagAct;
    const actRed = forceRed || st.actRed;
    // An error frame never floods the date block: red belongs to one thing
    // at a time, and here that thing is the error line.
    const dateRed = forceRed ? false : st.tearRed;

    return `
        <div class="tag-date${dateRed ? ' red' : ''}">
            <span class="dw">${DAYS_SHORT[st.next.getDay()].toUpperCase()}</span>
            <span class="dn">${st.next.getDate()}</span>
            <span class="dm">${MONTHS[st.next.getMonth()].toUpperCase()}</span>
            ${stamp ? `<span class="ds">${stamp}</span>` : ''}
        </div>
        <div class="tag-body">
            <span class="tag-act${actRed ? ' red' : ''}">${act}</span>
            <div class="tag-streams">${lines}</div>
            <div class="tag-foot">${foot}</div>
        </div>`;
}

/* What each "Send to label" option will actually put on the tag. Mirrors
   _sendTestPattern() and _generateAndSendImage() in the firmware. */
function frameFor(type, mac, stamp) {
    const st = currentState();

    if (type === 'pattern') {
        return {
            cls: 'tag plain border-red2',
            html: '<div class="tag-checks"></div>' +
                  '<div class="tag-centre"><span class="big">TEST PATTERN</span>' +
                  `<span class="mac">${mac}</span></div>`
        };
    }
    if (type === 'hello') {
        return {
            cls: 'tag plain border-red',
            html: '<div class="tag-centre"><span class="big">HELLO WORLD!</span></div>'
        };
    }
    // Mirrors the firmware's "dayN" override: shift the date as well as the
    // wording, so the preview is coherent rather than a mismatched pair.
    if (/^day\d+$/.test(type) && st) {
        const offset = Number(type.slice(3));
        const shown = new Date();
        shown.setDate(shown.getDate() + offset);
        const fake = Object.assign({}, st, {
            next: shown,
            diff: offset,
            tagAct: offset === 0 ? 'COLLECTION TODAY'
                : offset === 1 ? 'BINS OUT TONIGHT'
                : offset === 2 ? 'BINS OUT TOMORROW' : 'NEXT COLLECTION',
            tearRed: offset === 1,
            actRed: offset === 2
        });
        return { cls: 'tag', html: renderTagInner(fake, null, false, stamp) };
    }

    // "date:07/09/2026" — the real frame for a real upcoming collection.
    if (type.indexOf('date:') === 0) {
        const m = type.slice(5).match(/^(\d{2})\/(\d{2})\/(\d{4})$/);
        if (m) {
            const day = new Date(Number(m[3]), Number(m[2]) - 1, Number(m[1]));
            const fake = stateForDay(day);
            if (fake) return { cls: 'tag', html: renderTagInner(fake, null, false, stamp) };
        }
    }

    if (type === 'wifi') {
        return { cls: 'tag', html: renderTagInner(st, 'NO WIFI - LAST KNOWN', true, stamp) };
    }
    if (type === 'api') {
        return { cls: 'tag', html: renderTagInner(st, 'API ERROR - LAST KNOWN', true, stamp) };
    }
    return { cls: 'tag', html: renderTagInner(st, null, false, stamp) };
}

/* Changing the dropdown repaints that card's preview, so you can see what
   you are about to write before you write it. */
function previewFrame(mac, id) {
    const tag = el('tag-' + id);
    const select = el('send-' + id);
    if (!tag || !select) return;

    const frame = frameFor(select.value, mac, tag.dataset.stamp || '');
    const flipped = tag.classList.contains('flipped');
    tag.className = frame.cls + (flipped ? ' flipped' : '');
    tag.innerHTML = frame.html;

    const cap = el('cap-' + id);
    if (cap) {
        cap.textContent = select.value === 'schedule'
            ? (tag.dataset.written === '1' ? 'Last written frame' : 'Not reached yet')
            : 'Preview — not sent yet';
    }
}

/* ---------- onboard panel preview ---------- */

function wirePanel() {
    const lcd = el('lcd');
    if (!lcd) return;
    const screens = document.querySelectorAll('[data-screen]');
    const btns = document.querySelectorAll('#lcd-seg [data-lcd]');

    btns.forEach(b => {
        b.addEventListener('click', () => {
            btns.forEach(o => o.setAttribute('aria-pressed', String(o === b)));
            const shown = b.dataset.lcd === 'net' ? 'net' : 'awake';
            screens.forEach(s => { s.hidden = s.dataset.screen !== shown; });
            lcd.classList.toggle('asleep', b.dataset.lcd === 'asleep');
        });
    });

    const zoomBtns = document.querySelectorAll('#lcd-zoom-seg [data-zoom]');
    zoomBtns.forEach(b => {
        b.addEventListener('click', () => {
            zoomBtns.forEach(o => o.setAttribute('aria-pressed', String(o === b)));
            lcd.style.setProperty('--lcd-zoom', b.dataset.zoom);
        });
    });

    document.getElementById('lcd-qr-slot').innerHTML = qrPlaceholder();
}

function renderPanel() {
    const date = el('lcd-date');
    if (!date) return;
    const st = currentState();
    const act = document.getElementById('lcd-act');
    const streams = document.getElementById('lcd-streams');

    if (!st) {
        date.classList.remove('red');
        date.innerHTML = '<span class="dw">--</span><span class="dn">–</span><span class="dm">---</span>';
        act.textContent = 'NO SCHEDULE';
        act.classList.add('calm');
        streams.innerHTML = '';
        renderPanelBar();
        return;
    }

    date.classList.toggle('red', st.tearRed);
    date.innerHTML =
        `<span class="dw">${DAYS_SHORT[st.next.getDay()].toUpperCase()}</span>` +
        `<span class="dn">${st.next.getDate()}</span>` +
        `<span class="dm">${MONTHS[st.next.getMonth()].toUpperCase()}</span>`;

    act.textContent = st.tagAct;
    act.classList.toggle('red', st.actRed);

    streams.innerHTML = st.due.map(s =>
        `<div><i style="--s:var(--dev-${s.def.colour})"></i>${s.def.name}</div>`).join('');

    renderPanelBar();
}

function renderPanelBar() {
    if (!el('lcd-time')) return;
    const now = new Date();
    document.getElementById('lcd-time').textContent = pad2(now.getHours()) + ':' + pad2(now.getMinutes());

    const synced = devices.filter(d => d && d.result).length;
    document.getElementById('lcd-labels').innerHTML = devices.length
        ? `<span class="ok">${synced}</span>/${devices.length} labels`
        : 'no labels';

    document.getElementById('lcd-bars').className = 'lcd-bars lvl' + rssiLevel(status.rssi);
    document.getElementById('lcd-ssid').textContent = status.ssid || '—';
    document.getElementById('lcd-ip').textContent = location.hostname || '—';
}

/* A stand-in block: the device draws the real QR with LovyanGFX. Deterministic
   so it does not shimmer between renders. */
function qrPlaceholder() {
    const N = 21;
    const m = [];
    for (let i = 0; i < N; i++) m.push(new Array(N).fill(0));

    const finder = (r, c) => {
        for (let y = 0; y <= 6; y++) {
            for (let x = 0; x <= 6; x++) {
                const edge = y === 0 || y === 6 || x === 0 || x === 6;
                const core = y >= 2 && y <= 4 && x >= 2 && x <= 4;
                m[r + y][c + x] = edge || core ? 1 : 0;
            }
        }
    };
    finder(0, 0); finder(0, N - 7); finder(N - 7, 0);

    let h = 4242;
    for (let i = 0; i < N; i++) {
        for (let j = 0; j < N; j++) {
            const inFinder = (i < 8 && j < 8) || (i < 8 && j > N - 9) || (i > N - 9 && j < 8);
            if (inFinder) continue;
            h = (h * 1103515245 + 12345) & 0x7fffffff;
            m[i][j] = (h >> 16) & 1;
        }
    }
    for (let i = 8; i < N - 8; i++) {
        m[6][i] = i % 2 === 0 ? 1 : 0;
        m[i][6] = i % 2 === 0 ? 1 : 0;
    }

    let rects = '';
    for (let i = 0; i < N; i++) {
        for (let j = 0; j < N; j++) {
            if (m[i][j]) rects += `<rect x="${j}" y="${i}" width="1" height="1" fill="#000"/>`;
        }
    }
    return `<svg viewBox="0 0 ${N} ${N}" width="88" height="88" style="background:#fff;padding:4px;border-radius:2px;shape-rendering:crispEdges" role="img" aria-label="QR code placeholder">${rects}</svg>`;
}

/* ---------- status / wifi ---------- */

function rssiLevel(rssi) {
    if (typeof rssi !== 'number' || rssi === 0) return 0;
    if (rssi > -60) return 4;
    if (rssi > -70) return 3;
    if (rssi > -80) return 2;
    return 1;
}

function renderStatus() {
    const bars = el('wifi-bars');
    if (!bars) return;
    const text = document.getElementById('wifi-text');
    const wrap = document.getElementById('wifi-status');

    const level = rssiLevel(status.rssi);
    bars.className = 'bars lvl' + level;

    if (level === 0) {
        text.textContent = 'Offline';
        wrap.title = 'No WiFi signal reported';
    } else {
        text.innerHTML = `${escapeHtml(status.ssid || 'Connected')} · <span class="mono">${status.rssi} dBm</span>`;
        wrap.title = 'WiFi signal ' + status.rssi + ' dBm';
    }

    if (el('hero-checked')) {
        el('hero-checked').innerHTML = status.lastUpdate > 0
            ? 'Council checked <span class="mono">' + relTime(status.lastUpdate) + '</span>'
            : 'Council not checked yet';
    }

    if (el('next-poll')) {
        el('next-poll').textContent = status.nextUpdate > 0
            ? new Date(status.nextUpdate).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
            : 'ASAP';
    }

    renderPanelBar();
}

/* ---------- labels ---------- */

function deviceState(d) {
    if (d.pending && d.retries > 0) return { cls: 'working', text: `Retry ${d.retries} of ${RETRY_LIMIT}` };
    if (d.pending) return { cls: 'working', text: 'Queued' };
    if (d.result) return { cls: 'ok', text: 'Synced' };
    if (d.lastAttempt > 0) return { cls: 'fail', text: 'Failed' };
    return { cls: 'working', text: 'Never written' };
}

/* BLE signal of the last sighting — the number that predicts range problems. */
function bleSignal(d) {
    if (typeof d.rssi !== 'number' || d.rssi >= 0) {
        return { text: '—', warn: false, title: 'Not seen since boot' };
    }
    let quality = 'weak';
    if (d.rssi > -60) quality = 'excellent';
    else if (d.rssi > -75) quality = 'good';
    else if (d.rssi > -85) quality = 'fair';

    const measured = d.rssiTime > 0 ? new Date(d.rssiTime * 1000).toLocaleString() : 'unknown time';
    return {
        text: `${d.rssi} dBm (${quality})`,
        warn: d.rssi <= -85,
        title: 'Measured ' + measured
    };
}

let lastDevicesSig = '';

/* Cheap fingerprint of everything the cards actually draw from. */
function devicesSignature() {
    return JSON.stringify(devices) + '|' +
        (schedule.length ? schedule[0].date.getTime() : 0);
}

function renderDevices(force) {
    const list = el('device-list');
    if (!list) return;

    // Never rebuild the cards while the user is inside one. Replacing the
    // markup destroys the <select> they are using, which closes the menu and
    // looks like the choice snapping back.
    if (!force && list.contains(document.activeElement)) return;

    // And do not rebuild at all unless something changed. The poll runs every
    // five seconds; the device data usually has not moved.
    const sig = devicesSignature();
    if (!force && sig === lastDevicesSig && list.children.length) return;
    lastDevicesSig = sig;

    // A preview the user picked is not in the device data, so carry it over.
    const chosen = {};
    list.querySelectorAll('select[id^="send-"]').forEach(s => {
        if (s.value !== 'schedule') chosen[s.id] = s.value;
    });

    if (!Array.isArray(devices) || devices.length === 0) {
        list.innerHTML = '<p class="hint-sm">No labels paired yet. Use <strong>Pair a label</strong> to add one.</p>';
        return;
    }

    const st = currentState();
    const upcoming = upcomingCollectionDays(4);

    list.innerHTML = devices.map(raw => {
        const d = typeof raw === 'object' && raw !== null ? raw : { mac: raw };
        const mac = escapeHtml(d.mac || String(raw));
        const state = deviceState(d);
        const signal = bleSignal(d);
        const rotate = d.rotate === true;
        const id = mac.replace(/[^A-Za-z0-9]/g, '');

        // We cannot read a tag back, so this is the last frame we sent it.
        let preview, caption;
        const stamp = d.lastSuccess > 0 ? fmtStamp(d.lastSuccess) : '';

        // Real upcoming collections, so you can send next week's label today.
        const dayOptions = upcoming.length ? '<optgroup label="Preview a real collection">' +
            upcoming.map(u => {
                const val = 'date:' + pad2(u.date.getDate()) + '/' +
                    pad2(u.date.getMonth() + 1) + '/' + u.date.getFullYear();
                const names = u.due.map(x => x.def.name).join(', ');
                return `<option value="${val}">${shortDate(u.date)} — ${names}</option>`;
            }).join('') + '</optgroup>' : '';
        if (d.lastSuccess > 0) {
            preview = renderTagInner(st, null, false, stamp);
            caption = d.pending ? 'Last written frame · update queued' : 'Last written frame';
        } else {
            preview = '<div class="tag-stale">NEVER WRITTEN</div>';
            caption = 'Not reached yet';
        }

        return `
        <article class="label-card${state.cls === 'fail' ? ' attention' : ''}">
            <div class="lc-head">
                <div>
                    <span class="nm mono">${mac}</span>
                    <span class="sub">E-ink label</span>
                </div>
                <span class="state ${state.cls}">${state.text}</span>
            </div>

            <dl class="lc-meta">
                <dt>Last write</dt><dd>${relTimeSec(d.lastSuccess)}</dd>
                <dt>Last seen</dt><dd>${relTimeSec(d.lastSeen)}</dd>
                <dt>Signal</dt><dd class="${signal.warn ? 'warn' : ''}" title="${escapeHtml(signal.title)}">${signal.text}</dd>
            </dl>

            <div class="preview-zone">
                <span class="eyebrow" id="cap-${id}">${caption}</span>
                <div class="tag${rotate ? ' flipped' : ''}" id="tag-${id}"
                     data-stamp="${stamp}"
                     data-written="${d.lastSuccess > 0 ? 1 : 0}">${preview}</div>
            </div>

            <div class="lc-actions">
                <div class="field">
                    <span class="flabel" id="rot-${id}">Orientation</span>
                    <div class="seg" role="group" aria-labelledby="rot-${id}">
                        <button type="button" aria-pressed="${!rotate}" onclick="setRotation('${mac}', false)">0&deg;</button>
                        <button type="button" aria-pressed="${rotate}" onclick="setRotation('${mac}', true)">180&deg;</button>
                    </div>
                </div>

                <div class="field">
                    <label for="send-${id}">Send to label</label>
                    <div class="row">
                        <select id="send-${id}" class="grow" onchange="previewFrame('${mac}', '${id}')">
                            <option value="schedule">Bin schedule</option>
                            ${dayOptions}
                            <optgroup label="Preview a collection state">
                                <option value="day0">As if collection were today</option>
                                <option value="day1">As if collection were tomorrow</option>
                                <option value="day2">As if collection were in 2 days</option>
                                <option value="day5">As if collection were in 5 days</option>
                            </optgroup>
                            <optgroup label="Diagnostics">
                                <option value="pattern">Test pattern</option>
                                <option value="hello">Hello screen</option>
                                <option value="wifi">No-WiFi screen</option>
                                <option value="api">API error screen</option>
                            </optgroup>
                        </select>
                        <button class="btn" type="button" onclick="sendToDevice('${mac}', '${id}', this)">Send</button>
                    </div>
                </div>

                <div class="row">
                    <span class="toast" id="toast-${id}"></span>
                    <button class="btn btn-quiet" type="button" style="margin-left:auto"
                        onclick="removeDevice('${mac}', this)">Remove</button>
                </div>
            </div>
        </article>`;
    }).join('');

    // Put any non-default preview back the way the user left it.
    Object.keys(chosen).forEach(selectId => {
        const s = el(selectId);
        if (!s) return;
        s.value = chosen[selectId];
        const id = selectId.slice('send-'.length);
        const card = s.closest('.label-card');
        const macEl = card ? card.querySelector('.nm') : null;
        previewFrame(macEl ? macEl.textContent : '', id);
    });
}

/* ---------- actions ---------- */

function setLoading(btn, on, restore) {
    if (!btn) return;
    if (on) {
        btn.dataset.original = btn.textContent;
        btn.textContent = 'Working…';
        btn.disabled = true;
    } else {
        btn.textContent = restore || btn.dataset.original || btn.textContent;
        btn.disabled = false;
    }
}

function say(id, message, bad) {
    const el = document.getElementById(id);
    if (!el) return;
    el.textContent = message;
    el.classList.toggle('bad', !!bad);
    if (message) {
        setTimeout(() => { el.textContent = ''; el.classList.remove('bad'); }, 5000);
    }
}

/* Raw council payload, for when the derived view and reality disagree. */
async function loadRaw() {
    const out = el('raw-json');
    if (!out) return;
    out.textContent = 'Loading…';
    try {
        const res = await fetch('/api/schedule');
        const json = await res.json();
        out.textContent = JSON.stringify(json, null, 2);
    } catch (e) {
        out.textContent = 'Could not read /api/schedule';
    }
}

async function refreshData() {
    const btn = document.getElementById('refresh-btn');
    setLoading(btn, true);
    try {
        const res = await fetch('/api/refresh', { method: 'POST' });
        say('refresh-toast', res.ok
            ? 'Queued — the device fetches on its next loop.'
            : 'The device refused the request.', !res.ok);
    } catch (e) {
        say('refresh-toast', 'Could not reach the device.', true);
    } finally {
        setLoading(btn, false, 'Check council now');
    }
}

async function searchAddress() {
    const postcode = document.getElementById('postcode').value.trim();
    if (!postcode) {
        say('config-toast', 'Enter a postcode first.', true);
        return;
    }

    const btn = document.getElementById('find-btn');
    setLoading(btn, true);

    try {
        const res = await fetch('/api/lookup/postcode?postcode=' + encodeURIComponent(postcode));
        if (!res.ok) throw new Error('lookup failed');
        const json = await res.json();

        const select = document.getElementById('address-select');
        const group = document.getElementById('address-select-group');
        select.innerHTML = '';

        if (json.param2 && Array.isArray(json.param2) && json.param2.length) {
            json.param2.forEach(addr => {
                const opt = document.createElement('option');
                opt.value = addr.Value;
                opt.textContent = addr.Text;
                select.appendChild(opt);
            });
            group.hidden = false;
            say('config-toast', json.param2.length + ' properties found.');
        } else {
            group.hidden = true;
            say('config-toast', 'No properties found for that postcode.', true);
        }
    } catch (e) {
        say('config-toast', 'Postcode lookup failed.', true);
    } finally {
        setLoading(btn, false, 'Find');
    }
}

async function saveAddress() {
    const select = document.getElementById('address-select');
    const uprn = select.value;
    if (!uprn) return;

    const btn = document.getElementById('save-addr-btn');
    const propertyName = select.options[select.selectedIndex].textContent;
    setLoading(btn, true);

    try {
        const res = await fetch('/api/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `uprn=${encodeURIComponent(uprn)}&propertyName=${encodeURIComponent(propertyName)}`
        });
        if (!res.ok) throw new Error('save failed');

        say('config-toast', 'Property saved. Fetching the schedule…');
        loadConfig();
        await fetch('/api/refresh', { method: 'POST' });
    } catch (e) {
        say('config-toast', 'Could not save the property.', true);
    } finally {
        setLoading(btn, false, 'Save property');
    }
}

async function setRotation(mac, rotate) {
    try {
        const res = await fetch('/api/device/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ mac: mac, rotate: rotate })
        });
        if (!res.ok) throw new Error('rotate failed');
        loadDevices();
    } catch (e) {
        alert('Could not change orientation.');
    }
}

async function sendToDevice(mac, id, btn) {
    const select = document.getElementById('send-' + id);
    const type = select ? select.value : 'schedule';
    const label = select ? select.options[select.selectedIndex].textContent : 'Frame';

    setLoading(btn, true);
    try {
        const res = await fetch('/api/test', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `mac=${encodeURIComponent(mac)}&type=${encodeURIComponent(type)}`
        });
        say('toast-' + id, res.ok ? label + ' queued' : 'Device refused the request', !res.ok);
    } catch (e) {
        say('toast-' + id, 'Could not reach the device', true);
    } finally {
        setLoading(btn, false, 'Send');
    }
}

async function removeDevice(mac, btn) {
    if (!confirm('Remove ' + mac + '? It will stop receiving updates.')) return;
    setLoading(btn, true);
    try {
        await fetch('/api/devices?mac=' + encodeURIComponent(mac), { method: 'DELETE' });
        loadDevices();
    } catch (e) {
        setLoading(btn, false, 'Remove');
        alert('Remove failed.');
    }
}

/* ---------- pairing modal ---------- */

let html5QrcodeScanner = null;

function cameraAvailable() {
    return location.protocol === 'https:' ||
        location.hostname === 'localhost' ||
        location.hostname === '127.0.0.1';
}

function openModal() {
    document.getElementById('add-device-modal').classList.add('open');
    document.getElementById('manual-mac').focus();

    const scanBtn = document.getElementById('scan-btn');
    if (!cameraAvailable()) {
        scanBtn.disabled = true;
        scanBtn.textContent = 'Scanning needs HTTPS';
    } else if (typeof Html5Qrcode === 'undefined') {
        scanBtn.disabled = true;
        scanBtn.textContent = 'Scanner unavailable offline';
    }
}

function closeModal() {
    document.getElementById('add-device-modal').classList.remove('open');
    stopCamera();
}

function startCamera() {
    if (!cameraAvailable()) {
        say('modal-toast', 'The camera needs HTTPS. Type the MAC instead.', true);
        return;
    }
    if (typeof Html5Qrcode === 'undefined') {
        say('modal-toast', 'Scanner library did not load. Type the MAC instead.', true);
        return;
    }

    if (!html5QrcodeScanner) html5QrcodeScanner = new Html5Qrcode('reader');
    document.getElementById('stop-cam-btn').classList.remove('hidden');

    html5QrcodeScanner.start(
        { facingMode: 'environment' },
        { fps: 10, qrbox: { width: 250, height: 250 } },
        (decodedText) => {
            let mac = decodedText.trim().toUpperCase();
            if (mac.indexOf(':') === -1 && mac.length === 12) {
                mac = mac.match(/.{1,2}/g).join(':');
            }
            document.getElementById('manual-mac').value = mac;
            stopCamera();
        },
        () => { /* scan miss — ignore */ }
    ).catch(err => {
        console.error(err);
        say('modal-toast', 'Camera failed to start.', true);
    });
}

function stopCamera() {
    if (!html5QrcodeScanner) return;
    html5QrcodeScanner.stop()
        .then(() => document.getElementById('stop-cam-btn').classList.add('hidden'))
        .catch(err => console.error(err));
}

async function addManualDevice() {
    const input = document.getElementById('manual-mac');
    const mac = input.value.trim().toUpperCase();

    if (!/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac)) {
        say('modal-toast', 'That is not a MAC address. Expected six pairs like A4:C1:38:00:00:00.', true);
        return;
    }

    try {
        const res = await fetch('/api/devices', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'mac=' + encodeURIComponent(mac)
        });
        if (!res.ok) throw new Error('add failed');

        input.value = '';
        closeModal();
        loadDevices();
    } catch (e) {
        say('modal-toast', 'Could not pair that label.', true);
    }
}
