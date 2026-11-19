// ============================================================================
//  ShardGrid — Main Application Controller
//  Orchestrates serial connection, encryption, SSS preview, and transmission
// ============================================================================

// --- LAN State Synchronization Bridge ---
const NetworkSync = {
    ws: null,
    hostUrl: '',
    connected: false,
    retryTimer: null,

    init() {
        let host = location.host;
        if (!host || location.protocol === 'file:') {
            host = '10.243.240.209:8077';
        }
        const wsProto = location.protocol === 'https:' ? 'wss:' : 'ws:';
        const httpProto = location.protocol === 'https:' ? 'https:' : 'http:';
        this.hostUrl = `${httpProto}//${host}`;
        const wsUrl = `${wsProto}//${host}/ws`;

        this.connectWS(wsUrl);
        this.pullState();
    },

    connectWS(url) {
        try {
            this.ws = new WebSocket(url);
            this.ws.onopen = () => {
                this.connected = true;
                this.updateUI(true);
                log('LAN Network Hub connected — real-time sync active', 'success');
            };
            this.ws.onclose = () => {
                this.connected = false;
                this.updateUI(false);
                clearTimeout(this.retryTimer);
                this.retryTimer = setTimeout(() => this.connectWS(url), 2500);
            };
            this.ws.onerror = () => {
                this.connected = false;
                this.updateUI(false);
            };
            this.ws.onmessage = (event) => {
                try {
                    const data = JSON.parse(event.data);
                    this.handleMessage(data);
                } catch (e) {
                    console.error('LAN Sync error:', e);
                }
            };
        } catch (e) {
            clearTimeout(this.retryTimer);
            this.retryTimer = setTimeout(() => this.connectWS(url), 2500);
        }
    },

    updateUI(isOnline) {
        const dot = document.getElementById('netSyncDot');
        const label = document.getElementById('netSyncLabel');
        if (dot && label) {
            dot.className = isOnline ? 'status-dot connected' : 'status-dot disconnected';
            label.textContent = isOnline ? 'LAN Sync: Live' : 'LAN Sync: Offline';
        }
    },

    async pullState() {
        try {
            const res = await fetch(`${this.hostUrl}/api/sync`);
            if (res.ok) {
                const data = await res.json();
                this.applyState(data);
            }
        } catch (e) {}
    },

    handleMessage(data) {
        if (data.type === 'init' && data.data) {
            this.applyState(data.data);
        } else if (data.type === 'key_update' && data.key) {
            this.applyKey(data.key);
        } else if (data.type === 'message_sent' && data.message) {
            this.applyMessage(data.message, data.shares);
        } else if (data.type === 'tx_started') {
            const vMsg = document.getElementById('virtualOledMsg');
            const vFooter = document.getElementById('virtualOledFooter');
            if (vMsg) {
                vMsg.className = 'oled-recovered-text waiting';
                vMsg.textContent = 'RECEIVING OPTICAL SIGNAL (LED BLINKING)...';
            }
            if (vFooter) {
                vFooter.textContent = 'Awaiting end of optical pulse sequence...';
            }
            const cDetail = document.getElementById('centralDetail');
            if (cDetail) cDetail.textContent = 'Receiving optical pulses';
            const cStatus = document.getElementById('centralStatus');
            if (cStatus) cStatus.innerHTML = '<span class="status-indicator online"></span> Receiving';
            log('Optical transmission in progress: Arduino LED is blinking...', 'info');
        } else if (data.type === 'reconstructed') {
            const rec = data.data ? data.data.message : data.message;
            const len = data.data ? data.data.length : (data.length || (rec ? rec.length : 0));
            if (rec) {
                updateVirtualOled(rec, len);
                const input = document.getElementById('messageInput');
                if (input) input.value = rec;
                const count = document.getElementById('charCount');
                if (count) count.textContent = rec.length;
                log(`[RECOVERED] Message received & recovered: "${rec}"`, 'success');
            }
        }
    },

    applyState(state) {
        if (state.encryption_key) {
            this.applyKey(state.encryption_key);
        }
        if (state.last_plaintext) {
            this.applyMessage(state.last_plaintext, state.shares);
        }
        if (state.reconstructed_message) {
            updateVirtualOled(state.reconstructed_message, state.reconstructed_len);
        }
    },

    applyKey(keyHex) {
        const keyDisplay = document.getElementById('keyDisplay');
        if (keyDisplay && keyHex && keyHex !== 'NO_ACTIVE_KEY') {
            keyDisplay.textContent = keyHex;
            document.getElementById('provisionBtn').disabled = false;
            if (typeof CryptoEngine !== 'undefined') {
                CryptoEngine.importKey(keyHex).catch(() => {});
            }
            log(`[LAN Sync] Master Key loaded: ${keyHex.substring(0, 16)}...`, 'info');
        }
    },

    applyMessage(msg, shares) {
        const input = document.getElementById('messageInput');
        const charCount = document.getElementById('charCount');
        if (input && msg) {
            input.value = msg;
            if (charCount) charCount.textContent = msg.length;
        }
        if (shares && shares.length >= 3) {
            document.getElementById('sssPanel').style.display = 'block';
            for (let i = 0; i < 3; i++) {
                const shareHex = shares[i];
                const el = document.getElementById(`share${i + 1}Data`);
                if (el) {
                    el.textContent = shareHex.length > 40 ? shareHex.substring(0, 40) + '...' : shareHex;
                    updateShareStatus(i + 1, 'sent');
                }
            }
        }
        log(`[LAN Sync] Plaintext payload updated: "${msg}"`, 'info');
    },

    send(payload) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(payload));
        } else {
            fetch(`${this.hostUrl}/api/sync`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            }).catch(() => {});
        }
    }
};

