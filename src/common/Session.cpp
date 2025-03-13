#include "Session.h"

#include <KConfig>
#include <KConfigGroup>

namespace PLASMALOGIN {

static QString filePathForSession(Session::Type type, const QString &name)
{
    switch (type) {
    case Session::X11Session:
        return QStringLiteral("/usr/share/xsessions/") + name + QStringLiteral(".desktop");
    case Session::WaylandSession:
        return QStringLiteral("/usr/share/wayland-sessions/") + name + QStringLiteral(".desktop");
    default:
        return QString();
    }
}

Session::Session():
    Session(UnknownSession, QString())
{}

Session::Session(Type type, const QString &name)
    : m_type(type)
    , m_desktopFile(KSharedConfig::openConfig(filePathForSession(type, name))) //simple config
{
}

bool Session::isValid() const
{
    return m_type != UnknownSession;
}

Session::Type Session::type() const {
    return m_type;
}

QString Session::name() const {
    return m_desktopFile->group(QStringLiteral("Desktop Entry")).readEntry(QStringLiteral("Name"));
}

QString Session::fileName() const
{
    m_desktopFile->name();
}

QString Session::desktopSession() const
{
    m_desktopFile->name();
}

QString Session::xdgSessionType() const
{
    switch(m_type) {
    case WaylandSession:
        return QStringLiteral("wayland");
    case X11Session:
        return QStringLiteral("x11");
    default:
        return QString();
    }
}

QString Session::exec() const
{
    return m_desktopFile->group(QStringLiteral("Desktop Entry")).readEntry(QStringLiteral("Exec"));
}

QString Session::desktopNames() const
{
    return m_desktopFile->group(QStringLiteral("Desktop Entry")).readEntry(QStringLiteral("DesktopNames"));
}

} // namespace PLASMALOGIN
