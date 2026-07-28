let ws;

// Connection status badge (preserves the .status-dot + .status-text structure)

function setStatus(state, text) {
    const badge = document.getElementById('status');
    if (!badge) return;
    badge.className = 'status-badge ' + state;
    const label = badge.querySelector('.status-text');
    if (label) label.textContent = text;
    else badge.textContent = text;
}

// Arm / disarm switch (camera off switch)

async function setArm(want) {
    reflectArm(want);  // optimistic
    try {
        const res = await fetch('/api/arm', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ armed: want })
        });
        const d = await res.json();
        reflectArm(d.armed);
    } catch (e) {
        console.error('Arm toggle failed:', e);
    }
}

async function loadArmState() {
    try {
        const res = await fetch('/api/arm');
        const d = await res.json();
        reflectArm(d.armed);
    } catch (e) { /* ignore */ }
}

// Reflect armed state across the UI (toggle, label, and live-state box).
function reflectArm(isArmed) {
    const toggle = document.getElementById('arm-toggle');
    if (toggle) toggle.checked = isArmed;

    const label = document.getElementById('arm-label');
    if (label) {
        label.textContent = isArmed ? 'Armed' : 'Disarmed';
        label.className = 'arm-label ' + (isArmed ? 'armed' : 'disarmed');
    }

    document.body.classList.toggle('is-disarmed', !isArmed);

    // When disarmed the device stops sending readings, so show it explicitly.
    if (!isArmed) {
        const pred = document.getElementById('prediction');
        const box = document.getElementById('prediction-box');
        if (pred) pred.textContent = 'DISARMED';
        if (box) box.className = 'info-box prediction-box disarmed';
        setConfidenceMeter(null);
    }
}

// Confidence meter — width tracks probability; color crosses the 0.5 decision
// boundary (below = calm green, above = alert red) to reinforce the prediction.

function setConfidenceMeter(conf) {
    const bar = document.getElementById('confidence-bar');
    if (!bar) return;
    if (conf == null) { bar.style.width = '0%'; bar.className = 'meter-fill'; return; }
    bar.style.width = Math.round(conf * 100) + '%';
    bar.className = 'meter-fill ' + (conf > 0.5 ? 'high' : conf > 0.25 ? 'mid' : 'low');
}

// Heatmap rendering
//
// Renders an 8x8 frame onto a canvas. Defaults to the live-sensor
// canvas, but the playback modal passes its own canvas + label ids so the same
// renderer drives both the live view and event playback.

function renderHeatmap(pixels, thermistor, opts = {}) {
    const canvasId = opts.canvasId || 'heatmap';
    const minId = opts.minId || 'temp-min';
    const maxId = opts.maxId || 'temp-max';

    const canvas = document.getElementById(canvasId);
    const ctx = canvas.getContext('2d');
    const cellSize = canvas.width / 8;

    // Normalize relative to thermistor (extra credit)
    const minTemp = Math.min(...pixels);
    const maxTemp = Math.max(...pixels);
    const range = maxTemp - minTemp || 1;

    const minEl = document.getElementById(minId);
    const maxEl = document.getElementById(maxId);
    if (minEl) minEl.textContent = minTemp.toFixed(1) + '°';
    if (maxEl) maxEl.textContent = maxTemp.toFixed(1) + '°';

    for (let row = 0; row < 8; row++) {
        for (let col = 0; col < 8; col++) {
            const temp = pixels[row * 8 + col];
            // Normalize relative to thermistor: how much above ambient
            const relativeNorm = thermistor
                ? Math.min(1, Math.max(0, (temp - thermistor + 2) / 10))
                : (temp - minTemp) / range;
            const norm = relativeNorm;
            const r = Math.floor(255 * Math.min(1, norm * 2));
            const g = Math.floor(255 * Math.max(0, 1 - Math.abs(norm - 0.5) * 2));
            const b = Math.floor(255 * Math.max(0, 1 - norm * 2));
            ctx.fillStyle = `rgb(${r},${g},${b})`;
            ctx.fillRect(col * cellSize, row * cellSize, cellSize, cellSize);

            // Temperature label on each cell
            ctx.fillStyle = norm > 0.5 ? 'rgba(0,0,0,0.7)' : 'rgba(255,255,255,0.7)';
            ctx.font = `${cellSize * 0.28}px monospace`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(temp.toFixed(1), col * cellSize + cellSize / 2, row * cellSize + cellSize / 2);
        }
    }
}

