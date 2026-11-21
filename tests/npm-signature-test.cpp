#include "npm-signature.hpp"
#include <QJsonArray>
#include <iostream>

int main() {
    // Published public registry vector, also served by npmmirror for the reference launcher.
    const QString name = "@lightsing/llc-launcher-rs-win32", version = "0.2.7";
    const QJsonObject signature{{"keyid", "SHA256:DhQ8wR5APBvFHLF/+Tc+AYvPOdTpcIDqOhxsBHRwC7U"},
                                {"sig", "MEYCIQCFYNtu8I3g/s5LnI90C7HSFdY0ILerC8bUaE4Tu+umBQIhAKXpNKsHTn1TW6mJqLULikUBfdK6Y5YfwnTotc/XtxD8"}};
    QJsonObject dist{{"signatures", QJsonArray{signature}},
                     {"integrity", "sha512-ZYWT/6eYlxys/Ny2G7Gaf6hh9Q0GW5Zo7pn1sKwSCVIhM49OmAkkytooQLjH7oaEs1YIef6UoRrZ9FF9CMmHSQ=="}};
    if (!livehime::trustedMirrorSignature(name, version, dist) ||
        livehime::trustedMirrorSignature(name, "0.2.8", dist) ||
        livehime::trustedMirrorSignature("@lightsing/livehime-obs-win32-x64", version, dist))
        return 1;
    dist["integrity"] = "different package bytes";
    if (livehime::trustedMirrorSignature(name, version, dist))
        return 1;
    std::cout << "Verified npm registry signature and rejected substituted metadata.\n";
}
