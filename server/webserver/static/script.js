let ws;

// Heatmap rendering

function renderHeatmap(pixels, thermistor) {
    const canvas = document.getElementById('heatmap');
    const ctx = canvas.getContext('2d');
    const cellSize = canvas.width / 8;

    // Normalize relative to thermistor (extra credit)
    const minTemp = Math.min(...pixels);
    const maxTemp = Math.max(...pixels);
    const range = maxTemp - minTemp || 1;

    document.getElementById('temp-min').textContent = minTemp.toFixed(1) + '°';
    document.getElementById('temp-max').textContent = maxTemp.toFixed(1) + '°';

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
    ws = new WebSocket(`ws://${window.location.host}/ws`);

    ws.onopen = () => {
        const badge = document.getElementById('status');
        badge.textContent = 'Connected';
        badge.className = 'status-badge connected';
    };

    ws.onclose = () => {
        const badge = document.getElementById('status');
        badge.textContent = 'Disconnected';
        badge.className = 'status-badge disconnected';
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

            document.getElementById('mac').textContent = d.mac_address || '--';
        }
    };

    ws.onerror = () => { };
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

// Init

document.addEventListener('DOMContentLoaded', () => {
    connect();
    loadReadings();
    loadDevices();
});
