/***************************************************************************
* Copyright (c) 2014-2015 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
* Copyright (c) 2014 Martin Bříza <mbriza@redhat.com>
* Copyright (c) 2013 Abdurrahman AVCI <abdurrahmanavci@gmail.com>
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

#include "Display.h"

#include "Configuration.h"
#include "DaemonApp.h"
#include "DisplayManager.h"
#include "XorgUserDisplayServer.h"
#include "Seat.h"
#include "SocketServer.h"
#include "Greeter.h"
#include "Utils.h"

#include <QDebug>
#include <QFile>
#include <QTimer>
#include <QLocalSocket>

#include <pwd.h>
#include <unistd.h>
#include <sys/time.h>

#include <sys/ioctl.h>
#include <fcntl.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>

#include <KDesktopFile>
#include <KConfigGroup>

#include "Login1Manager.h"
#include "Login1Session.h"
#include "VirtualTerminal.h"
#include "WaylandDisplayServer.h"
#include "config.h"

static int s_ttyFailures = 0;
#define STRINGIFY(x) #x

namespace PLASMALOGIN {
    bool isTtyInUse(const QString &desiredTty) {
        if (Logind::isAvailable()) {
            OrgFreedesktopLogin1ManagerInterface manager(Logind::serviceName(), Logind::managerPath(), QDBusConnection::systemBus());
            auto reply = manager.ListSessions();
            reply.waitForFinished();

            const auto info = reply.value();
            for(const SessionInfo &s : info) {
                OrgFreedesktopLogin1SessionInterface session(Logind::serviceName(), s.sessionPath.path(), QDBusConnection::systemBus());
                if (desiredTty == session.tTY() && session.state() != QLatin1String("closing")) {
                    qDebug() << "tty" << desiredTty << "already in use by" << session.user().path.path() << session.state()
                                      << session.display() << session.desktop() << session.vTNr();
                    return true;
                }
            }
        }
        return false;
    }

    int fetchAvailableVt() {
        if (!isTtyInUse(QStringLiteral("tty" STRINGIFY(PLASMALOGIN_INITIAL_VT)))) {
            return PLASMALOGIN_INITIAL_VT;
        }
        const auto vt = VirtualTerminal::currentVt();
        if (vt > 0 && !isTtyInUse(QStringLiteral("tty%1").arg(vt))) {
            return vt;
        }
        return VirtualTerminal::setUpNewVt();
    }

    Display::DisplayServerType Display::defaultDisplayServerType()
    {
        const QString &displayServerType = mainConfig.DisplayServer.get().toLower();
        DisplayServerType ret;
        if (displayServerType == QStringLiteral("x11-user")) {
            ret = X11UserDisplayServerType;
        } else if (displayServerType == QStringLiteral("wayland")) {
            ret = WaylandDisplayServerType;
        } else {
            qWarning("\"%s\" is an invalid value for General.DisplayServer: fall back to \"wayland\"",
                    qPrintable(displayServerType));
            ret = WaylandDisplayServerType;
        }
        return ret;
    }

    Display::Display(Seat *parent, DisplayServerType serverType)
        : QObject(parent),
        m_displayServerType(serverType),
        m_auth(new Auth(this)),
        m_seat(parent),
        m_socketServer(new SocketServer(this)),
        m_greeter(new Greeter(this))
    {

        qDebug() << "Starting" << m_displayServerType;
        // Create display server
        switch (m_displayServerType) {
        // case X11DisplayServerType:
        //     if (seat()->canTTY()) {
        //         m_terminalId = VirtualTerminal::setUpNewVt();
        //     }
        //     m_displayServer = new XorgDisplayServer(this);
        //     break;
        // case X11UserDisplayServerType:
        //     if (seat()->canTTY()) {
        //         m_terminalId = fetchAvailableVt();
        //     }
        //     m_displayServer = new XorgUserDisplayServer(this);
        //     m_greeter->setDisplayServerCommand(XorgUserDisplayServer::command(this));
        //     break;
        case WaylandDisplayServerType:
            if (seat()->canTTY()) {
                m_terminalId = fetchAvailableVt();
            }
            m_displayServer = new WaylandDisplayServer(this);
            break;
        }

        qDebug("Using VT %d", m_terminalId);

        // respond to authentication requests
        m_auth->setVerbose(true);
        connect(m_auth, &Auth::requestChanged, this, &Display::slotRequestChanged);
        connect(m_auth, &Auth::authentication, this, &Display::slotAuthenticationFinished);
        connect(m_auth, &Auth::sessionStarted, this, &Display::slotSessionStarted);
        connect(m_auth, &Auth::finished, this, &Display::slotHelperFinished);
        connect(m_auth, &Auth::info, this, &Display::slotAuthInfo);
        connect(m_auth, &Auth::error, this, &Display::slotAuthError);

        // restart display after display server ended
        connect(m_displayServer, &DisplayServer::started, this, &Display::displayServerStarted);
        connect(m_displayServer, &DisplayServer::stopped, this, &Display::stop);

        // connect login signal
        connect(m_socketServer, &SocketServer::login, this, &Display::login);

        // connect login result signals
        connect(this, &Display::loginFailed, m_socketServer, &SocketServer::loginFailed);
        connect(this, &Display::loginSucceeded, m_socketServer, &SocketServer::loginSucceeded);

        connect(m_greeter, &Greeter::failed, this, &Display::stop);
        connect(m_greeter, &Greeter::ttyFailed, this, [this] {
            ++s_ttyFailures;
            if (s_ttyFailures > 5) {
                QCoreApplication::exit(23);
            }
            // It might be the case that we are trying a tty that has been taken over by a
            // different process. In such a case, switch back to the initial one and try again.
            VirtualTerminal::jumpToVt(PLASMALOGIN_INITIAL_VT, true);
            stop();
        });
        connect(m_greeter, &Greeter::displayServerFailed, this, &Display::displayServerFailed);

        // Load autologin configuration (whether to autologin, user, session, session type)
        if ((daemonApp->first || mainConfig.Autologin.Relogin.get()) &&
            !mainConfig.Autologin.User.get().isEmpty()) {
            // determine session type
            QString autologinSession = mainConfig.Autologin.Session.get();
            // not configured: try last successful logged in
            if (autologinSession.isEmpty()) {
                autologinSession = stateConfig.Last.Session.get();
            }
            if (findSessionEntry(mainConfig.Wayland.SessionDir.get(), autologinSession)) {
                m_autologinSession = Session(Session::WaylandSession, autologinSession);
            } else if (findSessionEntry(mainConfig.X11.SessionDir.get(), autologinSession)) {
                m_autologinSession = Session(Session::X11Session, autologinSession);
            } else {
                qCritical() << "Unable to find autologin session entry" << autologinSession;
            }
        }

        // reset first flag
        daemonApp->first = false;
    }

    Display::~Display() {
        disconnect(m_auth, &Auth::finished, this, &Display::slotHelperFinished);
        stop();
    }

    Display::DisplayServerType Display::displayServerType() const
    {
        return m_displayServerType;
    }

    DisplayServer *Display::displayServer() const
    {
        return m_displayServer;
    }

    int Display::terminalId() const {
        return m_auth->isActive() ? m_sessionTerminalId : m_terminalId;
    }

    const QString &Display::name() const {
        return m_displayServer->display();
    }

    QString Display::sessionType() const {
        return m_displayServer->sessionType();
    }

    Seat *Display::seat() const {
        return m_seat;
    }

    bool Display::start() {
        if (m_started)
            return true;

        m_started = true;

        // Handle autologin early, unless it needs the display server to be up
        // (rootful X + X11 autologin session).
        if (m_autologinSession.isValid()) {
            m_auth->setAutologin(true);
            if (startAuth(mainConfig.Autologin.User.get(), QString(), m_autologinSession))
                return true;
            else
                return handleAutologinFailure();
         }
         return m_displayServer->start();
    }

    void Display::startSocketServerAndGreeter() {
        // start socket server
        m_socketServer->start(m_displayServer->display());

        if (!daemonApp->testing()) {
            // change the owner and group of the socket to avoid permission denied errors
            struct passwd *pw = getpwnam("sddm");
            if (pw) {
                if (chown(qPrintable(m_socketServer->socketAddress()), pw->pw_uid, pw->pw_gid) == -1) {
                    qWarning() << "Failed to change owner of the socket";
                    return;
                }
            }
        }

        m_greeter->setSocket(m_socketServer->socketAddress());

        // start greeter
        m_greeter->start();
    }

    bool Display::handleAutologinFailure() {
        qWarning() << "Autologin failed!";
        m_auth->setAutologin(false);
        // For late autologin handling only the greeter needs to be started.
        return m_displayServer->start();
    }

    void Display::displayServerStarted() {
        // setup display
        m_displayServer->setupDisplay();

        // log message
        qDebug() << "Display server started.";

        startSocketServerAndGreeter();
    }

    void Display::stop() {
        // check flag
        if (!m_started)
            return;

        // stop the greeter
        m_greeter->stop();

        m_auth->stop();

        // stop socket server
        m_socketServer->stop();

        // stop display server
        m_displayServer->blockSignals(true);
        m_displayServer->stop();
        m_displayServer->blockSignals(false);

        // reset flag
        m_started = false;

        // emit signal
        emit stopped();
    }

    void Display::login(QLocalSocket *socket,
                        const QString &user, const QString &password,
                        const Session &session) {
        m_socket = socket;

        //the PLASMALOGIN user has special privileges that skip password checking so that we can load the greeter
        //block ever trying to log in as the PLASMALOGIN user
        if (user == QLatin1String("sddm")) {
            emit loginFailed(m_socket);
            return;
        }

        // authenticate
        startAuth(user, password, session);
    }

    bool Display::findSessionEntry(const QStringList &dirPaths, const QString &name) const {
        const QFileInfo fileInfo(name);
        QString fileName = name;

        // append extension
        const QString extension = QStringLiteral(".desktop");
        if (!fileName.endsWith(extension))
            fileName += extension;

        for (const auto &path: dirPaths) {
            QDir dir = path;

            // Given an absolute path: Check that it matches dir
            if (fileInfo.isAbsolute() && fileInfo.absolutePath() != dir.absolutePath())
                continue;

            if (dir.exists(fileName))
                return true;
        }
        return false;
    }

    bool Display::startAuth(const QString &user, const QString &password, const Session &session) {

        if (m_auth->isActive()) {
            qWarning() << "Existing authentication ongoing, aborting";
            return false;
        }

        m_passPhrase = password;

        // sanity check
        if (!session.isValid()) {
            qCritical() << "Invalid session" << session.fileName();
            return false;
        }

        if (session.xdgSessionType().isEmpty()) {
            qCritical() << "Failed to find XDG session type for session" << session.fileName();
            return false;
        }
        if (session.exec().isEmpty()) {
            qCritical() << "Failed to find command for session" << session.fileName();
            return false;
        }

        m_reuseSessionId = QString();

        if (Logind::isAvailable() && mainConfig.Users.ReuseSession.get()) {
            OrgFreedesktopLogin1ManagerInterface manager(Logind::serviceName(), Logind::managerPath(), QDBusConnection::systemBus());
            auto reply = manager.ListSessions();
            reply.waitForFinished();

            const auto info = reply.value();
            for(const SessionInfo &s : reply.value()) {
                if (s.userName == user) {
                    OrgFreedesktopLogin1SessionInterface session(Logind::serviceName(), s.sessionPath.path(), QDBusConnection::systemBus());
                    if (session.service() == QLatin1String("plasmalogin") && session.state() == QLatin1String("online")) {
                        m_reuseSessionId = s.sessionId;
                        break;
                    }
                }
            }
        }

        // save session desktop file name, we'll use it to set the
        // last session later, in slotAuthenticationFinished()
        m_sessionName = session.fileName();

        m_sessionTerminalId = m_terminalId;

        // some information
        qDebug() << "Session" << m_sessionName << "selected, command:" << session.exec() << "for VT" << m_sessionTerminalId;

        QProcessEnvironment env;
        env.insert(QStringLiteral("PATH"), mainConfig.Users.DefaultPath.get());
        env.insert(QStringLiteral("XDG_SEAT_PATH"), daemonApp->displayManager()->seatPath(seat()->name()));
        env.insert(QStringLiteral("XDG_SESSION_PATH"), daemonApp->displayManager()->sessionPath(QStringLiteral("Session%1").arg(daemonApp->newSessionId())));
        env.insert(QStringLiteral("DESKTOP_SESSION"), session.desktopSession());
        if (!session.desktopNames().isEmpty())
            env.insert(QStringLiteral("XDG_CURRENT_DESKTOP"), session.desktopNames());
        env.insert(QStringLiteral("XDG_SESSION_CLASS"), QStringLiteral("user"));
        env.insert(QStringLiteral("XDG_SESSION_TYPE"), session.xdgSessionType());
        env.insert(QStringLiteral("XDG_SEAT"), seat()->name());
        if (m_sessionTerminalId > 0)
            env.insert(QStringLiteral("XDG_VTNR"), QString::number(m_sessionTerminalId));
#ifdef HAVE_SYSTEMD
        env.insert(QStringLiteral("XDG_SESSION_DESKTOP"), session.desktopNames());
#endif

        if (session.xdgSessionType() == QLatin1String("x11")) {
             m_auth->setDisplayServerCommand(XorgUserDisplayServer::command(this));
        } else {
            m_auth->setDisplayServerCommand(QStringLiteral());
	}
        m_auth->setUser(user);
        if (m_reuseSessionId.isNull()) {
            m_auth->setSession(session.exec());
        }
        m_auth->start();

        return true;
    }

    void Display::slotAuthenticationFinished(const QString &user, bool success) {
        if (m_auth->autologin() && !success) {
            handleAutologinFailure();
            return;
        }

        if (success) {
            qDebug() << "Authentication for user " << user << " successful";

            if (!m_reuseSessionId.isNull()) {
                OrgFreedesktopLogin1ManagerInterface manager(Logind::serviceName(), Logind::managerPath(), QDBusConnection::systemBus());
                manager.UnlockSession(m_reuseSessionId);
                manager.ActivateSession(m_reuseSessionId);
            }

            // save last user and last session
            if (mainConfig.Users.RememberLastUser.get())
                stateConfig.Last.User.set(m_auth->user());
            else
                stateConfig.Last.User.setDefault();
            if (mainConfig.Users.RememberLastSession.get())
                stateConfig.Last.Session.set(m_sessionName);
            else
                stateConfig.Last.Session.setDefault();
            stateConfig.save();

            if (m_socket)
                emit loginSucceeded(m_socket);
        } else if (m_socket) {
            qDebug() << "Authentication for user " << user << " failed";
            emit loginFailed(m_socket);
        }
        m_socket = nullptr;
    }

    void Display::slotAuthInfo(const QString &message, Auth::Info info) {
        qWarning() << "Authentication information:" << info << message;

        if (!m_socket)
            return;

        m_socketServer->informationMessage(m_socket, message);
    }

    void Display::slotAuthError(const QString &message, Auth::Error error) {
        qWarning() << "Authentication error:" << error << message;

        if (!m_socket)
            return;

        m_socketServer->informationMessage(m_socket, message);
        if (error == Auth::ERROR_AUTHENTICATION)
            emit loginFailed(m_socket);
    }

    void Display::slotHelperFinished(Auth::HelperExitStatus status) {
        // Don't restart greeter and display server unless plasmalogin-helper exited
        // with an internal error or the user session finished successfully,
        // we want to avoid greeter from restarting when an authentication
        // error happens (in this case we want to show the message from the
        // greeter
        if (status != Auth::HELPER_AUTH_ERROR)
            stop();
    }

    void Display::slotRequestChanged() {
        if (m_auth->request()->prompts().length() == 1) {
            m_auth->request()->prompts()[0]->setResponse(qPrintable(m_passPhrase));
            m_auth->request()->done();
        } else if (m_auth->request()->prompts().length() == 2) {
            m_auth->request()->prompts()[0]->setResponse(qPrintable(m_auth->user()));
            m_auth->request()->prompts()[1]->setResponse(qPrintable(m_passPhrase));
            m_auth->request()->done();
        }
    }

    void Display::slotSessionStarted(bool success) {
        qDebug() << "Session started" << success;
        if (success) {
            QTimer::singleShot(5000, m_greeter, &Greeter::stop);
        }
    }
}
