#include "BuildBaseline.h"

namespace rad
{
QString buildBaseline()
{
    return QStringLiteral("Qt %1 / C++20 / MSVC x64").arg(QString::fromLatin1(QT_VERSION_STR));
}
}