// WebSocket

function connect() {
    // Use wss:// when the page is served over https, ws:// otherwise.
    const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
    ws = new WebSocket(`${scheme}://${window.location.host}/ws`);

    ws.onopen = () => setStatus('connected', 'Connected');

    ws.onclose = () => {
        setStatus('disconnected', 'Disconnected');
        setTimeout(connect, 1500);
    };

    ws.onmessage = (event) => {
        const msg = JSON.parse(event.data);
        if (msg.type === 'reading') {
            const d = msg.data;
            const thermistor = d.thermistor ?? d.thermistor_temp;
            renderHeatmap(d.pixels, thermistor);

            document.getElementById('thermistor').textContent =
                thermistor != null ? thermistor.toFixed(2) : '--';

            const pred = (d.prediction || '').toUpperCase();
            document.getElementById('prediction').textContent = pred || '--';
            const box = document.getElementById('prediction-box');
            box.className = 'info-box prediction-box ' + (pred === 'PRESENT' ? 'present' : pred === 'EMPTY' ? 'empty' : '');

            document.getElementById('confidence').textContent =
                d.confidence != null ? (d.confidence * 100).toFixed(1) + '%' : '--';
            setConfidenceMeter(d.confidence);

            document.getElementById('mac').textContent = d.mac_address || '--';
        } else if (msg.type === 'event_start') {
            // Instant presence notification at the moment of detection.
            const d = msg.data;
            showEventToast(d);
            loadEvents();
        } else if (msg.type === 'frame') {
            // Live-follow the in-progress event on the main heatmap.
            const d = msg.data;
            if (Array.isArray(d.pixels)) renderHeatmap(d.pixels, d.thermistor);
        } else if (msg.type === 'event_end') {
            loadEvents();
        } else if (msg.type === 'state') {
            // Device confirmed its armed/disarmed state.
            reflectArm(msg.data.armed);
        }
    };

    ws.onerror = () => { };
}

// Live event toast

let toastTimer = null;

function showEventToast(d) {
    const el = document.getElementById('event-toast');
    const conf = d.trigger_confidence != null ? (d.trigger_confidence * 100).toFixed(0) + '%' : '--';
    el.innerHTML = `<strong>Presence detected</strong> · ${d.mac_address || ''} · confidence ${conf}`;
    el.classList.remove('hidden');
    el.classList.add('show');
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
        el.classList.remove('show');
        el.classList.add('hidden');
    }, 8000);
}

// ESP32 commands

async function sendCommand(cmd) {
    const res = await fetch('/api/command', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ command: cmd })
    });
    if (!res.ok) {
        const err = await res.json();
        console.error('Command error:', err);
    }
}

// Readings table

