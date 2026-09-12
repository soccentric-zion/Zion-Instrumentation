// ============================================================================
//  Arches Instrumentation
//
//  Copyright (C) 2026 Soccentric. All rights reserved.
//  Author: Sandesh Ghimire (sandesh@soccentric)
//
//  This file is part of a proprietary product of Soccentric.
//
//  NO WARRANTY:
//  This software is provided "AS IS", without warranty of any kind, either
//  express or implied, including but not limited to the warranties of
//  merchantability, fitness for a particular purpose, and non-infringement.
//  In no event shall Soccentric be liable for any claim, damages, or other
//  liability arising from the use of this software.
//
//  This software may not be used, copied, modified, distributed, or
//  disclosed, in whole or in part, without the express prior written
//  consent of Soccentric.
// ============================================================================
/**
 * @file lib/common/include/instr/common/crypto.h
 * @brief cryptographic primitives for device identity,
 *
 * signing, and encrypted backups (REQ §6.2, §9, §10).
 * SHA-256 / HMAC-SHA256 (FIPS 180-4, RFC 2104), X25519 (RFC 7748),
 * ChaCha20 (RFC 8439), HKDF-SHA256 (RFC 5869). Self-contained, no external
 * crypto dependency; every primitive is tested against published vectors.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "instr/common/error.h"

namespace instr
{
/// @brief Self-contained cryptographic primitives (hashes, AEAD, X25519) and signature verification.
namespace crypto
{

/// @brief A 32-byte digest with a lowercase-hex renderer.
struct Digest32
{
    std::array<uint8_t, 32> b{}; ///< Raw digest bytes.
    /// @brief Render the digest as 64 lowercase hex characters.
    /// @return 64 lowercase hex characters.
    std::string hex() const;
};

/// @brief SHA-256 over raw bytes (FIPS 180-4).
/// @param data Input bytes.
/// @param len Input length.
/// @return 32-byte digest.
Digest32 sha256(const uint8_t* data, size_t len);

/// @brief SHA-256 over a byte string (convenience overload).
/// @param s Input bytes.
/// @return 32-byte digest.
inline Digest32 sha256(const std::string& s)
{
    return sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
/// @brief HMAC-SHA256 message authentication code (RFC 2104).
/// @param key Secret key bytes.
/// @param msg Message to authenticate.
/// @return 32-byte MAC.
Digest32 hmac_sha256(const std::string& key, const std::string& msg);

/// @brief An X25519 keypair (RFC 7748).
struct X25519Key
{
    std::array<uint8_t, 32> priv{}; ///< Private scalar (device secret).
    std::array<uint8_t, 32> pub{}; ///< Public curve point.
};
/// @brief Random-source callback filling a buffer with @p len bytes.
using Rng32 = std::function<void(uint8_t*, size_t)>;

/// @brief Generate an X25519 keypair using @p rng.
/// @param rng Random source (tests inject a deterministic one).
/// @return Fresh keypair.
X25519Key x25519_generate(const Rng32& rng);

/// @brief Compute the X25519 shared secret (RFC 7748).
/// @param priv Our 32-byte private scalar.
/// @param pub_in Peer's 32-byte public point.
/// @return 32-byte shared secret.
std::array<uint8_t, 32> x25519(const uint8_t priv[32], const uint8_t pub_in[32]);

/// @brief ChaCha20 stream cipher keystream application (RFC 8439).
/// @param key 32-byte key.
/// @param nonce 12-byte nonce.
/// @param counter Initial block counter.
/// @param in Plaintext input.
/// @param out Ciphertext output (may alias @p in).
/// @param len Byte length to process.
void chacha20(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t* in, uint8_t* out, size_t len);

/// @brief Encrypt-then-MAC convenience: ChaCha20 stream + HMAC-SHA256 over (nonce || ciphertext).
///
/// Output layout: nonce(12) || ciphertext || tag(32).
///
/// @param key32 32-byte symmetric key.
/// @param plaintext Bytes to protect.
/// @param rng Random source for the nonce.
/// @return Sealed token (nonce || ciphertext || tag).
std::string aead_seal(const std::string& key32, const std::string& plaintext, const Rng32& rng);

/// @brief Open a token produced by aead_seal(); verifies the tag before decrypting.
/// @param key32 32-byte symmetric key.
/// @param sealed Sealed token.
/// @return Plaintext, or a SignatureInvalid/Corrupt error on tampering.
Result<std::string> aead_open(const std::string& key32, const std::string& sealed);

/// @brief HKDF-SHA256 key derivation (RFC 5869).
/// @param ikm Input key material.
/// @param salt Optional salt (may be empty).
/// @param info Context/application string.
/// @param len Desired output length in bytes.
/// @return Derived key bytes.
std::string hkdf_sha256(const std::string& ikm, const std::string& salt, const std::string& info, size_t len);

/// @brief Standard base64 encode (with padding).
/// @param raw Bytes to encode.
/// @return Base64 text.
std::string base64_encode(const std::string& raw);
/// @brief Standard base64 decode.
/// @param b64 Base64 text.
/// @return Decoded bytes, or an InvalidArgument error for malformed input.
Result<std::string> base64_decode(const std::string& b64);
/// @brief Lowercase hex encode.
/// @param raw Bytes to encode.
/// @return Hex text.
std::string hex_encode(const std::string& raw);
/// @brief Hex decode (upper or lower case accepted).
/// @param hex Hex text.
/// @return Decoded bytes, or an InvalidArgument error for malformed input.
Result<std::string> hex_decode(const std::string& hex);

/// @brief Fill @p n bytes from the OS CSPRNG (getrandom(2)).
///
/// Fails loudly rather than silently degrading — key generation must not run
/// without entropy (§6.2).
///
/// @param out Buffer to fill.
/// @param n Number of bytes.
/// @return ok() on success, or the OS error.
Error random_bytes(uint8_t* out, size_t n);

} // namespace crypto
} // namespace instr