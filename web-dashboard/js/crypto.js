// ============================================================================
//  ShardGrid — AES-256-GCM Encryption (Web Crypto API)
//  Used for encrypting the message before sending to Uno, which forwards it
//  optically to the Central ESP32 for Shamir splitting.
// ============================================================================

const CryptoEngine = {
    currentKey: null,    // CryptoKey object
    currentKeyHex: '',   // Hex string of the raw key

    // Generate a new AES-256 key
    async generateKey() {
        const key = await crypto.subtle.generateKey(
            { name: 'AES-GCM', length: 256 },
            true,  // extractable
            ['encrypt', 'decrypt']
        );
        this.currentKey = key;

        // Export raw key bytes for display and provisioning
        const rawKey = await crypto.subtle.exportKey('raw', key);
        const keyBytes = new Uint8Array(rawKey);
        this.currentKeyHex = Array.from(keyBytes)
            .map(b => b.toString(16).padStart(2, '0')).join('');

        return this.currentKeyHex;
    },

    // Import a key from hex string
    async importKey(hexKey) {
        const keyBytes = new Uint8Array(hexKey.length / 2);
        for (let i = 0; i < keyBytes.length; i++) {
            keyBytes[i] = parseInt(hexKey.substr(i * 2, 2), 16);
        }

        this.currentKey = await crypto.subtle.importKey(
            'raw', keyBytes,
            { name: 'AES-GCM', length: 256 },
            true,
            ['encrypt', 'decrypt']
        );
        this.currentKeyHex = hexKey;
    },

    // Encrypt plaintext bytes
    // Returns: Uint8Array of [IV (12 bytes)][ciphertext][tag (appended by WebCrypto)]
    async encrypt(plaintextBytes) {
        if (!this.currentKey) throw new Error('No encryption key loaded');

        // Generate random 12-byte IV
        const iv = crypto.getRandomValues(new Uint8Array(12));

        // AES-GCM encrypt (WebCrypto appends 16-byte auth tag automatically)
        const ciphertext = await crypto.subtle.encrypt(
            { name: 'AES-GCM', iv: iv, tagLength: 128 },
            this.currentKey,
            plaintextBytes
        );

        // Combine: [IV][ciphertext+tag]
        const result = new Uint8Array(iv.length + ciphertext.byteLength);
        result.set(iv, 0);
        result.set(new Uint8Array(ciphertext), iv.length);

        return result;
    },

    // Decrypt ciphertext bytes
    // Input format: [IV (12 bytes)][ciphertext+tag]
    async decrypt(encryptedBytes) {
        if (!this.currentKey) throw new Error('No decryption key loaded');

        const iv = encryptedBytes.slice(0, 12);
        const ciphertext = encryptedBytes.slice(12);

        const plaintext = await crypto.subtle.decrypt(
            { name: 'AES-GCM', iv: iv, tagLength: 128 },
            this.currentKey,
            ciphertext
        );

        return new Uint8Array(plaintext);
    },

    // Get current key as hex
    getKeyHex() {
        return this.currentKeyHex;
    },

    hasKey() {
        return this.currentKey !== null;
    }
};
