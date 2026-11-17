// ============================================================================
//  ShardGrid — Shamir's Secret Sharing (JavaScript)
//  GF(251) arithmetic — threshold (k=2, n=3)
//  Runs in the browser for share preview; actual splitting is done on ESP32.
// ============================================================================

const SSS_PRIME = 251;

const ShamirJS = {
    // --- GF(p) Arithmetic ---
    add(a, b) {
        return ((a % SSS_PRIME) + (b % SSS_PRIME)) % SSS_PRIME;
    },

    sub(a, b) {
        return ((a % SSS_PRIME) - (b % SSS_PRIME) + SSS_PRIME) % SSS_PRIME;
    },

    mul(a, b) {
        return ((a % SSS_PRIME) * (b % SSS_PRIME)) % SSS_PRIME;
    },

    // Extended Euclidean Algorithm for modular inverse
    inv(a) {
        if (a === 0) return 0;
        let t = 0, newt = 1;
        let r = SSS_PRIME, newr = ((a % SSS_PRIME) + SSS_PRIME) % SSS_PRIME;
        while (newr !== 0) {
            const q = Math.floor(r / newr);
            [t, newt] = [newt, t - q * newt];
            [r, newr] = [newr, r - q * newr];
        }
        return ((t % SSS_PRIME) + SSS_PRIME) % SSS_PRIME;
    },

    div(a, b) {
        return this.mul(a, this.inv(b));
    },

    // --- Polynomial evaluation at x ---
    evalPoly(coeffs, x) {
        let result = 0;
        let xPow = 1;
        for (const c of coeffs) {
            result = this.add(result, this.mul(c, xPow));
            xPow = this.mul(xPow, x);
        }
        return result;
    },

    // --- Split a single byte ---
    splitByte(secret, k, n) {
        const s = secret % SSS_PRIME;
        const coeffs = [s];
        for (let i = 1; i < k; i++) {
            coeffs.push(Math.floor(Math.random() * (SSS_PRIME - 1)) + 1);
        }
        const shares = [];
        for (let i = 0; i < n; i++) {
            const x = i + 1;
            shares.push({ x, y: this.evalPoly(coeffs, x) });
        }
        return shares;
    },

    // --- Split a message (Uint8Array) into n share sets ---
    splitMessage(messageBytes, k = 2, n = 3) {
        const shareSets = [];
        for (let s = 0; s < n; s++) {
            shareSets.push({
                shareX: s + 1,
                data: new Uint8Array(messageBytes.length)
            });
        }

        for (let i = 0; i < messageBytes.length; i++) {
            const shares = this.splitByte(messageBytes[i], k, n);
            for (let s = 0; s < n; s++) {
                shareSets[s].data[i] = shares[s].y;
            }
        }

        return shareSets;
    },

    // --- Reconstruct from k shares ---
    reconstructByte(shares, k) {
        let secret = 0;
        for (let i = 0; i < k; i++) {
            const xi = shares[i].x;
            const yi = shares[i].y;
            let num = 1, den = 1;
            for (let j = 0; j < k; j++) {
                if (i === j) continue;
                const xj = shares[j].x;
                num = this.mul(num, this.sub(0, xj));
                den = this.mul(den, this.sub(xi, xj));
            }
            const basis = this.div(num, den);
            secret = this.add(secret, this.mul(yi, basis));
        }
        return secret;
    },

    reconstructMessage(shareSets, k = 2) {
        const len = shareSets[0].data.length;
        const result = new Uint8Array(len);
        for (let i = 0; i < len; i++) {
            const shares = [];
            for (let s = 0; s < k; s++) {
                shares.push({
                    x: shareSets[s].shareX,
                    y: shareSets[s].data[i]
                });
            }
            result[i] = this.reconstructByte(shares, k);
        }
        return result;
    },

    // --- Helper: bytes to hex string ---
    toHex(bytes) {
        return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join('');
    },

    fromHex(hex) {
        const bytes = new Uint8Array(hex.length / 2);
        for (let i = 0; i < bytes.length; i++) {
            bytes[i] = parseInt(hex.substr(i * 2, 2), 16);
        }
        return bytes;
    }
};