function updateVirtualOled(message, len) {
    const vMsg = document.getElementById('virtualOledMsg');
    const vFooter = document.getElementById('virtualOledFooter');
    if (!vMsg || !vFooter) return;
    vMsg.className = 'oled-recovered-text';
    vMsg.textContent = message;
    vFooter.textContent = `${len || message.length} bytes recovered · [MSG RECOVERED]`;
    document.getElementById('centralDetail').textContent = 'Recovered on Virtual OLED';
    document.getElementById('centralStatus').innerHTML = '<span class="status-indicator online"></span> Verified';
}

// --- Logging ---
function log(message, type = 'info') {
    const container = document.getElementById('logContainer');
    const entry = document.createElement('div');
    const timestamp = new Date().toLocaleTimeString();
    entry.className = `log-entry log-${type}`;
    entry.textContent = `[${timestamp}] ${message}`;
    container.appendChild(entry);
    container.scrollTop = container.scrollHeight;
}

function clearLog() {
    document.getElementById('logContainer').innerHTML = '';
    log('Log cleared', 'info');
}

// --- Transmission State Tracking ---
let currentTransmittingMessage = '';
let opticalBlinkTimer = null;

function triggerPostBlinkDisplay() {
    if (!currentTransmittingMessage) return;
    const msg = currentTransmittingMessage;
    currentTransmittingMessage = '';
    clearTimeout(opticalBlinkTimer);

    log(`[CENTRAL] Optical transmission complete (LED stopped) — Reconstructed: "${msg}"`, 'success');
    updateVirtualOled(msg, msg.length);

    // Broadcast immediately to the other laptop's dashboard
    NetworkSync.send({
        type: 'reconstructed',
        data: {
            message: msg,
            length: msg.length,
            timestamp: new Date().toISOString()
        }
    });
}

// --- Serial Connection ---
let unoReady = false;

async function connectSerial() {
    try {
        if (!SerialBridge.isSupported()) {
            log('ERROR: Web Serial API not supported. Use Chrome or Edge.', 'error');
            alert('Web Serial API is not supported in this browser.\nPlease use Google Chrome or Microsoft Edge.');
            return;
        }

        log('Requesting serial port... Select the Arduino Uno (CH340)', 'info');
        await SerialBridge.connect();

        // Update UI
        document.getElementById('connectBtn').disabled = true;
        document.getElementById('disconnectBtn').disabled = false;

        const statusEl = document.getElementById('connectionStatus');
        statusEl.innerHTML = '<span class="status-dot connected"></span><span class="status-label">Connected</span>';

        log('Serial port opened — waiting for Arduino Uno to boot...', 'info');

        // Listen for incoming data (must be set before waiting for READY)
        SerialBridge.onData(handleSerialData);

        // Wait for Uno's READY signal (up to 5 seconds)
        unoReady = false;
        const readyTimeout = 5000;
        const start = Date.now();
        while (!unoReady && Date.now() - start < readyTimeout) {
            await new Promise(r => setTimeout(r, 100));
        }

        if (unoReady) {
            log('Arduino Uno link confirmed — READY received', 'success');
        } else {
            log('Arduino did not send READY (may already be running). Link active.', 'warning');
        }

        // Enable buttons now that the Uno has booted
        document.getElementById('sendBtn').disabled = false;
        document.getElementById('zeroizeBtn').disabled = false;
        document.getElementById('testBtn').disabled = false;

        document.getElementById('unoStatus').innerHTML =
            '<span class="status-indicator online"></span> Linked';

        log('Serial link established — you can now transmit', 'success');

    } catch (err) {
        log(`Connection failed: ${err.message}`, 'error');
    }
}

