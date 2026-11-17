#pragma once

#include <QString>

/**
 * @brief Enterprise pinless pairing support.
 *
 * When enabled, this Moonlight build derives the pairing PIN from its own certificate
 * instead of generating a random one. An enterprise Sunshine host that has this client
 * certificate fingerprint in its authorized list derives the same PIN and approves the
 * handshake without an operator typing anything.
 *
 * Security note: the derived PIN is computed from public material (the client
 * certificate), so pinless pairing is authenticated by the host's authorized
 * fingerprint list plus this client's TLS private key. The derived PIN only removes
 * the human step; it is not a secret of its own.
 */
class EnterprisePairing
{
public:
    /**
     * @brief Derive the deterministic pairing PIN from this client's certificate.
     *
     * Mirrors Sunshine's derivation: SHA-256("LANPROJECTION-PAIR-v1" || clientCertDER),
     * first 32 bits big-endian, modulo 10000, formatted as four decimal digits.
     *
     * @return Four-digit PIN, or an empty string when the certificate is unavailable.
     */
    static QString derivePin();

    /**
     * @brief Report whether pinless pairing is enabled in the current preferences.
     * @return true when the derived PIN should be used instead of a random PIN.
     */
    static bool enabled();
};