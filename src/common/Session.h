/***************************************************************************
* Copyright (c) 2015 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
***************************************************************************/

#ifndef PLASMALOGIN_SESSION_H
#define PLASMALOGIN_SESSION_H

#include <QString>
#include <KSharedConfig>

namespace PLASMALOGIN {

class Session  {
public:
    enum Type {
        UnknownSession = 0,
        X11Session,
        WaylandSession
    };
    Session();
    Session(Type type, const QString &name);
    bool isValid() const;
    Type type() const;
    QString name() const;

    QString fileName() const;

    QString desktopSession() const;
    QString xdgSessionType() const;
    QString exec() const;
    QString desktopNames() const;

private:
    Type m_type = UnknownSession;
    KSharedConfig::Ptr m_desktopFile;
    QString m_name;
};

    inline QDataStream &operator>>(QDataStream &stream, Session &session) {
        quint32 type;
        QString fileName;
        stream >> type >> fileName;
        session = Session(static_cast<Session::Type>(type), fileName);
        return stream;
    }



};

#endif // PLASMALOGIN_SESSION_H
