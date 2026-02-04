document.addEventListener('DOMContentLoaded', () => {
    loadConfig();
    loadSchedule();
    loadDevices();
    loadVersion(); // New
});

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
    } catch (e) {
        console.error("Failed to load config", e);
    }
}

async function searchAddress() {
    const pc = document.getElementById('postcode').value;
    if (!pc) return alert("Please enter a postcode");

    document.getElementById('setup-section').style.opacity = '0.5';

    try {
        const res = await fetch(`/api/lookup/postcode?postcode=${encodeURIComponent(pc)}`);
        if (!res.ok) throw new Error("API Error");
        const json = await res.json();

        // Populate Select
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
        document.getElementById('setup-section').style.opacity = '1';
    }
}

async function saveAddress() {
    const uprn = document.getElementById('address-select').value;
    if (!uprn) return;

    await fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `uprn=${uprn}`
    });

    alert("Saved! Updating schedule...");
    loadConfig();
    refreshData();
}

async function refreshData() {
    await fetch('/api/refresh', { method: 'POST' });
    loadSchedule();
}

async function loadSchedule() {
    const container = document.getElementById('collection-preview');
    try {
        const res = await fetch('/api/schedule');
        if (!res.ok) return;
        const data = await res.json();

        container.innerHTML = '';
        if (!data.param2) {
            container.innerHTML = '<p>No schedule data.</p>';
            return;
        }

        data.param2.forEach(item => {
            const div = document.createElement('div');
            div.className = 'collection-item';

            let color = '#333';
            const service = item.Service.toUpperCase();

            // Colors based on user request / bin type
            if (service.includes("RUBBISH")) color = '#000000'; // Black
            else if (service.includes("RECYCLING")) color = '#2196F3'; // Blue
            else if (service.includes("GARDEN")) color = '#4CAF50'; // Green
            else if (service.includes("FOOD")) color = '#795548'; // Brown

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
// --- Device Management ---


// Loading State Helper
function setLoading(btnId, isLoading, originalText) {
    const btn = typeof btnId === 'string' ? document.getElementById(btnId) : btnId; // Support object or ID
    if (!btn) return;

    if (isLoading) {
        btn.dataset.originalText = btn.innerText;
        btn.innerText = "Loading...";
        btn.disabled = true;
        btn.style.opacity = 0.7;
        // Optionally add a spinner if we had CSS for it
    } else {
        btn.innerText = originalText || btn.dataset.originalText || "OK";
        btn.disabled = false;
        btn.style.opacity = 1;
    }
}

// Wrap existing functions to use loading state
const originalSearch = searchAddress;
searchAddress = async function () {
    const btn = document.querySelector('#setup-section button'); // First button in setup is Search (or give it ID)
    // Actually input group button.
    const btns = document.querySelectorAll('#setup-section button');
    // 0 = Search, 1 = Save.
    setLoading(btns[0], true);
    try { await originalSearch(); }
    catch (e) { console.error(e); }
    finally { setLoading(btns[0], false); }
}

const originalSave = saveAddress;
saveAddress = async function () {
    const btn = document.querySelector('#address-select-group button');
    setLoading(btn, true);
    try { await originalSave(); }
    finally { setLoading(btn, false); }
}

const originalRefreshBtn = refreshData;
// refreshData is already wrapped for hook.
// Let's modify the click handler in HTML or assume standard refreshData is called.
// We should update the HTML to pass 'this' or ID, but for now specific ID matching.
// Button in HTML is: <button onclick="refreshData()">Force Refresh</button>
// Provide an ID in HTML next time, or select by text? 
// Let's assume user calls `refreshData()`.
refreshData = async function () {
    const btn = document.querySelector('#dashboard-section button');
    setLoading(btn, true);
    try {
        await fetch('/api/refresh', { method: 'POST' });
        await loadSchedule();

        // Wait for preview generation (simulated delay or real wait?)
        // Backend generates immediately but BLE takes time? 
        // Preview generation is fast. BLE queue is slow.
        // Wait 2s
        await new Promise(r => setTimeout(r, 2000));

        loadStatus();
        loadPreview();
    } finally {
        setLoading(btn, false);
    }
}


// --- Device Management ---

async function loadDevices() {
    const list = document.getElementById('device-list');
    // Check if loading for first time?
    // list.innerHTML = '<div class="device-item">Loading...</div>'; 
    // Don't wipe list if refreshing status to avoid flicker

    try {
        const res = await fetch('/api/devices');
        const devices = await res.json();
        list.innerHTML = '';

        if (devices.length === 0) {
            list.innerHTML = '<div class="device-item">No devices added. Scan or add one.</div>';
            return;
        }

        // Handle new Object format: [{mac, lastSuccess, lastAttempt, result}]
        // Or legacy Array format (graceful fallback)

        devices.forEach(d => {
            let mac = d;
            let statusHtml = '';

            if (typeof d === 'object') {
                mac = d.mac;

                // Status Logic
                if (d.lastAttempt > 0) {
                    const resultColor = d.result ? '#4CAF50' : '#F44336';
                    const resultIcon = d.result ? '&#10004;' : '&#10008;'; // Check or Cross

                    const time = new Date(d.lastSuccess ? d.lastSuccess * 1000 : d.lastAttempt * 1000);
                    const timeStr = time.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });

                    statusHtml = `
                        <div style="font-size: 0.8rem; color: #aaa; margin-right: 10px; text-align: right;">
                            <span style="color:${resultColor}; font-weight:bold;">${resultIcon}</span>
                            ${d.result ? 'OK' : 'Fail'} <br>
                            ${timeStr}
                        </div>
                    `;

                    // If failed, maybe show last attempt time?
                    if (!d.result && d.lastAttempt) {
                        // Uses lastAttempt time above
                    }
                } else {
                    statusHtml = `<span style="font-size:0.8rem; color:#666; margin-right:10px;">New</span>`;
                }
            }

            const div = document.createElement('div');
            div.className = 'device-item';
            div.innerHTML = `
                <div style="display:flex; flex-direction:column;">
                    <span style="font-weight:600;">${mac}</span>
                </div>
                <div style="display:flex; align-items:center;">
                    ${statusHtml}
                    <div class="actions">
                        <button class="small" onclick="testDevice('${mac}', this)">Test</button>
                        <button class="small danger" onclick="removeDevice('${mac}', this)">Rem</button>
                    </div>
                </div>
            `;
            list.appendChild(div);
        });
    } catch (e) {
        console.error(e);
        list.innerHTML = '<div class="device-item">Failed to load devices</div>';
    }
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
    if (!confirm("Send Test Pattern to " + mac + "?")) return;

    setLoading(btn, true);

    try {
        const res = await fetch('/api/test', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `mac=${encodeURIComponent(mac)}`
        });

        if (res.ok) {
            // alert("Test Pattern Sent!"); // Intrusive alert? Maybe just refresh status
            // Wait a sec then reload preview/status
            setTimeout(() => {
                loadStatus();
                loadDevices(); // To update status icon
            }, 3000); // Give 3s for BLE
        } else {
            alert("Failed to send test: " + await res.text());
        }
    } catch (e) {
        console.error(e);
        alert("Error sending test command");
    } finally {
        setLoading(btn, false, "Test");
    }
}


// Modal & Camera Logic
let html5QrcodeScanner = null;

function openModal() {
    document.getElementById('add-device-modal').style.display = 'block';

    // Check Secure Context for Camera
    const isSecure = location.protocol === 'https:' || location.hostname === 'localhost' || location.hostname === '127.0.0.1';
    const scanBtn = document.querySelector('button[onclick="startCamera()"]');

    if (!isSecure) {
        if (scanBtn) {
            scanBtn.disabled = true;
            scanBtn.innerText = "Scan Unavailable (Requires HTTPS)";
            scanBtn.style.backgroundColor = "#ccc";
            scanBtn.style.color = "#666";
            scanBtn.style.cursor = "not-allowed";
        }
    }
}

function closeModal() {
    document.getElementById('add-device-modal').style.display = 'none';
    stopCamera();
}

function startCamera() {
    // Check if Secure Context (HTTPS or Localhost)
    if (location.protocol !== 'https:' && location.hostname !== 'localhost' && location.hostname !== '127.0.0.1') {
        alert("Camera Scan requires HTTPS.\n\nPlease use Manual Entry.");
        return;
    }

    // Check if library loaded
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
        {
            fps: 10,
            qrbox: { width: 250, height: 250 }
        },
        (decodedText, decodedResult) => {
            console.log(`Scan result: ${decodedText}`);
            let mac = decodedText.trim().toUpperCase();
            if (mac.indexOf(':') === -1 && mac.length === 12) {
                mac = mac.match(/.{1,2}/g).join(':');
            }

            document.getElementById('manual-mac').value = mac;
            stopCamera();
            // alert("Scanned: " + mac); // Annoying
        },
        (errorMessage) => {
            // parse error
        }
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

    // Basic validation?

    await fetch('/api/devices', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `mac=${encodeURIComponent(mac)}`
    });

    closeModal();
    loadDevices();
    alert("Device Added!");
}