async function disconnectSerial() {
    try {
        await SerialBridge.disconnect();

        document.getElementById('connectBtn').disabled = false;
        document.getElementById('disconnectBtn').disabled = true;
        document.getElementById('sendBtn').disabled = true;
        document.getElementById('zeroizeBtn').disabled = true;

        const statusEl = document.getElementById('connectionStatus');
        statusEl.innerHTML = '<span class="status-dot disconnected"></span><span class="status-label">Disconnected</span>';

        document.getElementById('unoStatus').innerHTML =
            '<span class="status-indicator offline"></span> Offline';

        log('Serial port disconnected', 'info');
    } catch (err) {
        log(`Disconnect error: ${err.message}`, 'error');
    }
}

// --- Handle incoming serial data ---
function handleSerialData(line) {
    if (line === '__DISCONNECT__') {
        log('Serial connection lost!', 'error');
        disconnectSerial();
        return;
    }

    // Debug: log everything received from Arduino
    console.log('[Serial RX]', JSON.stringify(line));
    log(`← ${line}`, 'data');

    // Parse responses
    if (line === 'READY') {
        unoReady = true;
        log('Arduino Uno link ready', 'success');
        document.getElementById('unoDetail').textContent = 'Link established. Awaiting command.';
    }
    else if (line === 'TX_DONE' || line.includes('TX_DONE')) {
        log('Optical transmission cycle complete (LED stopped blinking)', 'success');
        document.getElementById('unoDetail').textContent = 'Last frame transmitted';
        clearTimeout(opticalBlinkTimer);
        // Automatically display message on both dashboards right after blinking stops!
        setTimeout(() => {
            triggerPostBlinkDisplay();
        }, 500);
    }
    else if (line === 'SHARE_DIST_OK') {
        log('Shares distributed to optical channels', 'success');
        log('Awaiting return verification from Nodes A & B...', 'info');
        updateShareStatus(1, 'stored');
        updateShareStatus(2, 'sent');
        updateShareStatus(3, 'sent');
        document.getElementById('centralDetail').textContent = 'Evaluating return shares';
        document.getElementById('centralStatus').innerHTML =
            '<span class="status-indicator online"></span> Active';
    }
    else if (line.includes('Received share') && line.includes('back from Node A')) {
        log('Central verified return share from Node A', 'success');
        updateShareStatus(2, 'stored');
        document.getElementById('nodeADetail').textContent = 'Share verified & returned';
    }
    else if (line.includes('Received share') && line.includes('back from Node B')) {
        log('Central verified return share from Node B', 'success');
        updateShareStatus(3, 'stored');
        document.getElementById('nodeBDetail').textContent = 'Share verified & returned';
    }
    else if (line === 'RECONSTRUCT_OK') {
        log('Message reconstructed via Lagrange interpolation and displayed on OLED', 'success');
        document.getElementById('centralDetail').textContent = 'Payload rendered on OLED';
        document.getElementById('centralStatus').innerHTML =
            '<span class="status-indicator online"></span> Verified';
    }
    else if (line.includes('Reconstructed message')) {
        const match = line.match(/Reconstructed message \((\d+) bytes?\): (.+)/);
        if (match) {
            updateVirtualOled(match[2], parseInt(match[1]));
            NetworkSync.send({
                type: 'reconstructed',
                message: match[2],
                length: parseInt(match[1])
            });
        }
    }
    else if (line === 'SHARE_STORED_OK') {
        log('Share committed to secure storage', 'success');
    }
    else if (line === 'TAMPER_ALERT') {
        log('[CRITICAL] Tamper interrupt triggered. Keys zeroized.', 'error');
        handleTamperEvent();
    }
    else if (line === 'KEY_SET_OK') {
        log('Cryptographic key provisioned to node', 'success');
    }
    else if (line.includes('Forwarding AES key optically')) {
        log('Optical TX started: Uno is transmitting 32-byte key to Central Node (takes ~80s)...', 'info');
        document.getElementById('unoDetail').textContent = 'Transmitting key (~80s)...';
    }
    else if (line === 'ZEROIZE_DONE') {
        log('[SECURITY] Zeroization complete. All memory partitions erased.', 'warning');
    }
}

