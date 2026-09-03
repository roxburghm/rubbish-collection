let pollInterval = null;
let isPolling = false;
let lastKnownUpdate = 0;

document.addEventListener('DOMContentLoaded', () => {
    loadConfig();
    loadSchedule();
    loadDevices();
    loadVersion();
    loadStatus();
    startPolling();

    // Close modal if clicked outside content
    document.getElementById('add-device-modal').addEventListener('click', (e) => {
        if (e.target.id === 'add-device-modal') closeModal();
    });
});

function startPolling() {
    if (pollInterval) return;
    pollInterval = setInterval(pollStatus, 5000);
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
            const status = await statusRes.json();
            updateStatusUI(status);

            // Reload schedule if server data has changed
            if (status.lastUpdate !== lastKnownUpdate && status.lastUpdate > 0) {
                lastKnownUpdate = status.lastUpdate;
                loadSchedule();
            }
        }

        if (devicesRes.ok) {
            const devices = await devicesRes.json();
            renderDevices(devices);
        }
    } catch (e) {
        // Silently fail - network may be temporarily unavailable
    } finally {
        isPolling = false;
    }
}

async function loadVersion() {
    try {
        const res = await fetch('/api/version');
        const data = await res.json();
        document.getElementById('fw-version').innerText = data.version;
    } catch (e) {
        document.getElementById('fw-version').innerText = "Unknown (API Fail)";
    }
}

async function loadConfig() {
    try {
        const res = await fetch('/api/config');
        if (!res.ok) return;
        const data = await res.json();
        document.getElementById('current-uprn').innerText = data.uprn || 'Not Set';

        const display = data.propertyName ? data.propertyName : (data.uprn || 'Not Set');
        document.getElementById('current-address').innerText = display;
    } catch (e) {
        console.error("Failed to load config", e);
    }
}

async function searchAddress() {
    const pc = document.getElementById('postcode').value;
    if (!pc) return alert("Please enter a postcode");

    const btn = document.querySelector('#setup-section .input-group button');
    setLoading(btn, true);

    try {
        const res = await fetch(`/api/lookup/postcode?postcode=${encodeURIComponent(pc)}`);
        if (!res.ok) throw new Error("API Error");
        const json = await res.json();

        const select = document.getElementById('address-select');
        select.innerHTML = '';
        const group = document.getElementById('address-select-group');

        if (json.param2 && Array.isArray(json.param2)) {
            json.param2.forEach(addr => {
                const opt = document.createElement('option');
                opt.value = addr.Value;
                opt.innerText = addr.Text;
                select.appendChild(opt);
            });
            group.style.display = 'block';
        } else {
            alert("No addresses found");
        }
    } catch (e) {
        console.error(e);
        alert("Failed to find postcode");
    } finally {
        setLoading(btn, false);
    }
}

async function saveAddress() {
    const selector = document.getElementById('address-select');
    const uprn = selector.value;
    if (!uprn) return;

    const propertyName = selector.options[selector.selectedIndex].text;

    const btn = document.querySelector('#address-select-group button');
    setLoading(btn, true);

    try {
        await fetch('/api/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `uprn=${uprn}&propertyName=${encodeURIComponent(propertyName)}`
        });

        alert("Saved! Schedule will update shortly...");
        loadConfig();
        // Request a refresh - polling will pick up the new data
        await fetch('/api/refresh', { method: 'POST' });
    } finally {
        setLoading(btn, false);
    }
}

async function refreshData() {
    const btn = document.querySelector('#dashboard-section button');
    setLoading(btn, true);

    try {
        await fetch('/api/refresh', { method: 'POST' });
        // Polling will automatically pick up new schedule, device status, and timestamps
    } finally {
        setLoading(btn, false);
    }
}

