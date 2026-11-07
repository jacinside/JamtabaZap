#ifndef _JAMTABA_VERSION_
#define _JAMTABA_VERSION_

#include <QStringList>

namespace loginserver {

class Version {

private:
    int majorVersion;
    int minorVersion;
    int buildNumber;
    int prerelease; // the N in 3.0.0-beta.N, 0 in a final version

    Version();
    Version(int major, int minor, int build, int prerelease);

public:
    static Version fromString(const QString &string);
    bool isNewerThan(const Version &other) const;
    bool isValid() const;
    QString toString() const;

};

} // namespace

#endif