async function loadReadings() {
    const mac = document.getElementById('filter-mac').value.trim();
    const url = mac ? `/api/readings?device_mac=${encodeURIComponent(mac)}` : '/api/readings';
    const res = await fetch(url);
    const rows = await res.json();

    const container = document.getElementById('readings-container');
    if (!rows.length) {
        container.innerHTML = '<p class="empty-msg">No readings found.</p>';
        return;
    }

    const tbody = rows.map(r => {
        const pred = (r.prediction || '').toUpperCase();
        const tagClass = pred === 'PRESENT' ? 'tag-present' : 'tag-empty';
        const pixelPreview = Array.isArray(r.pixels)
            ? r.pixels.slice(0, 4).map(v => v.toFixed(1)).join(', ') + '...'
            : '--';
        return `<tr>
            <td>${r.id}</td>
            <td class="mono">${r.mac_address}</td>
            <td>${r.thermistor_temp != null ? r.thermistor_temp.toFixed(2) : '--'}</td>
            <td><span class="tag ${tagClass}">${pred}</span></td>
            <td>${r.confidence != null ? (r.confidence * 100).toFixed(1) + '%' : '--'}</td>
            <td title="${Array.isArray(r.pixels) ? r.pixels.join(', ') : ''}">${pixelPreview}</td>
            <td>${r.created_at || ''}</td>
            <td><button class="btn-delete" onclick="deleteReading(${r.id})">Delete</button></td>
        </tr>`;
    }).join('');

    container.innerHTML = `<table class="readings-table">
        <thead><tr>
            <th>ID</th><th>MAC</th><th>Thermistor (°C)</th>
            <th>Prediction</th><th>Confidence</th>
            <th>Pixels (preview)</th><th>Time</th><th></th>
        </tr></thead>
        <tbody>${tbody}</tbody>
    </table>`;
}

async function deleteReading(id) {
    await fetch(`/api/readings/${id}`, { method: 'DELETE' });
    loadReadings();
}

function clearFilter() {
    document.getElementById('filter-mac').value = '';
    loadReadings();
}

// Devices table

async function loadDevices() {
    const res = await fetch('/api/devices');
    const devices = await res.json();

    const container = document.getElementById('devices-container');
    if (!devices.length) {
        container.innerHTML = '<p class="empty-msg">No devices registered yet.</p>';
        return;
    }

    const rows = devices.map(d =>
        `<tr><td>${d.id}</td><td class="mono">${d.mac_address}</td><td>${d.created_at}</td></tr>`
    ).join('');

    container.innerHTML = `<table class="devices-table">
        <thead><tr><th>ID</th><th>MAC Address</th><th>First Seen</th></tr></thead>
        <tbody>${rows}</tbody>
    </table>`;
}

// Events table

function fmtDuration(started, ended) {
    if (!started || !ended) return '--';
    const s = (new Date(ended.replace(' ', 'T')) - new Date(started.replace(' ', 'T'))) / 1000;
    if (isNaN(s) || s < 0) return '--';
    const m = Math.floor(s / 60), sec = Math.round(s % 60);
    return `${m}m ${sec}s`;
}

// At-a-glance metrics above the events table.
function renderEventSummary(rows) {
    const el = document.getElementById('events-summary');
    if (!el) return;
    const total = rows.length;
    const active = rows.filter(e => !e.ended_at).length;
    const falseAlarms = rows.filter(e => e.false_alarm).length;
    const valid = total - falseAlarms;
    const cards = [
        { label: 'Total events', value: total, cls: '' },
        { label: 'Valid', value: valid, cls: 'ok' },
        { label: 'False alarms', value: falseAlarms, cls: 'warn' },
        { label: 'Recording now', value: active, cls: active ? 'live' : '' },
    ];
    el.innerHTML = cards.map(c =>
        `<div class="metric ${c.cls}"><span class="metric-value">${c.value}</span>` +
        `<span class="metric-label">${c.label}</span></div>`
    ).join('');
}