async function loadSchedule() {
    const container = document.getElementById('collection-preview');
    try {
        const res = await fetch('/api/schedule');
        if (!res.ok) return;
        const data = await res.json();

        container.innerHTML = '';
        if (!data || !data.param2) {
            container.innerHTML = '<p class="muted centered">No schedule data (or loading...).</p>';
            return;
        }

        // Sort: Rubbish, Recycling, Food, Garden
        function binPriority(service) {
            const s = service.toUpperCase();
            if (s.includes('RUBBISH')) return 0;
            if (s.includes('RECYCLING')) return 1;
            if (s.includes('FOOD')) return 2;
            if (s.includes('GARDEN')) return 3;
            return 4;
        }
        data.param2.sort((a, b) => binPriority(a.Service) - binPriority(b.Service));

        // Normalize comma-separated dates (API returns e.g. "23/03/2026,23/03/2026" for garden)
        data.param2.forEach(item => {
            if (item.collectionDateString && item.collectionDateString.includes(',')) {
                item.collectionDateString = item.collectionDateString.split(',')[0].trim();
            }
        });

        // Deduplicate by Service name (API sometimes returns duplicates)
        const seen = new Set();
        const dedupedItems = data.param2.filter(item => {
            const key = item.Service.toUpperCase();
            if (seen.has(key)) return false;
            seen.add(key);
            return true;
        });

        dedupedItems.forEach(item => {
            const div = document.createElement('div');
            div.className = 'collection-item';

            let color = '#333';
            const service = item.Service.toUpperCase();

            if (service.includes("RUBBISH")) color = '#000000';
            else if (service.includes("RECYCLING")) color = '#2196F3';
            else if (service.includes("GARDEN")) color = '#4CAF50';
            else if (service.includes("FOOD")) color = '#795548';

            const iconColor = (color === '#000000') ? '#444' : color;

            div.innerHTML = `
                <div class="bin-icon" style="color: ${iconColor};">
                     <svg width="32" height="32"><use href="#icon-bin"></use></svg>
                </div>
                <div class="bin-details">
                    <h3>${item.Service}</h3>
                    <p>${item.collectionDateString}</p>
                </div>
            `;
            container.appendChild(div);
        });
    } catch (e) {
        console.error(e);
    }
}

// --- Device Management ---

function setLoading(btn, isLoading, originalText) {
    if (typeof btn === 'string') btn = document.getElementById(btn);
    if (!btn) return;

    if (isLoading) {
        btn.dataset.originalText = btn.innerText;
        btn.innerText = "Loading...";
        btn.disabled = true;
        btn.classList.add('loading');
    } else {
        btn.innerText = originalText || btn.dataset.originalText || "OK";
        btn.disabled = false;
        btn.classList.remove('loading');
    }
}

async function loadDevices() {
    const list = document.getElementById('device-list');

    try {
        const res = await fetch('/api/devices');
        const devices = await res.json();
        renderDevices(devices);
    } catch (e) {
        console.error(e);
        list.innerHTML = '<div class="device-item">Failed to load devices</div>';
    }
}

