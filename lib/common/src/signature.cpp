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
 * @file lib/common/src/signature.cpp
 * @brief Ed25519 verification via libcrypto (see signature.h).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/signature.h"

#include "instr/common/crypto.h"

#ifdef INSTR_HAVE_OPENSSL
#include <openssl/evp.h>
#endif

namespace instr
{
/// @brief Self-contained cryptographic primitives (hashes, AEAD, X25519) and signature verification.
namespace crypto
{

/// @brief True when a signature backend is compiled in.
///
/// Used to report the device's security posture northbound, not to decide
/// whether to enforce: with no backend, verification fails and signed content
/// is refused.
bool signature_verification_available()
{
#ifdef INSTR_HAVE_OPENSSL
    return true;
#else
    return false;
#endif
}

/// @brief Verify an Ed25519 signature over @p message.
///
/// @p public_key is 32 raw bytes (the HQ/release signing key, provisioned in
/// the image or at enrollment — never a device secret); @p signature is 64
/// raw bytes. Returns ok() only on a good signature: every other outcome —
/// malformed key, wrong length, unavailable backend — is an error, never a
/// pass.
/// @param public_key 32 raw Ed25519 public key bytes.
/// @param message The exact bytes that were signed.
/// @param signature 64 raw signature bytes.
/// @return ok() only on a good signature; every other outcome is an error.
Error verify_ed25519(const std::string& public_key, const std::string& message, const std::string& signature)
{
    if (public_key.size() != 32) return Error::make(Errc::InvalidArgument, "Ed25519 public key must be 32 bytes, got " + std::to_string(public_key.size()), "signature");
    if (signature.size() != 64) return Error::make(Errc::InvalidArgument, "Ed25519 signature must be 64 bytes, got " + std::to_string(signature.size()), "signature");
#ifndef INSTR_HAVE_OPENSSL
    // Fail closed. A build without a verification backend must refuse signed
    // content, not accept it unchecked (§9, §10).
    return Error::make(Errc::NotSupported, "no signature backend compiled in: refusing to accept signed content", "signature");
#else
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(public_key.data()), public_key.size());
    if (!key) return Error::make(Errc::InvalidArgument, "malformed Ed25519 public key", "signature");

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        EVP_PKEY_free(key);
        return Error::make(Errc::Internal, "cannot allocate digest context", "signature");
    }
    Error result = Error::none();
    if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) != 1) { result = Error::make(Errc::Internal, "verify init failed", "signature"); }
    else
    {
        // Ed25519 is a one-shot algorithm: the whole message at once.
        const int rc = EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(signature.data()), signature.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size());
        if (rc != 1) result = Error::make(Errc::SignatureInvalid, "signature does not verify", "signature");
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return result;
#endif
}

/// @brief Convenience overload accepting a base64 key and signature (the wire form).
/// @param public_key_b64 Base64-encoded 32-byte public key.
/// @param message The exact bytes that were signed.
/// @param signature_b64 Base64-encoded 64-byte signature.
/// @return ok() only on a good signature.
Error verify_ed25519_b64(const std::string& public_key_b64, const std::string& message, const std::string& signature_b64)
{
    auto key = base64_decode(public_key_b64);
    if (!key.ok()) return Error::make(Errc::InvalidArgument, "malformed public key", "signature");
    auto sig = base64_decode(signature_b64);
    if (!sig.ok()) return Error::make(Errc::InvalidArgument, "malformed signature", "signature");
    return verify_ed25519(*key, message, *sig);
}

} // namespace crypto
} // namespace instr
