// ============================================================================
//  ShardGrid — Main Application Controller
//  Orchestrates serial connection, encryption, SSS preview, and transmission
// ============================================================================

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

// --- Serial Connection ---
async function connectSerial() {
    try {
        if (!SerialBridge.isSupported()) {
            log('ERROR: Web Serial API not supported. Use Chrome or Edge.', 'error');
            alert('Web Serial API is not supported in this browser.\nPlease use Google Chrome or Microsoft Edge.');
            return;
        }

        log('Requesting serial port...', 'info');
        await SerialBridge.connect();

        // Update UI
        document.getElementById('connectBtn').disabled = true;
        document.getElementById('disconnectBtn').disabled = false;
        document.getElementById('sendBtn').disabled = false;
        document.getElementById('zeroizeBtn').disabled = false;

        const statusEl = document.getElementById('connectionStatus');
        statusEl.innerHTML = '<span class="status-dot connected"></span><span>Connected</span>';

        document.getElementById('unoStatus').innerHTML =
            '<span class="status-indicator online">●</span> Connected';

        log('Serial port connected!', 'success');

        // Listen for incoming data
        SerialBridge.onData(handleSerialData);

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
        statusEl.innerHTML = '<span class="status-dot disconnected"></span><span>Disconnected</span>';

        document.getElementById('unoStatus').innerHTML =
            '<span class="status-indicator offline">●</span> Disconnected';

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

    log(`← ${line}`, 'data');

    // Parse responses
    if (line === 'READY') {
        log('Arduino Uno is ready', 'success');
        document.getElementById('unoDetail').textContent = 'Ready and waiting';
    }
    else if (line === 'TX_DONE' || line.includes('TX_DONE')) {
        log('Optical transmission complete', 'success');
        document.getElementById('unoDetail').textContent = 'Last TX: success';
    }
    else if (line === 'SHARE_DIST_OK') {
        log('✅ All shares distributed successfully!', 'success');
        updateShareStatus(1, 'stored');
        updateShareStatus(2, 'stored');
        updateShareStatus(3, 'stored');
        document.getElementById('centralDetail').textContent = 'Shares distributed';
        document.getElementById('centralStatus').innerHTML =
            '<span class="status-indicator online">●</span> Active';
    }
    else if (line === 'SHARE_STORED_OK') {
        log('Share stored on remote node', 'success');
    }
    else if (line === 'TAMPER_ALERT') {
        log('🚨 TAMPER DETECTED! Keys destroyed!', 'error');
        handleTamperEvent();
    }
    else if (line === 'KEY_SET_OK') {
        log('Encryption key provisioned to node', 'success');
    }
    else if (line === 'ZEROIZE_DONE') {
        log('⚠️ Zeroization complete — all keys destroyed', 'warning');
    }
}

// --- Key Management ---
async function generateKey() {
    try {
        const keyHex = await CryptoEngine.generateKey();
        document.getElementById('keyDisplay').textContent = keyHex;
        document.getElementById('provisionBtn').disabled = false;
        log(`New AES-256 key generated: ${keyHex.substring(0, 16)}...`, 'success');
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
        log('Key sent to Arduino Uno → will provision to Central ESP32', 'info');
        log('Central will distribute the key to Node A & B optically', 'info');
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

        // 2. Show SSS preview (browser-side visualization only)
        showSSSPreview(messageBytes);

        // 3. Send PLAINTEXT to Uno via USB serial
        //    Uno optically forwards it to Central ESP32
        //    Central ESP32 handles ALL encryption + SSS splitting
        log('Sending plaintext to Arduino Uno...', 'info');
        await SerialBridge.sendCommand(0x01, hexPayload);  // 0x01 = MSG_TYPE_DATA

        log(`📡 Message sent to Uno → optical TX to Central Node`, 'success');
        log('Central ESP32 will encrypt + SSS split + distribute shares', 'info');
        document.getElementById('unoDetail').textContent = 'Transmitting...';

    } catch (err) {
        log(`Send failed: ${err.message}`, 'error');
    }
}

// --- SSS Preview (browser-side visualization) ---
function showSSSPreview(messageBytes) {
    const sssPanel = document.getElementById('sssPanel');
    sssPanel.style.display = 'block';

    // Split using browser-side Shamir (for preview only)
    const shares = ShamirJS.splitMessage(messageBytes, 2, 3);

    for (let i = 0; i < 3; i++) {
        const shareHex = ShamirJS.toHex(shares[i].data);
        document.getElementById(`share${i + 1}Data`).textContent =
            shareHex.length > 40 ? shareHex.substring(0, 40) + '...' : shareHex;
        updateShareStatus(i + 1, 'sent');
    }

    // Verify reconstruction works
    const reconstructed = ShamirJS.reconstructMessage([shares[0], shares[1]], 2);
    const decoder = new TextDecoder();
    const reconstructedMsg = decoder.decode(reconstructed);
    log(`SSS preview — reconstruction check: "${reconstructedMsg}"`, 'info');

    log(`Share 1 (Central): x=${shares[0].shareX}`, 'data');
    log(`Share 2 (Node A):  x=${shares[1].shareX}`, 'data');
    log(`Share 3 (Node B):  x=${shares[2].shareX}`, 'data');
}

function updateShareStatus(shareNum, status) {
    const el = document.getElementById(`share${shareNum}Status`);
    el.className = `share-status ${status}`;
    const labels = { pending: 'Pending', sent: 'Sent', stored: 'Stored ✓', failed: 'Failed ✗' };
    el.textContent = labels[status] || status;
}

// --- Tamper Event ---
function handleTamperEvent() {
    // Flash all node cards red
    ['centralCard', 'nodeACard', 'nodeBCard'].forEach(id => {
        document.getElementById(id).classList.add('tampered');
    });

    document.getElementById('centralStatus').innerHTML =
        '<span class="status-indicator offline">●</span> TAMPERED!';
    document.getElementById('nodeAStatus').innerHTML =
        '<span class="status-indicator offline">●</span> TAMPERED!';
    document.getElementById('nodeBStatus').innerHTML =
        '<span class="status-indicator offline">●</span> TAMPERED!';

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
        '⚠️ EMERGENCY ZEROIZATION\n\n' +
        'This will PERMANENTLY destroy:\n' +
        '• All encryption keys on all nodes\n' +
        '• All stored secret shares\n' +
        '• The system will be locked until re-provisioned\n\n' +
        'This action CANNOT be undone.\n\n' +
        'Are you sure?'
    );

    if (!confirmed) {
        log('Zeroization cancelled', 'info');
        return;
    }

    log('🚨 INITIATING EMERGENCY ZEROIZATION...', 'error');
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
        log('⚠️ Web Serial API not supported. Use Chrome or Edge.', 'warning');
        document.getElementById('connectBtn').disabled = true;
    }

    log('Dashboard ready. Connect Arduino Uno to begin.', 'info');
});