function renderDevices(devices) {
    const list = document.getElementById('device-list');
    list.innerHTML = '';

    if (devices.length === 0) {
        list.innerHTML = '<div class="device-item"><span>No devices added. Scan or add one.</span></div>';
        return;
    }

    devices.forEach(d => {
        let mac = d;
        let statusHtml = '';
        let rotate = false;
        let pending = false;
        let retries = 0;

        if (typeof d === 'object') {
            mac = d.mac || d;
            rotate = d.rotate === true;
            pending = d.pending === true;
            retries = d.retries || 0;

            const lastSuccess = d.lastSuccess > 0 ? new Date(d.lastSuccess * 1000).toLocaleString() : 'Never';
            const lastAttempt = d.lastAttempt > 0 ? new Date(d.lastAttempt * 1000).toLocaleString() : 'Never';
            const lastSeen = d.lastSeen > 0 ? new Date(d.lastSeen * 1000).toLocaleString() : 'Never';

            if (d.result) {
                statusHtml = `<span class="status-ok">OK (${lastSuccess})</span>`;
            } else if (d.lastAttempt > 0) {
                statusHtml = `<span class="status-fail">Fail (${lastAttempt})</span>`;
            } else {
                statusHtml = `<span class="status-new">New</span>`;
            }

            statusHtml += ` <span class="status-seen">Seen: ${lastSeen}</span>`;

            if (pending) {
                statusHtml += ` <span class="status-pending-badge">Update Pending</span>`;
            }

            if (pending && retries > 0) {
                statusHtml += ` <span class="status-retry">[Retry ${retries}...]</span>`;
            }
        } else {
            mac = d;
        }

        const rotTitle = rotate ? "Display: Inverted (180\u00B0)" : "Display: Normal (0\u00B0)";
        const rotClass = rotate ? "rotate-btn rotated" : "rotate-btn";

        let itemClass = 'device-item';
        if (pending) itemClass += ' pending-update';

        const div = document.createElement('div');
        div.className = itemClass;
        div.innerHTML = `
            <div class="device-info">
                <span class="device-mac">${mac}</span>
                <small>${statusHtml}</small>
            </div>
            <div class="device-controls">
                <button onclick="toggleRotation('${mac}', ${!rotate})"
                        class="${rotClass}"
                        title="${rotTitle}">
                    <svg width="24" height="24">
                        <use href="#icon-id-card"></use>
                    </svg>
                </button>
                <div class="actions">
                    <select id="test-type-${mac}" style="font-size: 0.8em; padding: 2px;">
                        <option value="pattern">Pattern</option>
                        <option value="hello">Hello</option>
                        <option value="wifi">No WiFi</option>
                        <option value="api">API Err</option>
                    </select>
                    <button class="small" onclick="testDevice('${mac}', this)">Test</button>
                    <button class="small danger" onclick="removeDevice('${mac}', this)">Rem</button>
                </div>
            </div>
        `;
        list.appendChild(div);
    });
}

async function removeDevice(mac, btn) {
    if (!confirm('Remove ' + mac + '?')) return;
    setLoading(btn, true);
    try {
        await fetch(`/api/devices?mac=${encodeURIComponent(mac)}`, { method: 'DELETE' });
        loadDevices();
    } catch (e) {
        alert("Remove failed");
    }
}

async function testDevice(mac, btn) {
    const selector = document.getElementById('test-type-' + mac);
    const type = selector ? selector.value : 'pattern';
    if (!confirm("Send " + type + " test to " + mac + "?")) return;
    setLoading(btn, true);

    try {
        const res = await fetch('/api/test', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `mac=${encodeURIComponent(mac)}&type=${encodeURIComponent(type)}`
        });

        if (!res.ok) {
            alert("Failed to queue test: " + await res.text());
        }
        // Polling will pick up the result automatically
    } catch (e) {
        console.error(e);
        alert("Error sending test command");
    } finally {
        setLoading(btn, false, "Test");
    }
}

async function toggleRotation(mac, newRot) {
    if (!confirm(`Rotate display for ${mac} to ${newRot ? "180\u00B0" : "0\u00B0"}?`)) return;

    try {
        const res = await fetch('/api/device/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ mac: mac, rotate: newRot })
        });

        if (!res.ok) {
            alert("Failed to update rotation.");
        }
        // Polling will pick up the change
    } catch (e) {
        console.error(e);
        alert("Error updating rotation.");
    }
}

// --- Modal & Camera ---

let html5QrcodeScanner = null;

function openModal() {
    document.getElementById('add-device-modal').style.display = 'block';

    const isSecure = location.protocol === 'https:' || location.hostname === 'localhost' || location.hostname === '127.0.0.1';
    const scanBtn = document.querySelector('button[onclick="startCamera()"]');

    if (!isSecure && scanBtn) {
        scanBtn.disabled = true;
        scanBtn.innerText = "Scan Unavailable (Requires HTTPS)";
        scanBtn.classList.add('disabled');
    }
}