async function sendTest(mac) {
    // Implement test logic if needed, or trigger refresh
    alert("Test signal sent to " + mac);
}

// Close modal if clicked outside

// Status Updates
async function loadStatus() {
    try {
        const res = await fetch('/api/status');
        const status = await res.json();
        const last = new Date(status.lastUpdate);
        const next = new Date(status.nextUpdate);

        // Improve Dashboard UI
        const container = document.getElementById('collection-preview');
        let statusDiv = document.getElementById('status-info');
        if (!statusDiv) {
            statusDiv = document.createElement('div');
            statusDiv.id = 'status-info';
            statusDiv.style.marginBottom = '10px';
            statusDiv.style.color = '#888';
            statusDiv.style.fontSize = '0.9rem';
            statusDiv.style.textAlign = 'center';
            container.parentNode.insertBefore(statusDiv, container);
        }

        if (status.lastUpdate === 0) {
            statusDiv.innerHTML = `Not updated yet. <br> Next: ASAP`;
        } else {
            statusDiv.innerHTML = `Last: ${last.toLocaleTimeString()} <br> Next: ${next.toLocaleTimeString()}`;
        }

        // Update WiFi Status Badge
        const wifiBadge = document.getElementById('wifi-status');
        if (wifiBadge) {
            const rssi = status.rssi;
            let quality = "Weak";
            if (rssi > -60) quality = "Excellent";
            else if (rssi > -70) quality = "Good";
            else if (rssi > -80) quality = "Fair";

            wifiBadge.innerText = `${status.ssid} (${quality})`;
            if (quality === "Excellent") wifiBadge.style.backgroundColor = "#4CAF50";
            else if (quality === "Fair") wifiBadge.style.backgroundColor = "#FF9800";
            else wifiBadge.style.backgroundColor = "#F44336";
        }

    } catch (e) {
        console.error(e);
        const wifiBadge = document.getElementById('wifi-status');
        if (wifiBadge) {
            wifiBadge.innerText = "Offline";
            wifiBadge.style.backgroundColor = "#666";
        }
    }
}