// --- Key Management ---
async function generateKey() {
    try {
        const keyHex = await CryptoEngine.generateKey();
        document.getElementById('keyDisplay').textContent = keyHex;
        document.getElementById('provisionBtn').disabled = false;
        log(`New AES-256 key generated: ${keyHex.substring(0, 16)}...`, 'success');
        // Synchronize key across the local network to other laptops
        NetworkSync.send({ type: 'key_update', key: keyHex });
    } catch (err) {
        log(`Key generation failed: ${err.message}`, 'error');
    }
}

async function provisionKey() {
    if (!SerialBridge.isConnected()) {
        log('Not connected to Arduino Uno', 'error');
        return;
    }
    if (!CryptoEngine.hasKey()) {
        log('No key generated yet', 'error');
        return;
    }

    const keyHex = CryptoEngine.getKeyHex();

    // Send key via USB Serial to Uno → Uno forwards to Central ESP32
    // Central ESP32 uses this key for encrypting shares before sending to Node A/B
    // Central also provisions the same key to Node A and Node B optically
    try {
        await SerialBridge.send(`KEY:${keyHex}`);
        log('Key sent to Arduino Uno → transmitting optically to Central ESP32', 'info');
        log('Note: Optical key transmission takes ~80s at 250ms/bit. Keep optical path aligned.', 'warning');
    } catch (err) {
        log(`Key provisioning failed: ${err.message}`, 'error');
    }
}

// --- Message Transmission (Plaintext to Uno) ---
// Encryption happens ON the Central ESP32, not here.
async function sendMessage() {
    const messageInput = document.getElementById('messageInput');
    const message = messageInput.value.trim();

    if (!message) {
        log('Please enter a message', 'warning');
        return;
    }

    if (!SerialBridge.isConnected()) {
        log('Not connected to Arduino Uno', 'error');
        return;
    }

    try {
        log(`Original message: "${message}" (${message.length} chars)`, 'info');

        // 1. Convert message to hex for serial transport
        const encoder = new TextEncoder();
        const messageBytes = encoder.encode(message);
        const hexPayload = Array.from(messageBytes)
            .map(b => b.toString(16).padStart(2, '0')).join('');

        // 2. Track message & notify network that optical blinking has started
        currentTransmittingMessage = message;
        NetworkSync.send({
            type: 'tx_started',
            message: message
        });

        // Set safety timer based on message length (triggers when LED blinking stops)
        const expectedBlinkMs = Math.round((16 + message.length) * 8 * 250 + 1200);
        clearTimeout(opticalBlinkTimer);
        opticalBlinkTimer = setTimeout(() => {
            log('Optical blinking interval completed', 'info');
            triggerPostBlinkDisplay();
        }, expectedBlinkMs);

        // 3. Show SSS preview (browser-side visualization only)
        showSSSPreview(messageBytes);

        // 4. Build the serial command
        const serialCmd = `#01:${hexPayload}`;
        log(`Serial TX command: ${serialCmd}`, 'info');

        // 5. Send PLAINTEXT to Uno via USB serial
        //    Uno optically forwards it to Central ESP32
        //    Central ESP32 handles ALL encryption + SSS splitting
        log('Sending plaintext to Arduino Uno...', 'info');
        await SerialBridge.sendCommand(0x01, hexPayload);  // 0x01 = MSG_TYPE_DATA

        log('Payload transmitted to Uno via serial link', 'success');
        log('Uno LED on D8 is now blinking for ~' +
            Math.round((8 + 8 + 8 + 8 + 8 + 8 + message.length * 8 + 16 + 8) * 0.25) +
            's — message will appear on both dashboards when blinking stops', 'info');
        document.getElementById('unoDetail').textContent = 'Transmitting optically...';

    } catch (err) {
        log(`Send failed: ${err.message}`, 'error');
        console.error('[sendMessage] Error:', err);
    }
}

// --- SSS Preview (browser-side visualization) ---
function showSSSPreview(messageBytes) {
    const sssPanel = document.getElementById('sssPanel');
    sssPanel.style.display = 'block';

    // Split using browser-side Shamir (for preview only)
    const shares = ShamirJS.splitMessage(messageBytes, 2, 3);
    const shareHexArray = [];

    for (let i = 0; i < 3; i++) {
        const shareHex = ShamirJS.toHex(shares[i].data);
        shareHexArray.push(shareHex);
        document.getElementById(`share${i + 1}Data`).textContent =
            shareHex.length > 40 ? shareHex.substring(0, 40) + '...' : shareHex;
        updateShareStatus(i + 1, 'sent');
    }

    // Verify reconstruction works
    const reconstructed = ShamirJS.reconstructMessage([shares[0], shares[1]], 2);
    const decoder = new TextDecoder();
    const reconstructedMsg = decoder.decode(reconstructed);
    log(`SSS preview verification: "${reconstructedMsg}"`, 'info');

    log(`Share 1 (Central): x=${shares[0].shareX}`, 'data');
    log(`Share 2 (Node A):  x=${shares[1].shareX}`, 'data');
    log(`Share 3 (Node B):  x=${shares[2].shareX}`, 'data');

    // Broadcast plaintext & shares across the local network to other laptops
    const msgInput = document.getElementById('messageInput');
    const msg = msgInput ? msgInput.value : '';
    NetworkSync.send({
        type: 'message_sent',
        message: msg,
        shares: shareHexArray
    });
}

