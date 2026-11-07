#include "Version.h"
#include <QtDebug>
#include <QRegularExpression>

using namespace loginserver;

Version Version::fromString(const QString &string)
{
    // 3.0.0, v3.0.0 or 3.0.0-beta.2
    static const QRegularExpression format("^v?(\\d+)\\.(\\d+)\\.(\\d+)(?:-[A-Za-z]+\\.?(\\d+))?$");
    const auto match = format.match(string);
    if (!match.hasMatch()) {
        qWarning() << "version string with wrong format: " << string;
        return Version();
    }

    return Version(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt(), match.captured(4).toInt());
}

bool Version::isValid() const
{
    return majorVersion > 0 || minorVersion > 0 || buildNumber > 0;
}

bool Version::isNewerThan(const Version &other) const
{
    if (majorVersion != other.majorVersion)
        return majorVersion > other.majorVersion;
    if (minorVersion != other.minorVersion)
        return minorVersion > other.minorVersion;
    if (buildNumber != other.buildNumber)
        return buildNumber > other.buildNumber;
    if (prerelease == other.prerelease)
        return false;
    if (prerelease == 0) // the final version comes after all its betas
        return true;
    if (other.prerelease == 0)
        return false;
    return prerelease > other.prerelease;
}

Version::Version()
{
    majorVersion = 0;
    minorVersion = 0;
    buildNumber = 0;
    prerelease = 0;
}

Version::Version(int major, int minor, int build, int prerelease)
{
    majorVersion = major;
    minorVersion = minor;
    buildNumber = build;
    this->prerelease = prerelease;
}

QString Version::toString() const
{
    return QString("Version{ major=%1, minor=%2, buildNumber=%3 }")
            .arg(majorVersion)
            .arg(minorVersion)
            .arg(buildNumber);
}

