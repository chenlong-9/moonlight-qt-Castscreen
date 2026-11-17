#include "enterprisepairing.h"

#include "identitymanager.h"
#include "settings/streamingpreferences.h"

#include <QCryptographicHash>

#include <openssl/pem.h>
#include <openssl/x509.h>

namespace {

// Domain separator, must match Sunshine's enterprise_pairing.cpp.
constexpr char PIN_DOMAIN[] = "LANPROJECTION-PAIR-v1";

/**
 * @brief Convert a PEM certificate into its DER encoding.
 * @param pem PEM-encoded certificate.
 * @return DER bytes, or an empty array when the certificate cannot be parsed.
 */
QByteArray certificateDer(const QByteArray& pem)
{
    if (pem.isEmpty()) {
        return QByteArray();
    }

    BIO* bio = BIO_new_mem_buf(pem.constData(), pem.size());
    if (bio == nullptr) {
        return QByteArray();
    }

    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) {
        return QByteArray();
    }

    QByteArray der;
    const int length = i2d_X509(cert, nullptr);
    if (length > 0) {
        der.resize(length);
        auto* writePtr = reinterpret_cast<unsigned char*>(der.data());
        if (i2d_X509(cert, &writePtr) <= 0) {
            der.clear();
        }
    }

    X509_free(cert);
    return der;
}

}  // namespace

QString EnterprisePairing::derivePin()
{
    const QByteArray der = certificateDer(IdentityManager::get()->getCertificate());
    if (der.isEmpty()) {
        return QString();
    }

    QByteArray material(PIN_DOMAIN, static_cast<int>(sizeof(PIN_DOMAIN) - 1));
    material.append(der);

    const QByteArray digest = QCryptographicHash::hash(material, QCryptographicHash::Sha256);
    if (digest.size() < 4) {
        return QString();
    }

    const quint32 value = (static_cast<quint32>(static_cast<quint8>(digest.at(0))) << 24) |
                          (static_cast<quint32>(static_cast<quint8>(digest.at(1))) << 16) |
                          (static_cast<quint32>(static_cast<quint8>(digest.at(2))) << 8) |
                          static_cast<quint32>(static_cast<quint8>(digest.at(3)));

    return QStringLiteral("%1").arg(value % 10000U, 4, 10, QLatin1Char('0'));
}

bool EnterprisePairing::enabled()
{
    StreamingPreferences* prefs = StreamingPreferences::get();
    return prefs != nullptr && prefs->enterprisePinlessPairing;
}