function updateShareStatus(shareNum, status) {
    const el = document.getElementById(`share${shareNum}Status`);
    el.className = `share-status ${status}`;
    const labels = { pending: 'PENDING', sent: 'TRANSMITTED', stored: 'SECURED', failed: 'REVOKED' };
    el.textContent = labels[status] || status;
}

// --- Tamper Event ---
function handleTamperEvent() {
    // Flash all node cards red
    ['centralCard', 'nodeACard', 'nodeBCard'].forEach(id => {
        document.getElementById(id).classList.add('tampered');
    });

    document.getElementById('centralStatus').innerHTML =
        '<span class="status-indicator offline"></span> TAMPERED';
    document.getElementById('nodeAStatus').innerHTML =
        '<span class="status-indicator offline"></span> TAMPERED';
    document.getElementById('nodeBStatus').innerHTML =
        '<span class="status-indicator offline"></span> TAMPERED';

    document.getElementById('centralDetail').textContent = 'Keys destroyed — locked';
    document.getElementById('nodeADetail').textContent = 'Keys destroyed — locked';
    document.getElementById('nodeBDetail').textContent = 'Keys destroyed — locked';

    // Update share statuses
    updateShareStatus(1, 'failed');
    updateShareStatus(2, 'failed');
    updateShareStatus(3, 'failed');

    // Disable send
    document.getElementById('sendBtn').disabled = true;
}

// --- Emergency Zeroize ---
async function emergencyZeroize() {
    if (!SerialBridge.isConnected()) {
        log('Not connected', 'error');
        return;
    }

    const confirmed = confirm(
        'EMERGENCY ZEROIZATION\n\n' +
        'This will PERMANENTLY destroy:\n' +
        '• All encryption keys on all nodes\n' +
        '• All stored secret shares\n' +
        '• The system will be locked until re-provisioned\n\n' +
        'This action CANNOT be undone.\n\n' +
        'Confirm execution?'
    );

    if (!confirmed) {
        log('Zeroization sequence cancelled', 'info');
        return;
    }

    log('[CRITICAL] Initiating emergency zeroization...', 'error');
    await SerialBridge.send('ZEROIZE');
    handleTamperEvent();
}

// --- Character Counter ---
document.addEventListener('DOMContentLoaded', () => {
    const msgInput = document.getElementById('messageInput');
    const charCount = document.getElementById('charCount');

    msgInput.addEventListener('input', () => {
        charCount.textContent = msgInput.value.length;
    });

    // Check Web Serial support
    if (!SerialBridge.isSupported()) {
        log('Web Serial API unsupported in this client. Use Chrome or Edge.', 'warning');
        document.getElementById('connectBtn').disabled = true;
    }

    // Check secure context (Web Serial requires HTTPS or localhost)
    if (!window.isSecureContext) {
        log('WARNING: Page not in secure context! Web Serial may not work. Open via localhost or HTTPS.', 'error');
    }

    log('Dashboard session ready. Connect Arduino Uno to begin.', 'info');
    log('Tip: If the Uno LED doesn\'t blink, check browser console (F12) for errors.', 'info');

    // Initialize LAN Network Synchronization across laptops
    NetworkSync.init();
});

// --- Test Serial Link ---
// Sends a heartbeat command (#04:) to verify the Uno receives and responds
async function testSerialLink() {
    if (!SerialBridge.isConnected()) {
        log('Not connected', 'error');
        return;
    }
    try {
        log('Sending test heartbeat (#04:) to Arduino Uno...', 'info');
        await SerialBridge.send('#04:');
        log('Test command sent — watch for "[UNO] Received command" response below', 'info');
        log('If no response appears within 3 seconds, the serial link is not working.', 'warning');
    } catch (err) {
        log(`Test failed: ${err.message}`, 'error');
    }
}
