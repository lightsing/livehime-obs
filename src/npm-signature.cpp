#include "npm-signature.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>

namespace livehime {
namespace {
// Active npm registry signing key from https://registry.npmjs.org/-/npm/v1/keys.
// On rotation the updater can still obtain authoritative metadata over TLS directly from npm.
constexpr auto keyId = "SHA256:DhQ8wR5APBvFHLF/+Tc+AYvPOdTpcIDqOhxsBHRwC7U";
constexpr auto publicKey = "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEY6Ya7W++7aUPzvMTrezH6Ycx3c+HOKYCcNGybJZSCJq/fd7Qa8uuAKtdIkUQtQiEKERhAmE5lMMJhP8OkDOa2g==";

bool verifySignature(QByteArrayView der, QByteArray &digest) {
    const auto key = QByteArray::fromBase64(publicKey);
    CERT_PUBLIC_KEY_INFO *keyInfo = nullptr;
    DWORD size = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                             reinterpret_cast<const BYTE *>(key.constData()), DWORD(key.size()),
                             CRYPT_DECODE_ALLOC_FLAG, nullptr, &keyInfo, &size))
        return false;
    BCRYPT_KEY_HANDLE handle = nullptr;
    const bool imported = CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, keyInfo, 0, nullptr, &handle);
    LocalFree(keyInfo);
    if (!imported)
        return false;
    CERT_ECC_SIGNATURE *signature = nullptr;
    bool valid = false;
    if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ECC_SIGNATURE,
                            reinterpret_cast<const BYTE *>(der.data()), DWORD(der.size()),
                            CRYPT_DECODE_ALLOC_FLAG, nullptr, &signature, &size)) {
        // CryptoAPI gives unsigned little-endian components. CNG takes fixed-width big-endian r || s.
        std::array<BYTE, 64> raw{};
        auto copyPart = [](CRYPT_UINT_BLOB part, BYTE *end) {
            while (part.cbData && part.pbData[part.cbData - 1] == 0)
                --part.cbData;
            if (!part.cbData || part.cbData > 32)
                return false;
            std::reverse_copy(part.pbData, part.pbData + part.cbData, end - part.cbData);
            return true;
        };
        if (copyPart(signature->r, raw.data() + 32) && copyPart(signature->s, raw.data() + 64))
            valid = BCryptVerifySignature(handle, nullptr, reinterpret_cast<BYTE *>(digest.data()), DWORD(digest.size()),
                                          raw.data(), DWORD(raw.size()), 0) == 0;
        LocalFree(signature);
    }
    BCryptDestroyKey(handle);
    return valid;
}
} // namespace
bool trustedMirrorSignature(const QString &name, const QString &version, const QJsonObject &dist) {
    const auto integrity = dist.value("integrity").toString();
    const auto message = (name + '@' + version + ':' + integrity).toUtf8();
    auto digest = QCryptographicHash::hash(message, QCryptographicHash::Sha256);
    for (const auto &value : dist.value("signatures").toArray()) {
        const auto entry = value.toObject();
        const auto signature = entry.value("sig").toString();
        if (entry.value("keyid").toString() != QLatin1String(keyId) || signature.isEmpty() || signature.size() > 128)
            continue;
        const auto decoded = QByteArray::fromBase64Encoding(signature.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (decoded && verifySignature(decoded.decoded, digest))
            return true;
    }
    return false;
}
} // namespace livehime