function closeModal() {
    document.getElementById('add-device-modal').style.display = 'none';
    stopCamera();
}

function startCamera() {
    if (location.protocol !== 'https:' && location.hostname !== 'localhost' && location.hostname !== '127.0.0.1') {
        alert("Camera Scan requires HTTPS.\n\nPlease use Manual Entry.");
        return;
    }

    if (typeof Html5Qrcode === 'undefined') {
        alert("Scanner library not loaded. Check internet connection?");
        return;
    }

    if (!html5QrcodeScanner) {
        html5QrcodeScanner = new Html5Qrcode("reader");
    }
    document.getElementById('stop-cam-btn').style.display = 'inline-block';

    html5QrcodeScanner.start(
        { facingMode: "environment" },
        { fps: 10, qrbox: { width: 250, height: 250 } },
        (decodedText) => {
            let mac = decodedText.trim().toUpperCase();
            if (mac.indexOf(':') === -1 && mac.length === 12) {
                mac = mac.match(/.{1,2}/g).join(':');
            }
            document.getElementById('manual-mac').value = mac;
            stopCamera();
        },
        () => { /* scan miss - ignore */ }
    ).catch(err => {
        console.error(err);
        alert("Camera failed to start: " + err);
    });
}

function stopCamera() {
    if (html5QrcodeScanner) {
        html5QrcodeScanner.stop().then(() => {
            document.getElementById('stop-cam-btn').style.display = 'none';
        }).catch(err => console.error(err));
    }
}

async function addManualDevice() {
    const mac = document.getElementById('manual-mac').value;
    if (!mac) return;

    await fetch('/api/devices', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `mac=${encodeURIComponent(mac)}`
    });

    closeModal();
    loadDevices();
    alert("Device Added!");
}

// --- Status Updates ---

async function loadStatus() {
    try {
        const res = await fetch('/api/status');
        const status = await res.json();
        updateStatusUI(status);
    } catch (e) {
        console.error(e);
        const indicator = document.getElementById('wifi-status');
        if (indicator) {
            indicator.title = 'Offline';
            indicator.querySelectorAll('.wifi-bar').forEach(bar => {
                bar.style.fill = '#333';
            });
        }
    }
}

function updateStatusUI(status) {
    const last = new Date(status.lastUpdate);
    const next = new Date(status.nextUpdate);

    const container = document.getElementById('collection-preview');
    let statusDiv = document.getElementById('status-info');
    if (!statusDiv) {
        statusDiv = document.createElement('div');
        statusDiv.id = 'status-info';
        statusDiv.className = 'status-info';
        container.parentNode.insertBefore(statusDiv, container);
    }

    if (status.lastUpdate === 0) {
        statusDiv.innerHTML = `Not updated yet. <br> Next: ASAP`;
    } else {
        statusDiv.innerHTML = `Last: ${last.toLocaleTimeString()} <br> Next: ${next.toLocaleTimeString()}`;
    }

    const indicator = document.getElementById('wifi-status');
    if (indicator) {
        const rssi = status.rssi;
        let bars = 0;
        let color = '#666';
        let quality = 'Offline';

        if (rssi > -60) { bars = 4; color = '#4CAF50'; quality = 'Excellent'; }
        else if (rssi > -70) { bars = 3; color = '#4CAF50'; quality = 'Good'; }
        else if (rssi > -80) { bars = 2; color = '#FF9800'; quality = 'Fair'; }
        else if (rssi < 0) { bars = 1; color = '#F44336'; quality = 'Weak'; }

        indicator.title = status.ssid ? `${status.ssid} (${quality})` : quality;

        indicator.querySelectorAll('.wifi-bar').forEach(bar => {
            const barNum = parseInt(bar.dataset.bar);
            bar.style.fill = barNum <= bars ? color : '#333';
        });
    }
}