// Preview Logic
async function loadPreview() {
    const canvas = document.getElementById('preview-canvas');
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const W = 296;
    const H = 128;
    canvas.width = W;
    canvas.height = H;

    try {
        // Cache Busting: Add timestamp to prevent stale image
        const res = await fetch('/api/preview?ts=' + Date.now());
        if (!res.ok) {
            ctx.fillStyle = '#ddd';
            ctx.fillRect(0, 0, W, H);
            ctx.fillStyle = '#666';
            ctx.fillText("No Preview Available", 80, 60);
            return;
        }

        const blob = await res.blob();
        const buffer = await blob.arrayBuffer();
        const bytes = new Uint8Array(buffer);

        const imgData = ctx.createImageData(W, H);
        const data = imgData.data; // RGBA

        const planeSize = W * H;

        for (let i = 0; i < planeSize; i++) {
            const isBlack = bytes[i];          // 1=Black
            const isRed = bytes[planeSize + i]; // 1=Red

            const p = i * 4;

            if (isRed) {
                data[p] = 255;   // R
                data[p + 1] = 0;   // G
                data[p + 2] = 0;   // B
                data[p + 3] = 255; // A
            } else if (isBlack) {
                data[p] = 0;
                data[p + 1] = 0;
                data[p + 2] = 0;
                data[p + 3] = 255;
            } else {
                // White
                data[p] = 255;
                data[p + 1] = 255;
                data[p + 2] = 255;
                data[p + 3] = 255;
            }
        }
        ctx.putImageData(imgData, 0, 0);

    } catch (e) {
        console.error(e);
    }
}
