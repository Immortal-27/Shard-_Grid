#ifndef SHAMIR_SSS_H
#define SHAMIR_SSS_H

#include <Arduino.h>
#include "config.h"

// ============================================================================
//  Shamir's Secret Sharing — GF(p) arithmetic where p = 251
//  Threshold scheme: (k=2, n=3)
//  - Secret is split into 3 shares
//  - Any 2 shares can reconstruct the secret
//  - 1 share reveals nothing about the secret
// ============================================================================

// A single share: (x, y) point on the polynomial
struct SSSShare {
    uint8_t x;      // Share index (1, 2, or 3) — never 0
    uint8_t y;      // Share value
};

// Collection of shares for a full message (byte-by-byte)
struct SSSShareSet {
    uint8_t len;                        // Number of secret bytes
    uint8_t shareX;                     // Which share index this is (1, 2, or 3)
    uint8_t data[MAX_PAYLOAD_LEN];      // Share values (y for each byte)
};

class ShamirSSS {
public:
    // --- GF(p) Arithmetic (mod 251) ---

    static uint8_t gf_add(uint8_t a, uint8_t b) {
        return (a + b) % SSS_PRIME;
    }

    static uint8_t gf_sub(uint8_t a, uint8_t b) {
        return (a - b + SSS_PRIME) % SSS_PRIME;
    }

    static uint8_t gf_mul(uint8_t a, uint8_t b) {
        return ((uint16_t)a * b) % SSS_PRIME;
    }

    // Modular inverse using extended Euclidean algorithm
    static uint8_t gf_inv(uint8_t a) {
        if (a == 0) return 0; // Undefined, but prevent crash
        int t = 0, newt = 1;
        int r = SSS_PRIME, newr = a;
        while (newr != 0) {
            int quotient = r / newr;
            int tmp = t - quotient * newt;
            t = newt;
            newt = tmp;
            tmp = r - quotient * newr;
            r = newr;
            newr = tmp;
        }
        if (t < 0) t += SSS_PRIME;
        return (uint8_t)t;
    }

    static uint8_t gf_div(uint8_t a, uint8_t b) {
        return gf_mul(a, gf_inv(b));
    }

    // --- Evaluate polynomial at point x ---
    // For k=2: f(x) = coeffs[0] + coeffs[1]*x  (mod p)
    static uint8_t evalPoly(const uint8_t* coeffs, uint8_t k, uint8_t x) {
        uint8_t result = 0;
        uint8_t xPow = 1;  // x^0 = 1
        for (uint8_t i = 0; i < k; i++) {
            result = gf_add(result, gf_mul(coeffs[i], xPow));
            xPow = gf_mul(xPow, x);
        }
        return result;
    }

    // --- Split a single byte into n shares with threshold k ---
    static void splitByte(uint8_t secret, uint8_t k, uint8_t n,
                          SSSShare* shares) {
        // Reduce secret to GF(p) range
        uint8_t s = secret % SSS_PRIME;

        // Build random polynomial of degree k-1
        // coeffs[0] = secret, coeffs[1..k-1] = random
        uint8_t coeffs[SSS_THRESHOLD];
        coeffs[0] = s;
        for (uint8_t i = 1; i < k; i++) {
            coeffs[i] = (uint8_t)(esp_random() % (SSS_PRIME - 1)) + 1; // 1..250
        }

        // Evaluate polynomial at x = 1, 2, ..., n
        for (uint8_t i = 0; i < n; i++) {
            shares[i].x = i + 1;
            shares[i].y = evalPoly(coeffs, k, shares[i].x);
        }

        // Wipe coefficients from memory
        memset(coeffs, 0, sizeof(coeffs));
    }

    // --- Split a message into n share sets ---
    static void splitMessage(const uint8_t* message, uint8_t msgLen,
                             SSSShareSet* shareSets, uint8_t k, uint8_t n) {
        for (uint8_t s = 0; s < n; s++) {
            shareSets[s].len = msgLen;
            shareSets[s].shareX = s + 1;
        }

        SSSShare shares[SSS_NUM_SHARES];
        for (uint8_t i = 0; i < msgLen; i++) {
            splitByte(message[i], k, n, shares);
            for (uint8_t s = 0; s < n; s++) {
                shareSets[s].data[i] = shares[s].y;
            }
        }

        // Wipe temporary shares
        memset(shares, 0, sizeof(shares));
    }

    // --- Reconstruct a single byte from k shares using Lagrange interpolation ---
    static uint8_t reconstructByte(const SSSShare* shares, uint8_t k) {
        uint8_t secret = 0;

        for (uint8_t i = 0; i < k; i++) {
            uint8_t xi = shares[i].x;
            uint8_t yi = shares[i].y;

            // Compute Lagrange basis polynomial at x=0
            uint8_t num = 1;  // Numerator product
            uint8_t den = 1;  // Denominator product

            for (uint8_t j = 0; j < k; j++) {
                if (i == j) continue;
                uint8_t xj = shares[j].x;
                // num *= (0 - xj) = (-xj) mod p
                num = gf_mul(num, gf_sub(0, xj));
                // den *= (xi - xj)
                den = gf_mul(den, gf_sub(xi, xj));
            }

            // basis_i = num / den
            uint8_t basis = gf_div(num, den);
            // Add yi * basis_i to result
            secret = gf_add(secret, gf_mul(yi, basis));
        }

        return secret;
    }

    // --- Reconstruct a full message from k share sets ---
    static void reconstructMessage(const SSSShareSet* shareSets, uint8_t k,
                                   uint8_t* message, uint8_t& msgLen) {
        msgLen = shareSets[0].len;
        SSSShare shares[SSS_THRESHOLD];

        for (uint8_t i = 0; i < msgLen; i++) {
            for (uint8_t s = 0; s < k; s++) {
                shares[s].x = shareSets[s].shareX;
                shares[s].y = shareSets[s].data[i];
            }
            message[i] = reconstructByte(shares, k);
        }

        memset(shares, 0, sizeof(shares));
    }
};

#endif // SHAMIR_SSS_H