async function loadEvents() {
    const mac = document.getElementById('filter-event-mac').value.trim();
    const url = mac ? `/api/events?device_mac=${encodeURIComponent(mac)}` : '/api/events';
    const res = await fetch(url);
    const rows = await res.json();

    renderEventSummary(rows);

    const container = document.getElementById('events-container');
    if (!rows.length) {
        container.innerHTML = '<p class="empty-msg">No events recorded yet. Walk in front of the sensor to trigger one.</p>';
        return;
    }

    const body = rows.map(e => {
        const conf = e.trigger_confidence != null ? (e.trigger_confidence * 100).toFixed(1) + '%' : '--';
        const ongoing = !e.ended_at;
        const dur = ongoing ? '<em>recording…</em>' : fmtDuration(e.started_at, e.ended_at);
        const faBadge = e.false_alarm
            ? '<span class="tag tag-false-alarm">FALSE ALARM</span>'
            : '<span class="tag tag-valid">valid</span>';
        const faLabel = e.false_alarm ? 'Unmark' : 'Mark false alarm';
        return `<tr>
            <td>${e.id}</td>
            <td class="mono">${e.mac_address}</td>
            <td>${e.started_at || ''}</td>
            <td>${dur}</td>
            <td>${conf}</td>
            <td>${e.frame_count}</td>
            <td>${faBadge}</td>
            <td class="event-actions">
                <button onclick="playEvent(${e.id}, '${e.event_uid}')" ${e.frame_count ? '' : 'disabled'}>Play</button>
                <button onclick="toggleFalseAlarm(${e.id}, ${e.false_alarm ? 'true' : 'false'})">${faLabel}</button>
                <button class="btn-delete" onclick="deleteEvent(${e.id})">Delete</button>
            </td>
        </tr>`;
    }).join('');

    container.innerHTML = `<table class="readings-table">
        <thead><tr>
            <th>ID</th><th>MAC</th><th>Started</th><th>Duration</th>
            <th>Trigger conf.</th><th>Frames</th><th>Status</th><th></th>
        </tr></thead>
        <tbody>${body}</tbody>
    </table>`;
}

function clearEventFilter() {
    document.getElementById('filter-event-mac').value = '';
    loadEvents();
}

async function toggleFalseAlarm(id, current) {
    await fetch(`/api/events/${id}/false_alarm`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ false_alarm: !current })
    });
    loadEvents();
}

async function deleteEvent(id) {
    await fetch(`/api/events/${id}`, { method: 'DELETE' });
    loadEvents();
}

// Event playback

let pbFrames = [];
let pbIndex = 0;
let pbTimer = null;
let pbPlaying = false;

async function playEvent(id, uid) {
    const res = await fetch(`/api/events/${id}/frames`);
    pbFrames = await res.json();
    if (!pbFrames.length) return;

    document.getElementById('playback-title').textContent = `Event #${id} — ${pbFrames.length} frames`;
    document.getElementById('playback-modal').classList.remove('hidden');

    const scrub = document.getElementById('pb-scrub');
    scrub.max = pbFrames.length - 1;
    scrub.value = 0;

    pbIndex = 0;
    showFrame(0);
    startPlay();
}

function showFrame(i) {
    const f = pbFrames[i];
    if (!f) return;
    renderHeatmap(f.pixels, f.thermistor, {
        canvasId: 'playback-heatmap', minId: 'pb-temp-min', maxId: 'pb-temp-max'
    });
    document.getElementById('pb-frame-label').textContent = `${i + 1} / ${pbFrames.length}`;
    document.getElementById('pb-scrub').value = i;
}

function startPlay() {
    pbPlaying = true;
    document.getElementById('pb-play').textContent = 'Pause';
    if (pbTimer) clearInterval(pbTimer);
    // Playback at capture rate (1 fps).
    pbTimer = setInterval(() => {
        pbIndex++;
        if (pbIndex >= pbFrames.length) { pbIndex = pbFrames.length - 1; stopPlay(); return; }
        showFrame(pbIndex);
    }, 1000);
}

function stopPlay() {
    pbPlaying = false;
    document.getElementById('pb-play').textContent = 'Play';
    if (pbTimer) { clearInterval(pbTimer); pbTimer = null; }
}

function togglePlay() {
    if (pbPlaying) { stopPlay(); return; }
    if (pbIndex >= pbFrames.length - 1) pbIndex = 0;  // replay from start
    showFrame(pbIndex);
    startPlay();
}

function scrubTo(v) {
    stopPlay();
    pbIndex = parseInt(v, 10);
    showFrame(pbIndex);
}

function closePlayback() {
    stopPlay();
    document.getElementById('playback-modal').classList.add('hidden');
}

// Init

document.addEventListener('DOMContentLoaded', () => {
    connect();
    loadArmState();
    loadReadings();
    loadDevices();
    loadEvents();
});
