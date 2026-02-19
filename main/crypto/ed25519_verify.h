#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Verify an Ed25519 signature.
 *
 * Uses a compact implementation based on the SUPERCOP ref10 algorithm.
 * Only verification is implemented (no key generation or signing).
 *
 * @param signature  64-byte signature
 * @param message    message bytes
 * @param message_len message length
 * @param public_key 32-byte public key
 * @return true if signature is valid, false otherwise
 */
bool ed25519_verify(const uint8_t signature[64],
                    const uint8_t *message, size_t message_len,
                    const uint8_t public_key[32]);
