// ============================================================================
//  ShardGrid — Web Serial API Bridge
//  Handles USB serial communication with the Arduino Uno
// ============================================================================

const SerialBridge = {
    port: null,
    reader: null,
    writer: null,
    readableStreamClosed: null,
    writableStreamClosed: null,
    connected: false,
    onDataCallback: null,
    readBuffer: '',

    // Check browser support
    isSupported() {
        return 'serial' in navigator;
    },

    // Connect to serial port
    async connect() {
        if (!this.isSupported()) {
            throw new Error('Web Serial API not supported. Use Chrome/Edge.');
        }

        // Request port from user
        this.port = await navigator.serial.requestPort();

        // Open with matching baud rate
        await this.port.open({ baudRate: 115200 });

        this.connected = true;

        // Setup reader
        const textDecoder = new TextDecoderStream();
        this.readableStreamClosed = this.port.readable.pipeTo(textDecoder.writable);
        this.reader = textDecoder.readable.getReader();

        // Setup writer
        const textEncoder = new TextEncoderStream();
        this.writableStreamClosed = textEncoder.readable.pipeTo(this.port.writable);
        this.writer = textEncoder.writable.getWriter();

        // Start reading loop
        this._readLoop();

        // Wait for Arduino to finish rebooting after DTR reset
        // The Uno bootloader holds for ~1.5-2s before running user sketch
        await new Promise(resolve => setTimeout(resolve, 2500));

        return true;
    },

    // Disconnect
    async disconnect() {
        if (this.reader) {
            await this.reader.cancel();
            await this.readableStreamClosed.catch(() => {});
        }
        if (this.writer) {
            await this.writer.close();
            await this.writableStreamClosed.catch(() => {});
        }
        if (this.port) {
            await this.port.close();
        }

        this.connected = false;
        this.port = null;
        this.reader = null;
        this.writer = null;
    },

    // Send string to serial port
    async send(data) {
        if (!this.connected || !this.writer) {
            throw new Error('Not connected');
        }
        const payload = data + '\n';
        console.log('[SerialBridge] TX:', JSON.stringify(payload), `(${payload.length} chars)`);
        await this.writer.write(payload);
        // Small delay to let the Arduino process the input buffer
        await new Promise(resolve => setTimeout(resolve, 50));
    },

    // Send hex-encoded command
    // Format: #TYPE:HEX_PAYLOAD
    async sendCommand(msgType, hexPayload = '') {
        const typeHex = msgType.toString(16).padStart(2, '0');
        const cmd = `#${typeHex}:${hexPayload}`;
        await this.send(cmd);
    },

    // Set callback for received data
    onData(callback) {
        this.onDataCallback = callback;
    },

    // Internal read loop
    async _readLoop() {
        try {
            while (this.connected) {
                const { value, done } = await this.reader.read();
                if (done) break;

                if (value) {
                    this.readBuffer += value;

                    // Process complete lines
                    const lines = this.readBuffer.split('\n');
                    this.readBuffer = lines.pop(); // Keep incomplete line in buffer

                    for (const line of lines) {
                        const trimmed = line.trim();
                        if (trimmed && this.onDataCallback) {
                            this.onDataCallback(trimmed);
                        }
                    }
                }
            }
        } catch (err) {
            if (this.connected) {
                console.error('Serial read error:', err);
                this.connected = false;
                if (this.onDataCallback) {
                    this.onDataCallback('__DISCONNECT__');
                }
            }
        }
    },

    isConnected() {
        return this.connected;
    }
};
