// Keep <windows.h> from defining min()/max() and from pulling in the legacy
// winsock.h. These defines must precede every include in this translation unit.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "receiverservice.h"

#include "computermanager.h"
#include "enterprisepairing.h"
#include "identitymanager.h"
#include "nvcomputer.h"
#include "settings/streamingpreferences.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkDatagram>
#include <QReadLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSslCertificate>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>
#include <QUdpSocket>
#include <QVector>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <netfw.h>
#include <shellapi.h>

#if defined(_MSC_VER)
// Shell_NotifyIconW() and ShellExecuteW() are exported by shell32.dll. The
// qmake project links user32/gdi32 and friends explicitly but not shell32, so
// request it here to keep the build fix inside this translation unit.
#pragma comment(lib, "shell32.lib")
// BSTR helpers (SysAllocString/SysFreeString) used by the firewall rule code.
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")
#endif
#endif

namespace {

// Control protocol version reported to peers
constexpr auto kServiceVersion = "1.0.0";

constexpr quint16 kDefaultControlPort = 47999;
constexpr quint16 kDefaultDiscoveryPort = 47998;

// receiver.conf next to the executable. Only these keys are understood; the
// file is optional and its absence must be indistinguishable from the
// pre-configuration behavior (machine host name, default ports).
constexpr int kMaxDisplayNameLength = 32;

// Shell_NotifyIcon(NIM_ADD) is retried after startup because Explorer may not
// have created its notification area yet right after logon. The retries stop
// after this many attempts (the previous code retried forever every 2 s under
// UIPI, where the call can never succeed).
constexpr int kTrayMaxAttempts = 3;
constexpr int kTrayRetryDelayMs = 2000;

constexpr auto kDiscoveryRequest = "LANPROJ/1 DISCOVER";

// Upper bound for a control request body, to keep a stuck client from
// growing our buffer without limit.
constexpr int kMaxRequestBodyBytes = 64 * 1024;

// How long we wait for the streaming child process to be spawned.
constexpr int kStreamStartTimeoutMs = 10000;

// How long the child gets to exit cleanly before it is killed.
constexpr int kStreamStopTimeoutMs = 5000;

// Upper bound for the complete auto-pair handshake (add host + pair).
constexpr int kAutoPairTimeoutMs = 20000;

// Upper bound for re-validating a pairing we already have on disk. Failure or
// timeout must fail the cast; a cached certificate does not prove host trust.
constexpr int kVerifyPairingTimeoutMs = 6000;

// How long we give the ComputerManager to persist the paired host before
// starting the streaming child (which re-reads the host registry).
constexpr int kHostFlushTimeoutMs = 5000;

// Compare two textual IP addresses, treating IPv4 and IPv4-mapped IPv6
// representations of the same address as equal.
bool sameAddress(const QString& left, const QString& right)
{
    if (left.isEmpty() || right.isEmpty()) {
        return false;
    }

    QHostAddress leftAddress(left);
    QHostAddress rightAddress(right);
    if (!leftAddress.isNull() && !rightAddress.isNull()) {
        if (leftAddress.protocol() == rightAddress.protocol()) {
            return leftAddress == rightAddress;
        }

        bool leftIsV4 = false;
        bool rightIsV4 = false;
        const quint32 leftV4 = leftAddress.toIPv4Address(&leftIsV4);
        const quint32 rightV4 = rightAddress.toIPv4Address(&rightIsV4);
        if (leftIsV4 && rightIsV4) {
            return leftV4 == rightV4;
        }
    }

    return left.compare(right, Qt::CaseInsensitive) == 0;
}

// Normalize a socket peer address to plain IPv4 when it is an IPv4-mapped
// IPv6 address, so it matches the addresses stored by ComputerManager.
QString normalizedPeerAddress(const QHostAddress& address)
{
    if (address.isNull()) {
        return QString();
    }

    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4);
    if (isV4) {
        return QHostAddress(v4).toString();
    }

    return address.toString();
}

const char* reasonPhrase(int statusCode)
{
    switch (statusCode) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 412:
        return "Precondition Failed";
    case 500:
        return "Internal Server Error";
    default:
        return "Error";
    }
}

QJsonObject errorBody(const QString& error)
{
    QJsonObject body;
    body.insert(QStringLiteral("error"), error);
    return body;
}

// A control character cannot be displayed in a tooltip or carried in JSON,
// so a configured receiver name containing one is rejected.
bool hasControlCharacter(const QString& text)
{
    for (const QChar ch : text) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7F) {
            return true;
        }
    }
    return false;
}

// Rewrite receiver.conf with the dialog values. Comment lines, force_video_args
// and any unknown keys are preserved; name/port/discovery_port are replaced in
// place (or appended when the file did not exist yet).
bool writeReceiverConfigFile(const QString& path, const QString& name,
                             quint16 controlPort, quint16 discoveryPort,
                             QString* error, bool nameOnly = false)
{
    QStringList lines;
    bool wroteName = false;
    bool wroteControlPort = false;
    bool wroteDiscoveryPort = false;

    QFile existing(path);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly)) {
            if (error != nullptr) {
                *error = existing.errorString();
            }
            return false;
        }

        QByteArray raw = existing.readAll();
        if (raw.startsWith("\xEF\xBB\xBF")) {
            raw.remove(0, 3);
        }

        const QStringList input = QString::fromUtf8(raw).split(QLatin1Char('\n'));
        for (QString line : input) {
            if (line.endsWith(QLatin1Char('\r'))) {
                line.chop(1);
            }

            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
                lines.append(line);
                continue;
            }

            const int equals = trimmed.indexOf(QLatin1Char('='));
            if (equals <= 0) {
                lines.append(line);
                continue;
            }

            const QString key = trimmed.left(equals).trimmed().toLower();
            if (nameOnly && key != QStringLiteral("name")) {
                lines.append(line);
                continue;
            }
            if (key == QStringLiteral("name")) {
                if (!wroteName) {
                    lines.append(QStringLiteral("name = %1").arg(name));
                    wroteName = true;
                }
                continue;
            }
            if (key == QStringLiteral("port")) {
                if (!wroteControlPort) {
                    lines.append(QStringLiteral("port = %1").arg(controlPort));
                    wroteControlPort = true;
                }
                continue;
            }
            if (key == QStringLiteral("discovery_port")) {
                if (!wroteDiscoveryPort) {
                    lines.append(QStringLiteral("discovery_port = %1").arg(discoveryPort));
                    wroteDiscoveryPort = true;
                }
                continue;
            }

            lines.append(line);
        }

        existing.close();
    }

    if (!wroteName) {
        lines.append(QStringLiteral("name = %1").arg(name));
    }
    if (!nameOnly && !wroteControlPort) {
        lines.append(QStringLiteral("port = %1").arg(controlPort));
    }
    if (!nameOnly && !wroteDiscoveryPort) {
        lines.append(QStringLiteral("discovery_port = %1").arg(discoveryPort));
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }

    QByteArray data = lines.join(QLatin1Char('\n')).toUtf8();
    if (!data.endsWith('\n')) {
        data.append('\n');
    }

    if (file.write(data) != data.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("写入不完整");
        }
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }

    return true;
}

} // namespace

#if defined(_WIN32)

namespace {

// Window message the notification area uses to call us back. WM_APP+n keeps it
// out of the range Win32 or Qt use themselves.
constexpr UINT kTrayCallbackMessage = WM_APP + 42;
constexpr UINT kTrayIconId = 1;

// Popup-menu command ids. They are only used with TPM_RETURNCMD, so they never
// have to be matched against WM_COMMAND values.
constexpr UINT kTrayCommandSettings = 0xE000;
constexpr UINT kTrayCommandShowStatus = 0xE001;
constexpr UINT kTrayCommandStopStream = 0xE002;
constexpr UINT kTrayCommandOpenLogDirectory = 0xE003;
constexpr UINT kTrayCommandQuit = 0xE004;

constexpr wchar_t kTrayWindowClassName[] = L"MoonlightReceiverTray";
constexpr wchar_t kTrayWindowTitle[] = L"Moonlight Receiver";

// QString stores UTF-16, exactly what the W-suffixed Win32 APIs and wchar_t
// expect on Windows. QString's buffer is NUL-terminated, so the result can be
// used directly for as long as the QString stays alive.
const wchar_t* wide(const QString& text)
{
    return reinterpret_cast<const wchar_t*>(text.utf16());
}

// Copy a QString into a fixed-size wide buffer such as NOTIFYICONDATAW::szTip,
// truncating to fit and always NUL-terminating.
void copyWide(wchar_t* destination, int destinationSize, const QString& text)
{
    if (destinationSize <= 0) {
        return;
    }

    const qsizetype length = qMin<qsizetype>(text.size(), destinationSize - 1);
    for (qsizetype i = 0; i < length; i++) {
        destination[i] = static_cast<wchar_t>(text.at(i).unicode());
    }
    destination[length] = L'\0';
}

// Shared by the native fallback dialog and the QML settings bridge.
INT_PTR requestFirewallElevation(HWND owner, quint16 controlPort, quint16 discoveryPort)
{
    const QString parameters = QStringLiteral("--fix-firewall %1 %2")
            .arg(controlPort).arg(discoveryPort);
    const QString executable = QCoreApplication::applicationFilePath();
    const QString directory = QCoreApplication::applicationDirPath();
    return reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"runas", wide(executable),
                                                  wide(parameters), wide(directory), SW_SHOWNORMAL));
}

QString win32ErrorText(DWORD error)
{
    wchar_t buffer[256] = {};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                                                FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, error, 0, buffer,
                                        static_cast<DWORD>(ARRAYSIZE(buffer)), nullptr);
    QString text = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
    if (text.isEmpty()) {
        text = QStringLiteral("unknown error");
    }
    return QStringLiteral("%1 (code %2)").arg(text).arg(static_cast<uint>(error));
}

HICON loadIconResource(int resourceId)
{
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resourceId));
    if (icon != nullptr) {
        return icon;
    }

    // The icon ids inside a Qt-built executable are assigned by qmake and do not
    // necessarily start at 1, so fall back to the first icon group embedded in
    // our own file. Without this the notification-area entry shows up blank.
    // ("small"/"large" are legacy macros in rpcndr.h, hence the prefixes.)
    const QString exePath = QCoreApplication::applicationFilePath();
    if (!exePath.isEmpty()) {
        HICON iconLarge = nullptr;
        HICON iconSmall = nullptr;
        if (ExtractIconExW(reinterpret_cast<const wchar_t*>(exePath.utf16()), 0,
                           &iconLarge, &iconSmall, 1) > 0) {
            if (iconSmall != nullptr) {
                if (iconLarge != nullptr) {
                    DestroyIcon(iconLarge);
                }
                return iconSmall;
            }
            if (iconLarge != nullptr) {
                return iconLarge;
            }
        }
    }

    // Never leave the notification-area entry without an image.
    return LoadIconW(nullptr, IDI_APPLICATION);
}

HICON trayIconForState(bool streaming, bool failed)
{
    // The executable embeds a single icon (IDI_ICON1). The extra resource ids
    // let a later build ship per-state icons without touching this code; until
    // then every state falls back to the application icon and the tooltip
    // carries the state difference.
    if (failed) {
        HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(3));
        if (icon != nullptr) {
            return icon;
        }
    }
    if (streaming) {
        HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(2));
        if (icon != nullptr) {
            return icon;
        }
    }
    return loadIconResource(1);
}

} // namespace

// Window procedure of the hidden owner window. Declared as a friend of
// ReceiverService in receiverservice.h so the tray helpers can stay private
// without <windows.h> in the header.
struct ReceiverServiceTrayWindow
{
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

LRESULT CALLBACK ReceiverServiceTrayWindow::wndProc(HWND hwnd, UINT message,
                                                    WPARAM wParam, LPARAM lParam)
{
    auto* service = reinterpret_cast<ReceiverService*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        service = static_cast<ReceiverService*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(service));
    }

    // A second "standby --ui" instance cannot bind the control port, so it asks
// the already running instance (through this registered message, broadcast to
// all top-level windows) to bring its main window to the foreground.
    static const UINT activateWindowMessage =
            RegisterWindowMessageW(L"LANProjection.ActivateMainWindow");
    if (activateWindowMessage != 0 && message == activateWindowMessage) {
        if (service != nullptr) {
            service->activateMainWindow();
        }
        return 0;
    }

    if (service != nullptr) {
        switch (message) {
        case kTrayCallbackMessage:
            // NIM_SETVERSION is not used, so lParam carries the classic mouse
            // event (WM_LBUTTONDBLCLK, WM_RBUTTONUP, ...).
            service->onTrayCallback(static_cast<unsigned int>(LOWORD(lParam)));
            return 0;
        case WM_ENDSESSION:
            if (wParam != 0) {
                // Explorer is going away with the session; drop the icon so it
                // cannot be left behind as a zombie.
                service->removeTrayIcon();
            }
            return 0;
        case WM_CLOSE:
            // A WM_CLOSE on the hidden window can only come from the outside
            // (taskkill without /F, a scripted SendMessage). Treat it like the
            // tray's exit command so the child process and the icon are
            // cleaned up instead of leaving a zombie tray entry.
            service->quitFromTray();
            return 0;
        default:
            break;
        }
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

#endif // _WIN32

ReceiverService::ReceiverService(QObject* parent, ComputerManager* computerManager)
    : QObject(parent),
      m_ControlServer(nullptr),
      m_DiscoverySocket(nullptr),
      m_ControlPort(kDefaultControlPort),
      m_DiscoveryPort(kDefaultDiscoveryPort),
      m_Config{false, QString(), kDefaultControlPort, kDefaultDiscoveryPort, false},
      m_FirewallRefreshTimer(new QTimer(this)),
      m_StreamProcess(nullptr),
      m_Streaming(false),
      m_ComputerManager(computerManager),
      m_OwnsComputerManager(computerManager == nullptr),
      m_AutoPairTimer(new QTimer(this)),
      m_AutoPairPhase(AutoPairPhase::None),
      m_TrayHwnd(0),
      m_TrayIconAdded(false),
      m_TrayUiBusy(false),
      m_TrayShowError(false),
      m_TrayRetryPending(false),
      m_TrayAttempts(0),
      m_IntegrityLevel(QStringLiteral("unknown"))
{
    m_AutoPairTimer->setSingleShot(true);
    m_AutoPairTimer->setInterval(kAutoPairTimeoutMs);
    connect(m_AutoPairTimer, &QTimer::timeout,
            this, &ReceiverService::handleAutoPairTimeout);
    m_FirewallRefreshTimer->setInterval(2000);
    connect(m_FirewallRefreshTimer, &QTimer::timeout, this, [this]() {
        refreshFirewallStatus();
        if (--m_FirewallRefreshAttempts <= 0) {
            m_FirewallRefreshTimer->stop();
        }
    });
    loadReceiverConfig();
}

ReceiverService::~ReceiverService()
{
    stop();
}

bool ReceiverService::start()
{
    log(QStringLiteral("Receiver service starting (version %1, pid %2, executable %3)")
            .arg(QString::fromLatin1(kServiceVersion))
            .arg(QCoreApplication::applicationPid())
            .arg(QCoreApplication::applicationFilePath()));

    // Check the mandatory integrity level first: a low-integrity process looks
    // completely silent to the user (no tray icon, no auto-pairing), so this
    // has to be visible in the log before anything else can fail.
    logIntegrityLevel();

    // receiver.conf (optional) has to be read before the ports are resolved and
    // before anything uses the display name.
    loadReceiverConfig();

    // Standby mode never goes through the GUI startup path, which is where
    // IdentityManager is normally created. The derived pinless PIN is computed
    // from our client certificate, so make sure the identity exists before
    // any pairing attempt can happen.
    IdentityManager::get();
    if (EnterprisePairing::derivePin().isEmpty()) {
        log(QStringLiteral("ERROR: no usable client certificate, automatic pairing will not work"));
    }

    // ComputerManager is only needed for the auto-pair handshake, but create
    // it here to prove early that it works without a QGuiApplication and to
    // load the persisted host registry.
    ensureComputerManager();

    m_ControlPort = resolvePort("LANPROJ_CONTROL_PORT", m_Config.controlPort);
    m_DiscoveryPort = resolvePort("LANPROJ_DISCOVERY_PORT", m_Config.discoveryPort);

    m_ControlServer = new QTcpServer(this);
    if (!m_ControlServer->listen(QHostAddress::AnyIPv4, m_ControlPort)) {
        log(QStringLiteral("ERROR: cannot listen on TCP 0.0.0.0:%1: %2")
                .arg(m_ControlPort)
                .arg(m_ControlServer->errorString()));
        m_ControlServer->deleteLater();
        m_ControlServer = nullptr;
        return false;
    }

    connect(m_ControlServer, &QTcpServer::newConnection,
            this, &ReceiverService::handleNewControlConnection);

    m_DiscoverySocket = new QUdpSocket(this);
    if (!m_DiscoverySocket->bind(QHostAddress::AnyIPv4, m_DiscoveryPort,
                                 QUdpSocket::DontShareAddress)) {
        log(QStringLiteral("ERROR: cannot bind UDP 0.0.0.0:%1: %2")
                .arg(m_DiscoveryPort)
                .arg(m_DiscoverySocket->errorString()));
        m_DiscoverySocket->deleteLater();
        m_DiscoverySocket = nullptr;
        m_ControlServer->close();
        m_ControlServer->deleteLater();
        m_ControlServer = nullptr;
        return false;
    }

    connect(m_DiscoverySocket, &QUdpSocket::readyRead,
            this, &ReceiverService::handleDiscoveryDatagrams);

    log(QStringLiteral("Listening for control requests on TCP 0.0.0.0:%1")
            .arg(m_ControlPort));
    log(QStringLiteral("Listening for discovery requests on UDP 0.0.0.0:%1")
            .arg(m_DiscoveryPort));

#if defined(_WIN32)
    // Best effort: a failure here must never keep the receiver from serving.
    // Explorer may not have created its notification area yet right after
    // logon, so retry a couple of times; a low-integrity process can never
    // register an icon (UIPI), hence the hard attempt limit. Without an icon
    // the log file becomes the only place state changes are visible
    // (see logStateChange()).
    m_TrayAttempts = 0;
    attemptTrayCreation(kTrayMaxAttempts);
#endif

    logStateChange(QStringLiteral("service started"));

    return true;
}

void ReceiverService::stop()
{
    m_FirewallRefreshTimer->stop();
    const bool pairingWasActive = (m_AutoPairPhase != AutoPairPhase::None);

    // Cancel a pending auto-pair first, so the waiting /cast request gets a
    // definite answer instead of hanging until the client times out.
    if (pairingWasActive) {
        log(QStringLiteral("Stopping while auto-pair is in progress, cancelling it"));
        failAutoPair(QStringLiteral("cancelled"));
    }

    // Abandon any request socket that outlived the handshake (shouldn't happen).
    if (m_PendingSocket) {
        m_PendingSocket->disconnectFromHost();
        m_PendingSocket.clear();
    }

    // Tear down the child process first so it doesn't outlive the service.
    if (m_StreamProcess) {
        QProcess* process = m_StreamProcess;
        m_StreamProcess = nullptr;
        m_Streaming = false;
        m_StreamHost.clear();

        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            log(QStringLiteral("Terminating streaming process (pid %1) on shutdown")
                    .arg(process->processId()));
            process->terminate();
            if (!process->waitForFinished(kStreamStopTimeoutMs)) {
                log(QStringLiteral("Streaming process did not exit, killing it"));
                process->kill();
                process->waitForFinished(kStreamStopTimeoutMs);
            }
        }
        delete process;
    }

    if (m_DiscoverySocket) {
        m_DiscoverySocket->close();
        m_DiscoverySocket->deleteLater();
        m_DiscoverySocket = nullptr;
    }

    if (m_ControlServer) {
        m_ControlServer->close();
        m_ControlServer->deleteLater();
        m_ControlServer = nullptr;
    }

    for (auto it = m_ControlBuffers.begin(); it != m_ControlBuffers.end(); ++it) {
        it.key()->deleteLater();
    }
    m_ControlBuffers.clear();

    if (m_ComputerManager && m_OwnsComputerManager) {
        if (pairingWasActive || !m_PairingTasks.isEmpty() || m_AddHostPending) {
            // An add/pair task may still be running in the global thread pool
            // with a raw NvComputer owned by ComputerManager. The process is
            // about to exit, so leaking the manager is safer than turning
            // shutdown into a use-after-free.
            log(QStringLiteral("Leaving ComputerManager alive because a pairing task may still be running"));
            m_ComputerManager = nullptr;
        }
        else {
            delete m_ComputerManager;
            m_ComputerManager = nullptr;
        }
    }

    m_TrayRetryPending = false;

    // Drop the notification-area icon before the process exits; otherwise
    // Windows keeps a dead icon in the tray until the user hovers over it.
    if (m_TrayIconAdded || m_TrayHwnd != 0) {
        destroyTray();
        log(QStringLiteral("Tray icon removed"));
    }

    log(QStringLiteral("Receiver service stopped"));
    emit receiverSettingsChanged();
}

QString ReceiverService::localHostName()
{
    // QSysInfo::machineHostName() is synchronous, unlike QHostInfo lookups.
    QString name = QSysInfo::machineHostName();
    if (name.isEmpty()) {
        name = QStringLiteral("Moonlight");
    }
    return name;
}

QString ReceiverService::logFilePath()
{
    return QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("receiver-service.log"));
}

QString ReceiverService::configFilePath()
{
    return QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("receiver.conf"));
}

QString ReceiverService::displayName() const
{
    // m_DisplayName is only set by loadReceiverConfig(); before that (and when
    // the file is missing or a name was rejected) the machine host name is
    // used, exactly like the service behaved before receiver.conf existed.
    return m_DisplayName.isEmpty() ? localHostName() : m_DisplayName;
}

QString ReceiverService::roomName() const
{
    return displayName();
}

QString ReceiverService::receiverStatus() const
{
    return m_ControlServer != nullptr && m_DiscoverySocket != nullptr
            ? serviceStateText() : QStringLiteral("未运行");
}

QString ReceiverService::firewallStatus() const
{
    return m_FirewallStatus;
}

QString ReceiverService::settingsMessage() const
{
    return m_SettingsMessage;
}

bool ReceiverService::available() const
{
    return m_ControlServer != nullptr && m_DiscoverySocket != nullptr;
}

void ReceiverService::loadReceiverConfig()
{
    m_Config = ReceiverConfig{false, QString(), kDefaultControlPort,
                              kDefaultDiscoveryPort, false};
    m_DisplayName = localHostName();

    const QString path = configFilePath();
    QFile file(path);
    if (!file.exists()) {
        // No receiver.conf is the normal case for a plain install. Keep the
        // host name and the built-in ports without logging an error.
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        log(QStringLiteral("ERROR: cannot read %1: %2 (using the host name and default ports)")
                .arg(path, file.errorString()));
        return;
    }

    QByteArray raw = file.readAll();
    // Accept UTF-8 with or without a BOM: Notepad writes one by default and
    // hand-edited files usually do not have one.
    if (raw.startsWith("\xEF\xBB\xBF")) {
        raw.remove(0, 3);
    }

    const QStringList lines = QString::fromUtf8(raw).split(QLatin1Char('\n'));
    for (int index = 0; index < lines.size(); index++) {
        QString line = lines.at(index);
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }

        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }

        const int equals = trimmed.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            log(QStringLiteral("receiver.conf:%1: ignoring line without 'key = value': %2")
                    .arg(index + 1)
                    .arg(trimmed));
            continue;
        }

        const QString key = trimmed.left(equals).trimmed().toLower();
        const QString value = trimmed.mid(equals + 1).trimmed();

        if (key == QStringLiteral("name")) {
            const bool hasQuotes = value.contains(QLatin1Char('"')) ||
                                   value.contains(QLatin1Char('\''));
            if (value.isEmpty() || value.size() > kMaxDisplayNameLength ||
                hasQuotes || hasControlCharacter(value)) {
                log(QStringLiteral("ERROR: receiver.conf:%1: rejected 'name' value "
                                   "(must be 1-%2 characters without quotes or line breaks), "
                                   "falling back to the computer name '%3'")
                        .arg(index + 1)
                        .arg(kMaxDisplayNameLength)
                        .arg(localHostName()));
                m_DisplayName.clear();
                continue;
            }

            m_DisplayName = value;
            m_Config.name = value;
            continue;
        }

        if (key == QStringLiteral("port") || key == QStringLiteral("discovery_port")) {
            bool ok = false;
            const uint port = value.toUInt(&ok);
            if (!ok || port == 0 || port > 65535) {
                log(QStringLiteral("ERROR: receiver.conf:%1: invalid %2 value '%3' "
                                   "(expected 1-65535), keeping %4")
                        .arg(index + 1)
                        .arg(key)
                        .arg(value)
                        .arg(key == QStringLiteral("port") ? m_Config.controlPort
                                                           : m_Config.discoveryPort));
                continue;
            }

            if (key == QStringLiteral("port")) {
                m_Config.controlPort = static_cast<quint16>(port);
            }
            else {
                m_Config.discoveryPort = static_cast<quint16>(port);
            }
            continue;
        }

        if (key == QStringLiteral("force_video_args")) {
            const QString lowered = value.toLower();
            if (lowered == QStringLiteral("1") || lowered == QStringLiteral("true") ||
                lowered == QStringLiteral("yes") || lowered == QStringLiteral("on")) {
                m_Config.forceVideoArgs = true;
            }
            else if (lowered == QStringLiteral("0") || lowered == QStringLiteral("false") ||
                     lowered == QStringLiteral("no") || lowered == QStringLiteral("off") ||
                     lowered.isEmpty()) {
                m_Config.forceVideoArgs = false;
            }
            else {
                log(QStringLiteral("ERROR: receiver.conf:%1: invalid force_video_args value '%2' "
                                   "(expected 0 or 1), keeping %3")
                        .arg(index + 1)
                        .arg(value)
                        .arg(m_Config.forceVideoArgs ? 1 : 0));
            }
            continue;
        }

        log(QStringLiteral("receiver.conf:%1: ignoring unknown key '%2' "
                           "(supported: name, port, discovery_port, force_video_args)")
                .arg(index + 1)
                .arg(key));
    }

    m_Config.present = true;

    log(QStringLiteral("receiver.conf loaded from %1: name '%2', control port %3, "
                       "discovery port %4, force_video_args %5")
            .arg(path, displayName())
            .arg(m_Config.controlPort)
            .arg(m_Config.discoveryPort)
            .arg(m_Config.forceVideoArgs ? 1 : 0));
}

QString ReceiverService::integrityLevelName(int* rawRid)
{
    if (rawRid != nullptr) {
        *rawRid = -1;
    }

#if defined(_WIN32)
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return QStringLiteral("unknown");
    }

    // The first call only sizes the buffer, so ERROR_INSUFFICIENT_BUFFER is
    // the expected outcome here.
    DWORD requiredBytes = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &requiredBytes);
    if (requiredBytes == 0) {
        CloseHandle(token);
        return QStringLiteral("unknown");
    }

    QByteArray buffer(static_cast<int>(requiredBytes), '\0');
    const BOOL ok = GetTokenInformation(token, TokenIntegrityLevel,
                                        buffer.data(), requiredBytes, &requiredBytes);
    CloseHandle(token);
    if (!ok) {
        return QStringLiteral("unknown");
    }

    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.constData());
    PSID sid = label->Label.Sid;
    if (sid == nullptr || !IsValidSid(sid)) {
        return QStringLiteral("unknown");
    }

    const DWORD subAuthorityCount = *GetSidSubAuthorityCount(sid);
    if (subAuthorityCount == 0) {
        return QStringLiteral("unknown");
    }

    // The integrity level is the last sub-authority of the mandatory label
    // SID: 0x1000 low, 0x2000 medium, 0x3000 high, 0x4000 system.
    const DWORD rid = *GetSidSubAuthority(sid, subAuthorityCount - 1);
    if (rawRid != nullptr) {
        *rawRid = static_cast<int>(rid);
    }

    if (rid < SECURITY_MANDATORY_MEDIUM_RID) {
        return QStringLiteral("low");
    }
    if (rid < SECURITY_MANDATORY_HIGH_RID) {
        return QStringLiteral("medium");
    }
    return QStringLiteral("high");
#else
    // POSIX has no integrity labels, so report the neutral level.
    return QStringLiteral("medium");
#endif
}

void ReceiverService::logIntegrityLevel()
{
    int rid = -1;
    m_IntegrityLevel = integrityLevelName(&rid);

    const QString ridText = rid >= 0
            ? QStringLiteral("0x%1").arg(static_cast<uint>(rid), 0, 16)
            : QStringLiteral("unavailable");
    log(QStringLiteral("Process integrity level: %1 (mandatory RID %2)")
            .arg(m_IntegrityLevel, ridText));

    if (m_IntegrityLevel == QStringLiteral("low")) {
        // A low-integrity process cannot register a notification-area icon
        // (UIPI), and its automatic pairing usually fails too, so the user
        // only sees "nothing happens". Spell out the fix in the log.
        log(QStringLiteral("ERROR: 当前进程运行在低完整性（Low IL）下：文件所在目录带沙箱标签。"
                           "托盘图标、自动配对都会失败。"
                           "请把整个程序目录移动到普通路径（例如 D:\\投屏\\）后重新启动；"
                           "可用 icacls Moonlight.exe | findstr Mandatory 确认应无输出。"));
    }
}

void ReceiverService::logStateChange(const QString& reason)
{
    emit receiverSettingsChanged();
    // The tray icon is the primary way to observe the state. When no icon
    // could be created (tray creation failed, e.g. a low-integrity sandbox),
    // the log file is the only trace left, so record every transition here.
    if (reason.isEmpty()) {
        log(QStringLiteral("STATE: %1").arg(stateString()));
    }
    else {
        log(QStringLiteral("STATE: %1 (%2)").arg(stateString(), reason));
    }
}

void ReceiverService::log(const QString& message)
{
    QFile file(logFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << " " << message << "\n";

    // Remember the most recent error for the tray status dialog, then refresh
    // the tooltip/icon. updateTrayUi() itself never logs, so this cannot
    // recurse, and the state stored in the members above is already current
    // when a state transition is logged.
    if (message.startsWith(QStringLiteral("ERROR: "))) {
        m_LastError = message;
    }
    updateTrayUi();
}

quint16 ReceiverService::resolvePort(const char* envVar, quint16 defaultPort)
{
    QByteArray value = qgetenv(envVar).trimmed();
    if (value.isEmpty()) {
        return defaultPort;
    }

    bool ok = false;
    uint port = value.toUInt(&ok);
    if (!ok || port == 0 || port > 65535) {
        log(QStringLiteral("ERROR: ignoring invalid %1 value '%2', using %3")
                .arg(QString::fromLatin1(envVar))
                .arg(QString::fromLocal8Bit(value))
                .arg(defaultPort));
        return defaultPort;
    }

    return static_cast<quint16>(port);
}

QString ReceiverService::stateString() const
{
    if (m_Streaming) {
        return QStringLiteral("streaming");
    }
    if (m_AutoPairPhase != AutoPairPhase::None) {
        return QStringLiteral("pairing");
    }
    return QStringLiteral("idle");
}

QByteArray ReceiverService::jsonResponse(int statusCode, const QJsonObject& body) const
{
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(statusCode) + " " +
            reasonPhrase(statusCode) + "\r\n";
    response += "Content-Type: application/json\r\n";
    response += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += payload;
    return response;
}

void ReceiverService::handleNewControlConnection()
{
    while (QTcpSocket* socket = m_ControlServer->nextPendingConnection()) {
        m_ControlBuffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead,
                this, &ReceiverService::handleControlReadyRead);
        connect(socket, &QTcpSocket::disconnected,
                this, &ReceiverService::handleControlDisconnected);
    }
}

void ReceiverService::handleControlDisconnected()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) {
        return;
    }

    m_ControlBuffers.remove(socket);
    socket->deleteLater();
}

void ReceiverService::handleControlReadyRead()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    auto it = m_ControlBuffers.find(socket);
    if (!socket || it == m_ControlBuffers.end()) {
        return;
    }

    it.value().append(socket->readAll());

    HttpRequest request;
    bool malformed = false;
    if (!tryParseRequest(it.value(), request, malformed)) {
        if (malformed) {
            log(QStringLiteral("ERROR: malformed control request from %1:%2")
                    .arg(socket->peerAddress().toString())
                    .arg(socket->peerPort()));
            socket->write(jsonResponse(400, errorBody(QStringLiteral("bad_request"))));
            socket->flush();
            socket->disconnectFromHost();
        }
        // Otherwise the request is still incomplete; wait for more data.
        return;
    }

    QByteArray response = handleRequest(request, socket);
    if (response.isEmpty()) {
        // The request is completed asynchronously (auto-pairing).
        // finishPendingRequest() writes the response on this socket, so stop
        // buffering it here and leave the connection open.
        m_ControlBuffers.remove(socket);
        return;
    }

    socket->write(response);
    socket->flush();
    // We always answer with Connection: close, so no keep-alive bookkeeping
    // is required here.
    socket->disconnectFromHost();
}

bool ReceiverService::tryParseRequest(const QByteArray& buffer,
                                      HttpRequest& request, bool& malformed) const
{
    malformed = false;

    int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buffer.size() > kMaxRequestBodyBytes) {
            malformed = true;
        }
        return false;
    }

    QList<QByteArray> headerLines = buffer.left(headerEnd).split('\n');
    for (QByteArray& line : headerLines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
    }

    if (headerLines.isEmpty()) {
        malformed = true;
        return false;
    }

    QList<QByteArray> requestLine = headerLines.first().split(' ');
    if (requestLine.size() < 3) {
        malformed = true;
        return false;
    }

    request.method = requestLine.at(0);
    request.path = requestLine.at(1);

    qint64 contentLength = 0;
    for (int i = 1; i < headerLines.size(); i++) {
        const QByteArray& line = headerLines.at(i);
        int colon = line.indexOf(':');
        if (colon < 0) {
            continue;
        }

        QByteArray name = line.left(colon).trimmed().toLower();
        QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "content-length") {
            bool ok = false;
            contentLength = value.toLongLong(&ok);
            if (!ok || contentLength < 0 || contentLength > kMaxRequestBodyBytes) {
                malformed = true;
                return false;
            }
        }
    }

    QByteArray body = buffer.mid(headerEnd + 4);
    if (body.size() < contentLength) {
        return false;
    }

    request.body = body.left(static_cast<int>(contentLength));
    return true;
}

QByteArray ReceiverService::handleRequest(const HttpRequest& request, QTcpSocket* socket)
{
    QByteArray path = request.path;
    int queryIndex = path.indexOf('?');
    if (queryIndex >= 0) {
        path = path.left(queryIndex);
    }

    if (request.method == "GET" && path == "/state") {
        QJsonObject body;
        body.insert(QStringLiteral("state"), stateString());
        body.insert(QStringLiteral("version"), QString::fromLatin1(kServiceVersion));
        // Display name from receiver.conf, falling back to the host name.
        body.insert(QStringLiteral("name"), displayName());
        body.insert(QStringLiteral("host"), m_StreamHost);
        // Remote diagnostics: a false tray flag together with a low integrity
        // level explains "no tray icon and no reaction" without a console.
        body.insert(QStringLiteral("tray"), m_TrayIconAdded);
        body.insert(QStringLiteral("integrity"), m_IntegrityLevel);
        return jsonResponse(200, body);
    }

    if (request.method == "POST" && path == "/cast") {
        return handleCast(request.body, socket);
    }

    if (request.method == "POST" && path == "/stop") {
        return handleStop();
    }

    return jsonResponse(404, errorBody(QStringLiteral("not_found")));
}

QByteArray ReceiverService::handleCast(const QByteArray& body, QTcpSocket* socket)
{
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        log(QStringLiteral("ERROR: POST /cast with malformed JSON body: %1")
                .arg(parseError.errorString()));
        return jsonResponse(400, errorBody(QStringLiteral("bad_request")));
    }

    QJsonObject object = document.object();
    QString host = object.value(QStringLiteral("host")).toString().trimmed();
    QString sender = object.value(QStringLiteral("sender")).toString();
    QString bodyAddress = object.value(QStringLiteral("address")).toString().trimmed();

    // The HTTP connection comes from the sender itself, so its peer address is
    // the most reliable address of the host we have to pair with. The address
    // field in the body is only a fallback for old senders.
    QString sourceAddress = socket ? normalizedPeerAddress(socket->peerAddress()) : QString();
    if (sourceAddress.isEmpty()) {
        sourceAddress = bodyAddress;
    }

    log(QStringLiteral("POST /cast host='%1' sender='%2' bodyAddress='%3' source='%4'")
            .arg(host, sender, bodyAddress, sourceAddress));

    if (m_Streaming) {
        log(QStringLiteral("ERROR: /cast rejected, a stream to '%1' is already running")
                .arg(m_StreamHost));
        return jsonResponse(409, errorBody(QStringLiteral("busy")));
    }

    if (m_AutoPairPhase != AutoPairPhase::None || m_AddHostPending || !m_PairingTasks.isEmpty()) {
        log(QStringLiteral("ERROR: /cast rejected, an automatic pairing task is still in progress"));
        QJsonObject response = errorBody(QStringLiteral("busy"));
        response.insert(QStringLiteral("state"), stateString());
        return jsonResponse(409, response);
    }

    if (host.isEmpty()) {
        log(QStringLiteral("ERROR: /cast rejected, missing host"));
        return jsonResponse(400, errorBody(QStringLiteral("bad_request")));
    }

    if (sourceAddress.isEmpty()) {
        log(QStringLiteral("ERROR: /cast rejected, no source address available"));
        QJsonObject response = errorBody(QStringLiteral("auto_pair_failed"));
        response.insert(QStringLiteral("detail"), QStringLiteral("no_source_address"));
        return jsonResponse(412, response);
    }

    // Refresh server information even for cached hosts. This resolves the current
    // UUID after sender reinstall and gives pairing the actual reachable ports.
    beginAutoPair(host, sender, sourceAddress, socket);
    return QByteArray();
}

QByteArray ReceiverService::handleStop()
{
    log(QStringLiteral("POST /stop"));

    bool pairingCancelled = false;
    if (m_AutoPairPhase != AutoPairPhase::None) {
        log(QStringLiteral("POST /stop cancelling the in-progress automatic pairing"));
        // Defer the cancellation so the 412 reply to the pending /cast request
        // is written on a later event loop iteration than this /stop reply.
        QTimer::singleShot(0, this, [this]() { failAutoPair(QStringLiteral("cancelled")); });
        pairingCancelled = true;
    }

    QProcess* process = m_StreamProcess;
    if (!process) {
        log(QStringLiteral("No stream is running, nothing to stop"));
        QJsonObject response;
        response.insert(QStringLiteral("stopped"), true);
        if (pairingCancelled) {
            response.insert(QStringLiteral("pairing_cancelled"), true);
        }
        return jsonResponse(200, response);
    }

    // Stop listening to the process before waiting, so handleStreamProcessFinished()
    // does not touch the object while we are blocking on waitForFinished().
    process->disconnect(this);
    m_StreamProcess = nullptr;
    m_Streaming = false;
    m_StreamHost.clear();

    if (process->state() != QProcess::NotRunning) {
        log(QStringLiteral("Terminating streaming process (pid %1)").arg(process->processId()));
        process->terminate();
        if (!process->waitForFinished(kStreamStopTimeoutMs)) {
            log(QStringLiteral("Streaming process did not exit, killing it"));
            process->kill();
            process->waitForFinished(kStreamStopTimeoutMs);
        }
    }

    log(QStringLiteral("Streaming process stopped (exit code %1)").arg(process->exitCode()));
    process->deleteLater();
    logStateChange(QStringLiteral("stream stopped by request"));

    QJsonObject response;
    response.insert(QStringLiteral("stopped"), true);
    return jsonResponse(200, response);
}

void ReceiverService::ensureComputerManager()
{
    if (m_ComputerManagerConnected) {
        return;
    }

    if (m_ComputerManager == nullptr) {
        m_ComputerManager = new ComputerManager(StreamingPreferences::get());
    }
    m_ComputerManagerConnected = true;

    connect(m_ComputerManager, &ComputerManager::manualHostAddCompleted,
            this, &ReceiverService::handleComputerAddCompleted);
    connect(m_ComputerManager, &ComputerManager::pairingCompleted,
            this, &ReceiverService::handlePairingCompleted);

    log(QStringLiteral("Auto-pair backend initialized: %1 known host(s), pinless pairing %2")
            .arg(m_ComputerManager->getComputers().size())
            .arg(EnterprisePairing::enabled() ? QStringLiteral("enabled")
                                              : QStringLiteral("disabled")));
}

NvComputer* ReceiverService::findComputerByAddress(const QString& address) const
{
    if (m_ComputerManager == nullptr || address.isEmpty()) {
        return nullptr;
    }

    const QVector<NvComputer*> computers = m_ComputerManager->getComputers();
    for (NvComputer* computer : computers) {
        // uniqueAddresses() takes the computer's read lock internally.
        const QVector<NvAddress> addresses = computer->uniqueAddresses();
        for (const NvAddress& candidate : addresses) {
            if (sameAddress(candidate.address(), address)) {
                return computer;
            }
        }
    }

    return nullptr;
}

bool ReceiverService::isComputerPaired(NvComputer* computer) const
{
    if (computer == nullptr) {
        return false;
    }

    QReadLocker locker(&computer->lock);
    return computer->pairState == NvComputer::PS_PAIRED &&
           !computer->serverCert.isNull() && !computer->serverCert.toDer().isEmpty();
}

void ReceiverService::beginAutoPair(const QString& host, const QString& sender,
                                    const QString& address, QTcpSocket* socket)
{
    ensureComputerManager();

    m_AutoPairPhase = AutoPairPhase::AddingHost;
    m_PairHostName = host;
    m_PairSender = sender;
    m_PairAddress = address;
    m_PendingSocket = socket;
    // Expose the address we are working on through /state while pairing.
    m_StreamHost = address;
    // A fresh attempt replaces the previous failure in the tray tooltip.
    m_TrayShowError = false;

    log(QStringLiteral("Auto-pair: no paired host at %1 for '%2' (sender '%3'), adding it")
            .arg(address, host, sender));
    logStateChange(QStringLiteral("auto-pair started"));

    m_AutoPairTimer->setInterval(kAutoPairTimeoutMs);
    m_AutoPairTimer->start();

    m_PendingPairComputer = nullptr;
    m_AddHostPending = true;
    m_ComputerManager->addNewHostManually(address);
}

bool ReceiverService::startPairingWith(NvComputer* computer)
{
    if (computer == nullptr) {
        return false;
    }

    const QString pin = EnterprisePairing::derivePin();
    if (pin.isEmpty()) {
        log(QStringLiteral("Auto-pair: cannot derive a pairing PIN (no usable certificate)"));
        return false;
    }

    if (m_AutoPairPhase != AutoPairPhase::VerifyingPairing) {
        m_AutoPairPhase = AutoPairPhase::PairingHost;
    }
    m_PendingPairComputer = computer;
    m_PairingTasks.insert(computer);

    // A host record restored from settings has an empty activeAddress and a zero
    // HTTPS port until it has been polled once. NvHTTP() would then build a URL
    // with no host and pairing would fail immediately with
    // "Host  not found (Error 3)" (QNetworkReply::HostNotFoundError), which is
    // exactly what the verify path used to hit right after start-up.
    {
        QWriteLocker locker(&computer->lock);
        if (computer->activeAddress.isNull() && !m_PairAddress.isEmpty()) {
            computer->activeAddress = NvAddress(m_PairAddress, DEFAULT_HTTP_PORT);
        }
        if (computer->activeHttpsPort == 0) {
            computer->activeHttpsPort = DEFAULT_HTTPS_PORT;
        }
        if (computer->state == NvComputer::CS_UNKNOWN) {
            computer->state = NvComputer::CS_ONLINE;
        }
    }

    log(QStringLiteral("Auto-pair: host '%1' is reachable at %2, starting pinless pairing")
            .arg(computer->name, m_PairAddress));

    if (!m_ComputerManager->tryPairHost(computer, pin)) {
        m_PairingTasks.remove(computer);
        m_PendingPairComputer = nullptr;
        failAutoPair(QStringLiteral("host_busy_or_deleted"));
        return false;
    }
    return true;
}

void ReceiverService::finishCastRequest(const QString& address, const QString& hostId, bool reused)
{
    if (!startStream(hostId)) {
        QJsonObject response = errorBody(QStringLiteral("launch_failed"));
        response.insert(QStringLiteral("detail"), QStringLiteral("stream_launch_failed"));
        finishPendingRequest(500, response);
        return;
    }

    QJsonObject response;
    response.insert(QStringLiteral("accepted"), true);
    response.insert(QStringLiteral("host"), address);
    response.insert(QStringLiteral("paired"), true);
    response.insert(QStringLiteral("reused"), reused);
    finishPendingRequest(200, response);
}

void ReceiverService::failAutoPair(const QString& detail)
{
    if (m_AutoPairPhase == AutoPairPhase::None && m_PendingSocket.isNull()) {
        return;
    }

    m_AutoPairPhase = AutoPairPhase::None;
    m_PendingPairComputer = nullptr;
    m_AutoPairTimer->stop();
    if (!m_Streaming) {
        m_StreamHost.clear();
    }

    // Keep the failure visible in the tray tooltip until the next attempt.
    m_TrayShowError = true;
    m_LastError = QStringLiteral("自动配对失败（%1）").arg(detail);

    log(QStringLiteral("Auto-pair failed: %1 (source %2, requested host '%3')")
            .arg(detail, m_PairAddress, m_PairHostName));
    logStateChange(QStringLiteral("auto-pair failed"));

    QJsonObject response;
    response.insert(QStringLiteral("error"), QStringLiteral("auto_pair_failed"));
    response.insert(QStringLiteral("detail"), detail);
    finishPendingRequest(412, response);
}

void ReceiverService::finishPendingRequest(int statusCode, const QJsonObject& body)
{
    QTcpSocket* socket = m_PendingSocket.data();
    m_PendingSocket.clear();

    if (socket == nullptr) {
        log(QStringLiteral("Auto-pair: requestor connection is already gone, response dropped"));
        return;
    }

    QByteArray payload = jsonResponse(statusCode, body);
    // Defer the reply to the next event loop iteration: this keeps the pending
    // request's socket write out of the stack of whatever slot finished the
    // pairing (e.g. /stop), which otherwise can race with the reply written for
    // that request.
    QTimer::singleShot(0, socket, [socket, payload]() {
        socket->write(payload);
        socket->flush();
        socket->disconnectFromHost();
    });
}

void ReceiverService::handleComputerAddCompleted(QString address, NvComputer* computer,
                                                 bool success, bool detectedPortBlocking)
{
    if (!m_AddHostPending || !sameAddress(address, m_PairAddress)) {
        return;
    }

    m_AddHostPending = false;
    if (m_AutoPairPhase != AutoPairPhase::AddingHost) {
        // The request was cancelled or timed out while the worker was running.
        return;
    }

    if (!success || computer == nullptr) {
        log(QStringLiteral("Auto-pair: adding host at %1 failed (port blocking detected: %2)")
                .arg(m_PairAddress, detectedPortBlocking ? QStringLiteral("yes")
                                                       : QStringLiteral("no")));
        failAutoPair(QStringLiteral("host_unreachable"));
        return;
    }

    // Use the exact discovery result, not an address lookup that could select
    // an older UUID left behind by a sender reinstall.
    if (isComputerPaired(computer)) {
        m_AutoPairPhase = AutoPairPhase::VerifyingPairing;
        m_AutoPairTimer->start(kVerifyPairingTimeoutMs);
    }
    if (!startPairingWith(computer)) {
        failAutoPair(QStringLiteral("pin_unavailable"));
    }
}

void ReceiverService::handlePairingCompleted(NvComputer* computer, QString error)
{
    // Timeouts/cancellation end the request, not the worker. A late completion
    // may release our busy guard but must not finish another cast request.
    const bool tracked = m_PairingTasks.remove(computer) != 0;
    const bool verifying = (m_AutoPairPhase == AutoPairPhase::VerifyingPairing);
    if (!tracked || computer != m_PendingPairComputer ||
            (!verifying && m_AutoPairPhase != AutoPairPhase::PairingHost)) {
        return;
    }
    m_PendingPairComputer = nullptr;

    if (!error.isEmpty()) {
        log(QStringLiteral("Auto-pair: pairing with '%1' failed: %2")
                .arg(computer->name, error));
        failAutoPair(QStringLiteral("pairing_failed"));
        return;
    }

    QString hostId;
    QByteArray expectedCert;
    {
        QReadLocker locker(&computer->lock);
        hostId = computer->uuid;
        expectedCert = computer->serverCert.toDer();
    }
    if (hostId.isEmpty() || expectedCert.isEmpty()) {
        failAutoPair(QStringLiteral("pairing_failed"));
        return;
    }

    log(QStringLiteral("Auto-pair: paired with '%1' at %2, persisting the host registry")
            .arg(computer->name, m_PairAddress));

    // The streaming child process re-reads the host registry from QSettings,
    // so the pinned certificate must be on disk before it is spawned. The
    // normal saveHost() path is asynchronous.
    if (!m_ComputerManager->flushHostsNow(kHostFlushTimeoutMs)) {
        log(QStringLiteral("ERROR: host registry flush failed (write error, unavailable worker, or timeout); integrity=%1")
                .arg(m_IntegrityLevel));
        failAutoPair(QStringLiteral("persist_failed"));
        return;
    }

    const QString address = m_PairAddress;

    // The child process reloads the host registry from QSettings from scratch,
    // so verify the pinned certificate actually reached disk before spawning
    // it. Otherwise a serialization hiccup would present as an unpaired host.
    int persistedHostCount = 0;
    bool persistedCertMatches = false;
    {
        QSettings persistedHosts;
        persistedHostCount = persistedHosts.beginReadArray(QStringLiteral("hosts"));
        for (int i = 0; i < persistedHostCount; i++) {
            persistedHosts.setArrayIndex(i);
            if (persistedHosts.value(QStringLiteral("uuid")).toString() != hostId) {
                continue;
            }

            const QByteArray persistedPem = persistedHosts.value(QStringLiteral("srvcert")).toByteArray();
            persistedCertMatches = QSslCertificate(persistedPem).toDer() == expectedCert;
            break;
        }
        persistedHosts.endArray();
    }

    log(QStringLiteral("Auto-pair: host registry holds %1 host(s), certificate for UUID %2 matches: %3")
            .arg(persistedHostCount).arg(hostId)
            .arg(persistedCertMatches ? QStringLiteral("yes") : QStringLiteral("no")));

    if (!persistedCertMatches) {
        failAutoPair(QStringLiteral("persist_failed"));
        return;
    }

    m_AutoPairPhase = AutoPairPhase::None;
    m_AutoPairTimer->stop();
    finishCastRequest(address, hostId, verifying);
}

void ReceiverService::handleAutoPairTimeout()
{
    if (m_AutoPairPhase == AutoPairPhase::None) {
        return;
    }

    log(QStringLiteral("Auto-pair: timed out during %1")
            .arg(m_AutoPairPhase == AutoPairPhase::VerifyingPairing
                     ? QStringLiteral("verification") : QStringLiteral("discovery/pairing")));
    failAutoPair(QStringLiteral("timeout"));
}

bool ReceiverService::startStream(const QString& host)
{
    m_StreamProcess = new QProcess(this);
    m_StreamProcess->setWorkingDirectory(QCoreApplication::applicationDirPath());
    // Send the child's output to the null device. A child that fills an unread
    // pipe would block forever, and the child keeps its own log file anyway.
    m_StreamProcess->setStandardOutputFile(QProcess::nullDevice());
    m_StreamProcess->setStandardErrorFile(QProcess::nullDevice());

    connect(m_StreamProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ReceiverService::handleStreamProcessFinished);
    connect(m_StreamProcess, &QProcess::errorOccurred,
            this, &ReceiverService::handleStreamProcessError);

    // Only the action, host and app are passed by default so the child reads
    // the user's own StreamingPreferences (resolution, FPS, bitrate, display
    // mode). The historical fixed arguments are appended only when
    // receiver.conf explicitly opts in with "force_video_args = 1", which
    // keeps installations that relied on them working unchanged.
    QStringList arguments;
    arguments << QStringLiteral("stream")
              << host
              << QStringLiteral("Desktop");
    if (m_Config.forceVideoArgs) {
        arguments << QStringLiteral("--1080")
                  << QStringLiteral("--fps") << QStringLiteral("60")
                  << QStringLiteral("--bitrate") << QStringLiteral("20000")
                  << QStringLiteral("--display-mode") << QStringLiteral("fullscreen");
    }

    m_StreamHost = host;
    m_Streaming = true;
    m_TrayShowError = false;

    log(QStringLiteral("Starting stream: %1 %2")
            .arg(QCoreApplication::applicationFilePath(), arguments.join(' ')));

    m_StreamProcess->start(QCoreApplication::applicationFilePath(), arguments);
    if (!m_StreamProcess->waitForStarted(kStreamStartTimeoutMs)) {
        log(QStringLiteral("ERROR: failed to start streaming process: %1")
                .arg(m_StreamProcess->errorString()));
        QProcess* process = m_StreamProcess;
        m_StreamProcess = nullptr;
        m_Streaming = false;
        m_StreamHost.clear();
        process->disconnect(this);
        process->deleteLater();
        return false;
    }

    log(QStringLiteral("Streaming process started with pid %1 for host '%2'")
            .arg(m_StreamProcess->processId())
            .arg(host));
    logStateChange(QStringLiteral("stream started"));
    return true;
}

void ReceiverService::handleStreamProcessFinished(int exitCode,
                                                  QProcess::ExitStatus exitStatus)
{
    QProcess* process = qobject_cast<QProcess*>(sender());
    if (!process || process != m_StreamProcess) {
        return;
    }

    log(QStringLiteral("Streaming process exited with code %1 (%2)")
            .arg(exitCode)
            .arg(exitStatus == QProcess::NormalExit
                     ? QStringLiteral("normal exit")
                     : QStringLiteral("crashed")));

    m_StreamProcess = nullptr;
    m_Streaming = false;
    m_StreamHost.clear();
    process->deleteLater();

    logStateChange(QStringLiteral("stream ended"));
}

void ReceiverService::handleStreamProcessError(QProcess::ProcessError error)
{
    QProcess* process = qobject_cast<QProcess*>(sender());
    if (!process || process != m_StreamProcess) {
        return;
    }

    // FailedToStart is handled by startStream(); other errors are followed by
    // a finished() signal, so we only log them here.
    log(QStringLiteral("ERROR: streaming process error (%1): %2")
            .arg(static_cast<int>(error))
            .arg(process->errorString()));
}

void ReceiverService::handleDiscoveryDatagrams()
{
    while (m_DiscoverySocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_DiscoverySocket->receiveDatagram();
        QString message = QString::fromUtf8(datagram.data()).trimmed();

        if (message != QString::fromLatin1(kDiscoveryRequest)) {
            log(QStringLiteral("Ignoring unexpected discovery payload '%1' from %2:%3")
                    .arg(message)
                    .arg(datagram.senderAddress().toString())
                    .arg(datagram.senderPort()));
            continue;
        }

        log(QStringLiteral("Discovery request from %1:%2, replying with state '%3'")
                .arg(datagram.senderAddress().toString())
                .arg(datagram.senderPort())
                .arg(stateString()));

        QJsonObject offer;
        offer.insert(QStringLiteral("name"), displayName());
        offer.insert(QStringLiteral("port"), static_cast<int>(m_ControlPort));
        offer.insert(QStringLiteral("state"), stateString());
        offer.insert(QStringLiteral("version"), QString::fromLatin1(kServiceVersion));

        QByteArray payload = QJsonDocument(offer).toJson(QJsonDocument::Compact);
        payload += '\n';

        m_DiscoverySocket->writeDatagram(payload,
                                         datagram.senderAddress(),
                                         datagram.senderPort());
    }
}
// ---------------------------------------------------------------------------
// Windows notification-area (tray) UI
//
// The tray icon is owned by a hidden Win32 window that lives in this same
// thread, so its WM_APP callback is dispatched by the existing Qt event loop.
// Nothing here blocks: menus and message boxes only run their own short modal
// loop, exactly like a native tray application would.
// ---------------------------------------------------------------------------

QString ReceiverService::serviceStateText() const
{
    if (m_Streaming) {
        return QStringLiteral("投屏中");
    }
    if (m_AutoPairPhase != AutoPairPhase::None) {
        return QStringLiteral("配对中");
    }
    return QStringLiteral("待机中");
}

QString ReceiverService::trayTooltipText() const
{
    // Keep this below NOTIFYICONDATAW::szTip (128 wide characters incl. NUL).
    // The display name from receiver.conf leads, so a room ("三楼会议室") is
    // recognizable at a glance; the fallback is the machine host name.
    const QString name = displayName();
    if (m_TrayShowError) {
        return QStringLiteral("%1 · 配对失败").arg(name);
    }
    if (m_Streaming) {
        if (m_StreamHost.isEmpty()) {
            return QStringLiteral("%1 · 投屏中").arg(name);
        }
        return QStringLiteral("%1 · 投屏中 (%2)").arg(name, m_StreamHost);
    }
    if (m_AutoPairPhase != AutoPairPhase::None) {
        if (m_StreamHost.isEmpty()) {
            return QStringLiteral("%1 · 配对中").arg(name);
        }
        return QStringLiteral("%1 · 配对中 (%2)").arg(name, m_StreamHost);
    }
    return QStringLiteral("%1 · 待机中").arg(name);
}

QString ReceiverService::trayStatusDetails() const
{
    QString status = serviceStateText();
    if ((m_Streaming || m_AutoPairPhase != AutoPairPhase::None) && !m_StreamHost.isEmpty()) {
        status += QStringLiteral(" (") + m_StreamHost + QStringLiteral(")");
    }

    int pairedCount = 0;
    int knownCount = 0;
    if (m_ComputerManager != nullptr) {
        const QVector<NvComputer*> computers = m_ComputerManager->getComputers();
        knownCount = static_cast<int>(computers.size());
        for (NvComputer* computer : computers) {
            if (isComputerPaired(computer)) {
                pairedCount++;
            }
        }
    }

    const QString error =
            m_LastError.isEmpty() ? QStringLiteral("无") : m_LastError;

    // The last entries explain why a tray icon may be missing, which is the
    // first thing to check when the user reports "no icon at all".
    QString details;
    details += QStringLiteral("服务版本：%1\n").arg(QString::fromLatin1(kServiceVersion));
    details += QStringLiteral("本机名称：%1\n").arg(displayName());
    details += QStringLiteral("控制端口：TCP %1\n").arg(m_ControlPort);
    details += QStringLiteral("发现端口：UDP %1\n").arg(m_DiscoveryPort);
    details += QStringLiteral("当前状态：%1\n").arg(status);
    details += QStringLiteral("已配对主机：%1 台（已知 %2 台）\n")
            .arg(pairedCount)
            .arg(knownCount);
    details += QStringLiteral("完整性级别：%1\n").arg(m_IntegrityLevel);
    details += QStringLiteral("托盘图标：%1\n")
            .arg(m_TrayIconAdded ? QStringLiteral("已创建")
                                 : QStringLiteral("不可用（状态只写入日志）"));
    details += QStringLiteral("配置文件：%1\n")
            .arg(m_Config.present ? configFilePath()
                                  : QStringLiteral("未使用（无 receiver.conf）"));
    details += QStringLiteral("日志文件：%1\n").arg(logFilePath());
    details += QStringLiteral("最近错误：%1").arg(error);
    return details;
}

void ReceiverService::quitFromTray()
{
    log(QStringLiteral("Exit requested (tray menu or window close), shutting the receiver service down"));

    // Stop the retry timer before it can recreate the icon we are about to
    // delete, then reuse the HTTP /stop handler so streaming is torn down the
    // same way a sender's request would do it.
    m_TrayRetryPending = false;
    if (m_StreamProcess != nullptr || m_AutoPairPhase != AutoPairPhase::None) {
        handleStop();
    }

    removeTrayIcon();
    QCoreApplication::quit();
}

#if defined(_WIN32)

void ReceiverService::attemptTrayCreation(int attemptsLeft)
{
    m_TrayAttempts++;

    if (createTray()) {
        m_TrayRetryPending = false;
        return;
    }

    if (attemptsLeft <= 1) {
        m_TrayRetryPending = false;
        // One conclusion instead of an endless retry loop: under UIPI (low
        // integrity) or without Explorer the call can never succeed, and
        // retrying forever would only spam the shell and the log.
        log(QStringLiteral("ERROR: tray: no tray icon after %1 attempt(s), continuing without one; "
                           "state changes are written to this log instead")
                .arg(m_TrayAttempts));
        return;
    }

    m_TrayRetryPending = true;
    log(QStringLiteral("Tray icon attempt %1 of %2 failed, retrying in %3 ms")
            .arg(m_TrayAttempts)
            .arg(m_TrayAttempts + attemptsLeft - 1)
            .arg(kTrayRetryDelayMs));
    QTimer::singleShot(kTrayRetryDelayMs, this, [this, attemptsLeft]() {
        if (!m_TrayRetryPending || m_TrayIconAdded) {
            return;
        }
        m_TrayRetryPending = false;
        attemptTrayCreation(attemptsLeft - 1);
    });
}

bool ReceiverService::createTray()
{
    if (m_TrayHwnd != 0) {
        return true;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        log(QStringLiteral("ERROR: tray: GetModuleHandleW failed (%1)")
                .arg(win32ErrorText(static_cast<DWORD>(GetLastError()))));
        return false;
    }

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = static_cast<UINT>(sizeof(windowClass));
    windowClass.lpfnWndProc = &ReceiverServiceTrayWindow::wndProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = loadIconResource(1);
    windowClass.lpszClassName = kTrayWindowClassName;

    if (RegisterClassExW(&windowClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log(QStringLiteral("ERROR: tray: RegisterClassExW failed (%1)")
                .arg(win32ErrorText(static_cast<DWORD>(GetLastError()))));
        return false;
    }

    // Hidden top-level window rather than HWND_MESSAGE: a message-only window
    // cannot take the foreground, and TrackPopupMenu() needs its owner to be
    // the foreground window for the menu to close on an outside click. The
    // window is zero-sized, so it never paints anything on screen.
    const HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                      kTrayWindowClassName,
                                      kTrayWindowTitle,
                                      WS_POPUP,
                                      0, 0, 0, 0,
                                      nullptr, nullptr, instance,
                                      static_cast<LPVOID>(this));
    if (hwnd == nullptr) {
        log(QStringLiteral("ERROR: tray: CreateWindowExW failed (%1)")
                .arg(win32ErrorText(static_cast<DWORD>(GetLastError()))));
        return false;
    }
    m_TrayHwnd = reinterpret_cast<quintptr>(hwnd);

    // Zero-sized and shown without activation: it must be a "visible" window
    // for SetForegroundWindow() to succeed later, but it is invisible to the
    // user because it has no client area.
    ShowWindow(hwnd, SW_SHOWNA);

    NOTIFYICONDATAW iconData = {};
    iconData.cbSize = static_cast<DWORD>(sizeof(iconData));
    iconData.hWnd = hwnd;
    iconData.uID = kTrayIconId;
    iconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    iconData.uCallbackMessage = kTrayCallbackMessage;
    iconData.hIcon = trayIconForState(m_Streaming, m_TrayShowError);
    const QString tooltip = trayTooltipText();
    copyWide(iconData.szTip, static_cast<int>(ARRAYSIZE(iconData.szTip)), tooltip);

    if (!Shell_NotifyIconW(NIM_ADD, &iconData)) {
        const DWORD error = static_cast<DWORD>(GetLastError());
        DestroyWindow(hwnd);
        m_TrayHwnd = 0;
        log(QStringLiteral("ERROR: tray: Shell_NotifyIconW(NIM_ADD) failed (%1)")
                .arg(win32ErrorText(error)));
        // The tooltip is exactly what cannot be shown in this situation, so
        // leave the same actionable hint in the log file instead. Error 5
        // (access denied) from NIM_ADD is almost always UIPI: the process runs
        // at low integrity because its directory carries a sandbox label.
        log(QStringLiteral("ERROR: tray: 托盘图标不可用，状态提示只能写入本日志。"
                           "若上方错误为“拒绝访问”，通常是低完整性/UIPI 限制："
                           "请把整个程序目录移动到普通路径（例如 D:\\投屏\\）后重新启动，"
                           "并用 icacls Moonlight.exe | findstr Mandatory 确认应无输出。"));
        return false;
    }

    m_TrayIconAdded = true;
    log(QStringLiteral("Tray icon created (tooltip: %1)").arg(tooltip));
    return true;
}

void ReceiverService::removeTrayIcon()
{
    if (!m_TrayIconAdded) {
        return;
    }

    NOTIFYICONDATAW iconData = {};
    iconData.cbSize = static_cast<DWORD>(sizeof(iconData));
    iconData.hWnd = reinterpret_cast<HWND>(m_TrayHwnd);
    iconData.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &iconData);
    m_TrayIconAdded = false;
}

void ReceiverService::destroyTray()
{
    removeTrayIcon();

    if (m_TrayHwnd != 0) {
        DestroyWindow(reinterpret_cast<HWND>(m_TrayHwnd));
        m_TrayHwnd = 0;
    }
}

void ReceiverService::updateTrayUi()
{
    // Called from log() and from every state transition, including very early
    // ones before the icon exists. Never log from here: log() calls back into
    // this function.
    if (!m_TrayIconAdded || m_TrayHwnd == 0) {
        return;
    }

    NOTIFYICONDATAW iconData = {};
    iconData.cbSize = static_cast<DWORD>(sizeof(iconData));
    iconData.hWnd = reinterpret_cast<HWND>(m_TrayHwnd);
    iconData.uID = kTrayIconId;
    iconData.uFlags = NIF_ICON | NIF_TIP;
    iconData.hIcon = trayIconForState(m_Streaming, m_TrayShowError);
    copyWide(iconData.szTip, static_cast<int>(ARRAYSIZE(iconData.szTip)),
             trayTooltipText());

    Shell_NotifyIconW(NIM_MODIFY, &iconData);
}

void ReceiverService::onTrayCallback(unsigned int event)
{
    switch (event) {
    case WM_LBUTTONDBLCLK:
        // Double-click opens the status dialog, like the default menu entry.
        showTrayStatus();
        break;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        showTrayMenu();
        break;
    default:
        break;
    }
}

void ReceiverService::showTrayMenu()
{
    if (m_TrayHwnd == 0 || m_TrayUiBusy) {
        return;
    }

    const HWND hwnd = reinterpret_cast<HWND>(m_TrayHwnd);

    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        log(QStringLiteral("ERROR: tray: CreatePopupMenu failed (%1)")
                .arg(win32ErrorText(static_cast<DWORD>(GetLastError()))));
        return;
    }

    AppendMenuW(menu, MF_STRING, kTrayCommandSettings, L"接收端设置…");
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kTrayCommandShowStatus, L"显示状态");

    const bool canStop = m_Streaming || m_AutoPairPhase != AutoPairPhase::None;
    AppendMenuW(menu, MF_STRING | (canStop ? 0 : MF_GRAYED),
                kTrayCommandStopStream, L"停止投屏");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCommandOpenLogDirectory, L"打开日志目录");
    AppendMenuW(menu, MF_STRING, kTrayCommandQuit, L"退出");

    // A tray menu only disappears on an outside click when its owner is the
    // foreground window. The documented dance is SetForegroundWindow() before
    // TrackPopupMenu() and a posted WM_NULL afterwards.
    SetForegroundWindow(hwnd);

    // Anchor the menu at the mouse: passing 0,0 puts it in the top-left corner
    // of the screen, because the owner window is an invisible 0x0 window.
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        cursor.x = 0;
        cursor.y = 0;
    }

    const UINT command = TrackPopupMenu(menu,
                                        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        cursor.x, cursor.y, 0, hwnd, nullptr);

    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    if (command != 0) {
        handleTrayCommand(static_cast<unsigned int>(command));
    }
}

void ReceiverService::handleTrayCommand(unsigned int command)
{
    switch (command) {
    case kTrayCommandSettings:
        if (activateMainWindow()) {
            refreshFirewallStatus();
            emit openNativeSettingsRequested();
        }
        else {
            // Retain the Win32 dialog for standalone/headless receivers.
            showSettingsDialog();
        }
        break;
    case kTrayCommandShowStatus:
        showTrayStatus();
        break;
    case kTrayCommandStopStream:
        log(QStringLiteral("Tray menu: stop streaming requested"));
        // Reuse the HTTP /stop implementation instead of duplicating the
        // teardown logic (auto-pair cancellation, child process termination).
        handleStop();
        updateTrayUi();
        break;
    case kTrayCommandOpenLogDirectory:
        openLogDirectory();
        break;
    case kTrayCommandQuit:
        quitFromTray();
        break;
    default:
        log(QStringLiteral("Tray menu: ignoring unknown command %1")
                .arg(command));
        break;
    }
}

void ReceiverService::showTrayStatus()
{
    // The message box runs a nested modal loop; block re-entry so a second
    // double-click cannot stack a second dialog on top of the first.
    if (m_TrayUiBusy) {
        return;
    }

    m_TrayUiBusy = true;
    updateTrayUi();
    MessageBoxW(nullptr,
                wide(trayStatusDetails()),
                L"局域网投屏 · 状态",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    m_TrayUiBusy = false;
    updateTrayUi();
}

void ReceiverService::openLogDirectory()
{
    const QString directory = QDir(QCoreApplication::applicationDirPath()).absolutePath();
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", wide(directory),
                                           nullptr, wide(directory), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        log(QStringLiteral("ERROR: tray: ShellExecuteW failed to open '%1' (%2)")
                .arg(directory)
                .arg(win32ErrorText(static_cast<DWORD>(reinterpret_cast<INT_PTR>(result)))));
    }
}

// ---------------------------------------------------------------------------
// Receiver settings dialog and firewall integration
//
// The receiver has no QML page of its own (the QML tree is Moonlight's normal
// UI and must not be modified for the receiver), so its configuration lives in
// a native Win32 dialog. It is assembled from an in-memory DLGTEMPLATE to avoid
// a resource script, and it only uses the Win32 GUI APIs that the tray code
// already depends on: no Qt Widgets, no extra DLLs.
//
// Firewall management uses the COM firewall policy API directly (no
// netsh/PowerShell): the dialog can enumerate the effective inbound rules and
// the "--fix-firewall" helper adds the LANProjection-* rules.
// ---------------------------------------------------------------------------

namespace {

// Display names of the rules created by "--fix-firewall" / 放行防火墙. They are
// also the fast path for detection.
//
// The program rule is suffixed with "-Moonlight" because the sender creates an
// application rule for its own executable; sharing the bare name made each side
// overwrite the other's rule. The port rules stay shared: both sides allow the
// same Moonlight port set, so one rule is valid for both.
constexpr wchar_t kFirewallRuleApp[] = L"LANProjection-App-Moonlight";
constexpr wchar_t kFirewallRuleTcp[] = L"LANProjection-TCP";
constexpr wchar_t kFirewallRuleUdp[] = L"LANProjection-UDP";

// Name used by older receiver builds. It is only trusted (and only removed)
// when it still points at this executable.
constexpr wchar_t kFirewallRuleAppLegacy[] = L"LANProjection-App";

// Fixed Moonlight ports: HTTPS, HTTP and RTSP for the TCP rule; the streaming
// session's UDP range. The configurable control/discovery ports are added to
// the required set at runtime.
constexpr int kFixedTcpPorts[] = { 47984, 47989, 47990 };
constexpr int kStreamingUdpFirstPort = 47998;
constexpr int kStreamingUdpLastPort = 48010;

// GUIDs of the firewall coclasses/interfaces, defined locally so this file
// does not depend on uuid.lib (netfw.h only declares them as extern).
const CLSID kClsidNetFwPolicy2 = {0xE2B3C97F, 0x6AE1, 0x41AC,
                                  {0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD}};
const CLSID kClsidNetFwRule = {0x2C5BC43E, 0x3369, 0x4C33,
                               {0xAB, 0x0C, 0xBE, 0x94, 0x69, 0x67, 0x7A, 0xF4}};
const IID kIidNetFwPolicy2 = {0x98325047, 0xC671, 0x4174,
                              {0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86}};
const IID kIidNetFwRule = {0xAF230D27, 0xBABA, 0x4E42,
                           {0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2}};
const IID kIidEnumVariant = {0x00020404, 0x0000, 0x0000,
                             {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// Dialog control ids. They only have to be unique inside this dialog.
constexpr int kSettingsNameEdit = 2001;
constexpr int kSettingsControlPortEdit = 2002;
constexpr int kSettingsDiscoveryPortEdit = 2003;
constexpr int kSettingsIntegrityValue = 2010;
constexpr int kSettingsTrayValue = 2011;
constexpr int kSettingsFirewallSummary = 2012;
constexpr int kSettingsFirewallAppValue = 2013;
constexpr int kSettingsFirewallTcpValue = 2014;
constexpr int kSettingsFirewallUdpValue = 2015;
constexpr int kSettingsStatusText = 2016;
constexpr int kSettingsFixFirewallButton = 2101;
constexpr int kSettingsOpenGuiButton = 2102;

constexpr UINT_PTR kSettingsRefreshTimer = 1;

enum class RuleStatus
{
    Present,
    Missing,
    Failed,
};

struct RuleCheck
{
    RuleStatus status = RuleStatus::Missing;
    bool matchedByFallback = false;
    bool staleNamedRule = false;
    QString matchedName;

    bool present() const { return status == RuleStatus::Present; }
};

struct FirewallReport
{
    bool detectionFailed = false;
    QString failure;
    RuleCheck app;
    RuleCheck tcp;
    RuleCheck udp;

    // The receiver is effectively allowed when its own program rule matches, or
    // when the shared TCP/UDP rules cover every port it needs. The sender uses
    // the same port sets, so those rules are often already in place.
    bool allowed() const
    {
        return !detectionFailed && (app.present() || (tcp.present() && udp.present()));
    }
};

// Minimal owning COM pointer: avoids pulling in comdef.h/WRL for four objects.
template <typename T>
struct ComPtr
{
    T* ptr = nullptr;

    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ~ComPtr() { reset(); }

    void reset(T* value = nullptr)
    {
        if (ptr != nullptr) {
            ptr->Release();
        }
        ptr = value;
    }

    T* get() const { return ptr; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

// COM apartment for one query/fix operation. A thread that already joined an
// apartment (Qt may have done OleInitialize for drag & drop) returns S_FALSE or
// RPC_E_CHANGED_MODE; the objects still work, so only SUCCEEDED() results are
// paired with CoUninitialize().
struct ComApartment
{
    HRESULT result;

    ComApartment() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment()
    {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

QString hresultText(HRESULT hr)
{
    return QStringLiteral("HRESULT 0x%1")
            .arg(QString::number(static_cast<quint32>(hr), 16)
                     .rightJustified(8, QLatin1Char('0')));
}

// Read the current token only when Windows denies a firewall query. A low
// integrity label is diagnostic evidence, not evidence that rules are missing.
bool firewallQueryRunsAtLowIntegrity()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    DWORD requiredBytes = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &requiredBytes);
    QByteArray buffer(static_cast<int>(requiredBytes), '\0');
    const BOOL ok = requiredBytes != 0 &&
            GetTokenInformation(token, TokenIntegrityLevel, buffer.data(),
                                requiredBytes, &requiredBytes);
    CloseHandle(token);
    if (!ok) {
        return false;
    }

    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.constData());
    PSID sid = label->Label.Sid;
    if (sid == nullptr || !IsValidSid(sid)) {
        return false;
    }
    const DWORD count = *GetSidSubAuthorityCount(sid);
    return count != 0 &&
            *GetSidSubAuthority(sid, count - 1) < SECURITY_MANDATORY_MEDIUM_RID;
}

QString takeBstr(BSTR value)
{
    if (value == nullptr) {
        return QString();
    }
    const QString text = QString::fromWCharArray(value,
                                                 static_cast<int>(SysStringLen(value)));
    SysFreeString(value);
    return text;
}

bool sameFilePath(const QString& left, const QString& right)
{
    if (left.isEmpty() || right.isEmpty()) {
        return false;
    }

    const auto normalized = [](const QString& path) {
        const QString native = QDir::cleanPath(QDir::fromNativeSeparators(path));
        const QString canonical = QFileInfo(native).canonicalFilePath();
        return canonical.isEmpty() ? native : QDir::cleanPath(canonical);
    };

    return normalized(left).compare(normalized(right), Qt::CaseInsensitive) == 0;
}

struct PortRange
{
    int first;
    int last;
};

// Parse a firewall LocalPorts value such as "47984,47989,47990" or
// "47998-48010". Returns false for the textual port groups Windows also
// accepts ("RPC", "IPHTTPS", ...); such a rule is simply not a match.
bool parsePortRanges(const QString& text, QVector<PortRange>& ranges, bool& allPorts)
{
    allPorts = false;
    ranges.clear();

    const QStringList tokens = text.split(QRegularExpression(QStringLiteral("[,\\s]+")),
                                          Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (token == QStringLiteral("*")) {
            allPorts = true;
            continue;
        }

        const int dash = token.indexOf(QLatin1Char('-'));
        if (dash > 0) {
            bool firstOk = false;
            bool lastOk = false;
            const int first = token.left(dash).toInt(&firstOk);
            const int last = token.mid(dash + 1).toInt(&lastOk);
            if (!firstOk || !lastOk) {
                return false;
            }
            ranges.append(PortRange{ qMin(first, last), qMax(first, last) });
        }
        else {
            bool ok = false;
            const int port = token.toInt(&ok);
            if (!ok) {
                return false;
            }
            ranges.append(PortRange{ port, port });
        }
    }

    return true;
}

bool portsCovered(const QVector<PortRange>& ranges, bool allPorts,
                  const QVector<int>& required)
{
    if (allPorts) {
        return true;
    }

    for (int port : required) {
        bool covered = false;
        for (const PortRange& range : ranges) {
            if (port >= range.first && port <= range.last) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            return false;
        }
    }

    return true;
}

// Enumerate the effective inbound allow rules once and report the three rules
// the receiver needs:
//   1. fast path: a rule literally named LANProjection-App-Moonlight/TCP/UDP
//      that is enabled, inbound, allow and active for the current profile. The
//      program rule additionally has to point at this executable;
//   2. loose matching, so an equivalent rule the user created by hand is not
//      reported as missing: the application rule by executable path (including
//      the legacy LANProjection-App name when it points at us), the port rules
//      by TCP/UDP local ports covering the receiver's ports.
FirewallReport queryFirewallStatus(quint16 controlPort, quint16 discoveryPort)
{
    FirewallReport report;
    ComApartment apartment;

    auto fail = [&report](const QString& reason, HRESULT result = S_OK) {
        report.detectionFailed = true;
        report.failure = reason + QStringLiteral("；规则状态未知，不能据此判断未放行。");
        for (RuleCheck* check : { &report.app, &report.tcp, &report.udp }) {
            if (!check->present()) {
                check->status = RuleStatus::Failed;
            }
        }
        if (result == E_ACCESSDENIED) {
            if (firewallQueryRunsAtLowIntegrity()) {
                report.failure += QStringLiteral("当前进程为低完整性（low），防火墙查询被拒绝；请退出后从普通桌面/资源管理器以中完整性（medium）启动接收端，再检查规则。外部脚本放行不会改变当前进程的完整性级别。");
            }
            else {
                report.failure += QStringLiteral("Windows 拒绝了查询访问；请检查系统防火墙服务/策略及运行环境，拒绝访问本身不表示规则缺失。");
            }
        }
    };

    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) {
        fail(QStringLiteral("无法初始化防火墙查询 COM（%1）")
                     .arg(hresultText(apartment.result)), apartment.result);
        return report;
    }

    ComPtr<INetFwPolicy2> policy;
    HRESULT hr = CoCreateInstance(kClsidNetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER,
                                  kIidNetFwPolicy2,
                                  reinterpret_cast<void**>(&policy.ptr));
    if (FAILED(hr) || !policy) {
        fail(QStringLiteral("无法创建防火墙策略对象（%1）").arg(hresultText(hr)), hr);
        return report;
    }

    long currentProfiles = 0;
    hr = policy->get_CurrentProfileTypes(&currentProfiles);
    if (FAILED(hr)) {
        fail(QStringLiteral("无法读取当前网络配置文件（get_CurrentProfileTypes，%1）")
                     .arg(hresultText(hr)), hr);
        return report;
    }
    if (currentProfiles == 0) {
        fail(QStringLiteral("当前无可用网络配置文件（get_CurrentProfileTypes 返回 0）"));
        return report;
    }

    ComPtr<INetFwRules> rules;
    hr = policy->get_Rules(&rules.ptr);
    if (FAILED(hr) || !rules) {
        fail(QStringLiteral("无法枚举防火墙规则（%1）").arg(hresultText(hr)), hr);
        return report;
    }

    ComPtr<IUnknown> enumeratorUnknown;
    hr = rules->get__NewEnum(&enumeratorUnknown.ptr);
    if (FAILED(hr) || !enumeratorUnknown) {
        fail(QStringLiteral("无法枚举防火墙规则（%1）").arg(hresultText(hr)), hr);
        return report;
    }

    ComPtr<IEnumVARIANT> enumerator;
    hr = enumeratorUnknown->QueryInterface(kIidEnumVariant,
                                           reinterpret_cast<void**>(&enumerator.ptr));
    if (FAILED(hr) || !enumerator) {
        fail(QStringLiteral("无法枚举防火墙规则（%1）").arg(hresultText(hr)), hr);
        return report;
    }

    const QString executablePath = QCoreApplication::applicationFilePath();
    const QString expectedAppName = QString::fromWCharArray(kFirewallRuleApp);
    const QString expectedAppLegacyName = QString::fromWCharArray(kFirewallRuleAppLegacy);
    const QString expectedTcpName = QString::fromWCharArray(kFirewallRuleTcp);
    const QString expectedUdpName = QString::fromWCharArray(kFirewallRuleUdp);

    QVector<int> requiredTcpPorts;
    for (int port : kFixedTcpPorts) {
        requiredTcpPorts.append(port);
    }
    if (!requiredTcpPorts.contains(static_cast<int>(controlPort))) {
        requiredTcpPorts.append(static_cast<int>(controlPort));
    }

    QVector<int> requiredUdpPorts;
    if (!requiredUdpPorts.contains(static_cast<int>(discoveryPort))) {
        requiredUdpPorts.append(static_cast<int>(discoveryPort));
    }
    requiredUdpPorts.append(kStreamingUdpFirstPort);
    requiredUdpPorts.append(kStreamingUdpLastPort);

    for (;;) {
        VARIANT item;
        VariantInit(&item);
        ULONG fetched = 0;
        hr = enumerator->Next(1, &item, &fetched);
        if (hr == S_FALSE || fetched == 0) {
            VariantClear(&item);
            break;
        }
        if (FAILED(hr)) {
            VariantClear(&item);
            fail(QStringLiteral("枚举防火墙规则时出错（%1）").arg(hresultText(hr)), hr);
            return report;
        }
        if (item.vt != VT_DISPATCH || item.pdispVal == nullptr) {
            VariantClear(&item);
            continue;
        }

        ComPtr<INetFwRule> rule;
        hr = item.pdispVal->QueryInterface(kIidNetFwRule,
                                           reinterpret_cast<void**>(&rule.ptr));
        VariantClear(&item);
        if (FAILED(hr) || !rule) {
            continue;
        }

        // Only enabled, inbound, allow rules that apply to the current profile
        // can make the receiver reachable.
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(rule->get_Enabled(&enabled)) || enabled == VARIANT_FALSE) {
            continue;
        }

        NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
        if (FAILED(rule->get_Direction(&direction)) ||
            direction != NET_FW_RULE_DIR_IN) {
            continue;
        }

        NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
        if (FAILED(rule->get_Action(&action)) || action != NET_FW_ACTION_ALLOW) {
            continue;
        }

        long profiles = 0;
        if (FAILED(rule->get_Profiles(&profiles)) ||
            (profiles & currentProfiles) == 0) {
            continue;
        }

        BSTR nameValue = nullptr;
        if (FAILED(rule->get_Name(&nameValue))) {
            continue;
        }
        const QString ruleName = takeBstr(nameValue);

        // An empty ApplicationName means "any program"; a rule bound to a
        // different executable does not open the ports for us.
        QString applicationName;
        BSTR applicationValue = nullptr;
        if (SUCCEEDED(rule->get_ApplicationName(&applicationValue))) {
            applicationName = takeBstr(applicationValue);
        }
        const bool appliesToThisProgram = applicationName.isEmpty() ||
                                          sameFilePath(applicationName, executablePath);

        // The program rule only counts when it really allows this executable:
        // the legacy name is shared with the sender, which may have pointed it
        // at its own program.
        const bool rulePointsAtThisProgram = sameFilePath(applicationName, executablePath);

        if (!report.app.present() &&
            ruleName.compare(expectedAppName, Qt::CaseInsensitive) == 0) {
            if (rulePointsAtThisProgram) {
                report.app.status = RuleStatus::Present;
                report.app.matchedByFallback = false;
                report.app.matchedName = ruleName;
            }
            else {
                report.app.staleNamedRule = true;
            }
        }

        if (!report.app.present() && rulePointsAtThisProgram &&
            ruleName.compare(expectedAppLegacyName, Qt::CaseInsensitive) == 0) {
            report.app.status = RuleStatus::Present;
            report.app.matchedByFallback = true;
            report.app.matchedName = ruleName;
        }

        long protocol = -1;
        if (FAILED(rule->get_Protocol(&protocol))) {
            continue;
        }
        if (protocol != NET_FW_IP_PROTOCOL_TCP && protocol != NET_FW_IP_PROTOCOL_UDP) {
            continue;
        }

        BSTR portsValue = nullptr;
        if (FAILED(rule->get_LocalPorts(&portsValue))) {
            continue;
        }
        const QString localPorts = takeBstr(portsValue);

        QVector<PortRange> ranges;
        bool allPorts = false;
        if (!parsePortRanges(localPorts, ranges, allPorts)) {
            continue;
        }

        const bool isTcp = protocol == NET_FW_IP_PROTOCOL_TCP;
        RuleCheck& check = isTcp ? report.tcp : report.udp;
        if (check.present()) {
            continue;
        }

        const bool coversRequiredPorts =
                portsCovered(ranges, allPorts, isTcp ? requiredTcpPorts : requiredUdpPorts);
        const QString& expectedName = isTcp ? expectedTcpName : expectedUdpName;

        if (ruleName.compare(expectedName, Qt::CaseInsensitive) == 0) {
            // A rule with our name but stale ports (for example left over from
            // a receiver that used another control port) must not count as
            // "already allowed": report a missing rule instead.
            if (coversRequiredPorts) {
                check.status = RuleStatus::Present;
                check.matchedByFallback = false;
                check.matchedName = ruleName;
            }
            else {
                check.staleNamedRule = true;
            }
        }
        else if (appliesToThisProgram && coversRequiredPorts) {
            check.status = RuleStatus::Present;
            check.matchedByFallback = true;
            check.matchedName = ruleName;
        }
    }

    return report;
}

bool hasElevatedToken()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation = {};
    DWORD returnedBytes = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &returnedBytes);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// Append a line to the receiver log without needing a ReceiverService instance
// (used by the --fix-firewall helper, which runs as a short-lived process).
void appendToReceiverLog(const QString& message)
{
    QFile file(QDir(QCoreApplication::applicationDirPath())
                       .filePath(QStringLiteral("receiver-service.log")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << QDateTime::currentDateTime()
                      .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
               << " " << message << "\n";
    }

    // The log file is UTF-8 (QTextStream default), but a Windows console is
    // usually in the ANSI code page, so local 8-bit keeps Chinese readable
    // there without mangling the file.
    fprintf(stderr, "%s\n", message.toLocal8Bit().constData());
    fflush(stderr);
}

// The main window of a process, for "打开 Moonlight 界面" and for the second
// instance of "standby --ui". The tray owner window is excluded explicitly and
// the usual top-level/visible/titled filters keep helper windows out.
struct MainWindowSearch
{
    DWORD processId;
    HWND trayWindow;
    HWND found;
};

BOOL CALLBACK findProcessMainWindow(HWND hwnd, LPARAM parameter)
{
    auto* search = reinterpret_cast<MainWindowSearch*>(parameter);

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != search->processId || hwnd == search->trayWindow) {
        return TRUE;
    }

    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CHILD) != 0) {
        return TRUE;
    }
    const LONG_PTR extendedStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    if (GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }

    search->found = hwnd;
    return FALSE;
}

// ---- DLGTEMPLATE construction ---------------------------------------------

void appendWord(QByteArray& data, quint16 value)
{
    data.append(static_cast<char>(value & 0xFF));
    data.append(static_cast<char>((value >> 8) & 0xFF));
}

void appendDword(QByteArray& data, quint32 value)
{
    appendWord(data, static_cast<quint16>(value & 0xFFFF));
    appendWord(data, static_cast<quint16>(value >> 16));
}

void alignDword(QByteArray& data)
{
    while (data.size() % 4 != 0) {
        data.append('\0');
    }
}

void appendWideText(QByteArray& data, const QString& text)
{
    const wchar_t* value = reinterpret_cast<const wchar_t*>(text.utf16());
    data.append(reinterpret_cast<const char*>(value),
                (text.size() + 1) * static_cast<int>(sizeof(wchar_t)));
}

constexpr quint16 kButtonClassAtom = 0x0080;
constexpr quint16 kEditClassAtom = 0x0081;
constexpr quint16 kStaticClassAtom = 0x0082;

void appendControl(QByteArray& data, quint32 style, quint32 extendedStyle,
                   int x, int y, int width, int height, quint16 id,
                   quint16 classAtom, const QString& text)
{
    alignDword(data);
    appendDword(data, style);
    appendDword(data, extendedStyle);
    appendWord(data, static_cast<quint16>(x));
    appendWord(data, static_cast<quint16>(y));
    appendWord(data, static_cast<quint16>(width));
    appendWord(data, static_cast<quint16>(height));
    appendWord(data, id);
    appendWord(data, 0xFFFF); // next word is a predefined control class atom
    appendWord(data, classAtom);
    appendWideText(data, text);
    appendWord(data, 0); // no creation data
}

QByteArray buildSettingsDialogTemplate()
{
    QByteArray data;

    // Dialog units. DS_CENTER puts the dialog in the middle of the monitor
    // that owns the mouse cursor, which is right for a tray menu action.
    appendDword(data, static_cast<quint32>(WS_POPUP | WS_CAPTION | WS_SYSMENU |
                                           DS_MODALFRAME | DS_SETFONT | DS_CENTER));
    appendDword(data, 0);    // extended style
    appendWord(data, 0);     // number of controls, patched at the end
    appendWord(data, 0);     // x
    appendWord(data, 0);     // y
    appendWord(data, 320);   // cx
    appendWord(data, 262);   // cy
    appendWord(data, 0);     // no menu
    appendWord(data, 0);     // default window class
    appendWideText(data, QStringLiteral("局域网投屏 · 接收端设置"));

    // DS_SETFONT: point size, then the typeface. The point size must directly
    // follow the title; only the block that follows (the controls) needs
    // DWORD alignment.
    appendWord(data, 9);
    appendWideText(data, QStringLiteral("Microsoft YaHei UI"));
    alignDword(data);

    int controlCount = 0;
    const auto control = [&](quint32 style, int x, int y, int width, int height,
                             quint16 id, quint16 classAtom, const QString& text) {
        appendControl(data, style, 0, x, y, width, height, id, classAtom, text);
        controlCount++;
    };

    constexpr quint32 kLabelStyle = WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX;
    constexpr quint32 kEditStyle = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                                   ES_AUTOHSCROLL;
    constexpr quint32 kPortEditStyle = kEditStyle | ES_NUMBER;
    constexpr quint32 kButtonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;

    control(kLabelStyle, 10, 14, 80, 12, 0xFFFF, kStaticClassAtom,
            QStringLiteral("会议室名称："));
    control(kEditStyle, 92, 12, 218, 14, kSettingsNameEdit, kEditClassAtom, QString());

    control(kLabelStyle, 10, 38, 80, 12, 0xFFFF, kStaticClassAtom,
            QStringLiteral("控制端口(TCP)："));
    control(kPortEditStyle, 92, 36, 60, 14, kSettingsControlPortEdit, kEditClassAtom,
            QString());
    control(kLabelStyle, 170, 38, 88, 12, 0xFFFF, kStaticClassAtom,
            QStringLiteral("发现端口(UDP)："));
    control(kPortEditStyle, 260, 36, 50, 14, kSettingsDiscoveryPortEdit, kEditClassAtom,
            QString());

    control(kLabelStyle, 10, 58, 300, 12, 0xFFFF, kStaticClassAtom,
            QStringLiteral("提示：会议室名称保存后立即用于发现应答；端口修改需重启接收端。"));

    control(WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 10, 76, 300, 1, 0xFFFF,
            kStaticClassAtom, QString());

    control(kLabelStyle, 10, 84, 80, 12, 0xFFFF, kStaticClassAtom,
            QStringLiteral("运行状态"));

    control(kLabelStyle, 10, 102, 72, 12, 0xFFFF, kStaticClassAtom,
            QStringLiteral("进程完整性："));
    control(kLabelStyle, 84, 102, 226, 12, kSettingsIntegrityValue, kStaticClassAtom,
            QStringLiteral("—"));
    control(kLabelStyle, 10, 118, 72, 12, 0xFFFF, kStaticClassAtom,
            QStringLiteral("托盘图标："));
    control(kLabelStyle, 84, 118, 226, 12, kSettingsTrayValue, kStaticClassAtom,
            QStringLiteral("—"));

    control(kLabelStyle, 10, 136, 72, 12, 0xFFFF, kStaticClassAtom,
            QStringLiteral("防火墙："));
    control(kLabelStyle, 84, 136, 226, 12, kSettingsFirewallSummary, kStaticClassAtom,
            QStringLiteral("检测中…"));
    control(kLabelStyle, 84, 152, 226, 12, kSettingsFirewallAppValue, kStaticClassAtom,
            QString());
    control(kLabelStyle, 84, 168, 226, 12, kSettingsFirewallTcpValue, kStaticClassAtom,
            QString());
    control(kLabelStyle, 84, 184, 226, 12, kSettingsFirewallUdpValue, kStaticClassAtom,
            QString());

    control(kLabelStyle, 10, 202, 300, 26, kSettingsStatusText, kStaticClassAtom,
            QStringLiteral(" "));

    control(kButtonStyle | BS_DEFPUSHBUTTON, 8, 238, 58, 14, IDOK, kButtonClassAtom,
            QStringLiteral("保存"));
    control(kButtonStyle, 72, 238, 72, 14, kSettingsFixFirewallButton, kButtonClassAtom,
            QStringLiteral("放行防火墙"));
    control(kButtonStyle, 150, 238, 96, 14, kSettingsOpenGuiButton, kButtonClassAtom,
            QStringLiteral("打开 Moonlight 界面"));
    control(kButtonStyle, 252, 238, 58, 14, IDCANCEL, kButtonClassAtom,
            QStringLiteral("关闭"));

    // Patch cdit in the DLGTEMPLATE header (style + exStyle precede it).
    data[8] = static_cast<char>(controlCount & 0xFF);
    data[9] = static_cast<char>((controlCount >> 8) & 0xFF);

    return data;
}

QString readControlText(HWND dialog, int controlId)
{
    const HWND control = GetDlgItem(dialog, controlId);
    if (control == nullptr) {
        return QString();
    }

    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return QString();
    }

    QVector<wchar_t> buffer(length + 1, L'\0');
    const int copied = GetWindowTextW(control, buffer.data(), length + 1);
    return QString::fromWCharArray(buffer.constData(), copied);
}

void setControlText(HWND dialog, int controlId, const QString& text)
{
    SetDlgItemTextW(dialog, controlId, wide(text));
}

QString integrityLabel(const QString& level)
{
    if (level == QStringLiteral("low")) {
        return QStringLiteral("low（低完整性）");
    }
    if (level == QStringLiteral("medium")) {
        return QStringLiteral("medium（中完整性）");
    }
    if (level == QStringLiteral("high")) {
        return QStringLiteral("high（高完整性）");
    }
    return level.isEmpty() ? QStringLiteral("unknown") : level;
}

QString ruleText(const wchar_t* ruleName, const RuleCheck& check)
{
    const QString name = QString::fromWCharArray(ruleName);
    switch (check.status) {
    case RuleStatus::Present:
        return check.matchedByFallback
                ? (check.matchedName.isEmpty()
                           ? QStringLiteral("%1：已存在（等效规则）").arg(name)
                           : QStringLiteral("%1：已存在（等效规则：%2）")
                                     .arg(name, check.matchedName))
                : QStringLiteral("%1：已存在").arg(name);
    case RuleStatus::Failed:
        return QStringLiteral("%1：未知（检测失败）").arg(name);
    case RuleStatus::Missing:
    default:
        return check.staleNamedRule
                ? QStringLiteral("%1：缺失（同名规则未覆盖当前端口）").arg(name)
                : QStringLiteral("%1：缺失").arg(name);
    }
}

COLORREF ruleColor(RuleStatus status)
{
    switch (status) {
    case RuleStatus::Present:
        return RGB(0, 128, 0);
    case RuleStatus::Failed:
        return RGB(120, 120, 120);
    case RuleStatus::Missing:
    default:
        return RGB(190, 0, 0);
    }
}

} // namespace

// Lives outside the anonymous namespace because the dialog procedure below
// (a friend of ReceiverService) takes a pointer to it in its signature. The
// FirewallReport member has internal linkage, which is fine for a data member.
struct SettingsDialogState
{
    ReceiverService* service = nullptr;
    FirewallReport report;
    quint16 controlPort = 0;
    quint16 discoveryPort = 0;
    int timerTicks = 0;
    bool detectionFailureLogged = false;
};

// The dialog procedure and its small helpers. Declared as a friend of
// ReceiverService so it can inspect the loaded config, the tray state and the
// integrity level without widening ReceiverService's public API.
struct ReceiverServiceSettingsDialog
{
    static INT_PTR CALLBACK dialogProc(HWND dialog, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    static void refreshStatus(HWND dialog, SettingsDialogState* state);
    static void saveSettings(HWND dialog, SettingsDialogState* state);
    static void requestFirewallFix(HWND dialog, SettingsDialogState* state);
    static void openMainWindow(HWND dialog, SettingsDialogState* state);
    static void setStatus(HWND dialog, const QString& text);
};

INT_PTR CALLBACK ReceiverServiceSettingsDialog::dialogProc(HWND dialog, UINT message,
                                                           WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<SettingsDialogState*>(
                GetWindowLongPtrW(dialog, DWLP_USER));

    switch (message) {
    case WM_INITDIALOG:
        {
            state = reinterpret_cast<SettingsDialogState*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            if (state == nullptr || state->service == nullptr) {
                EndDialog(dialog, 0);
                return TRUE;
            }

            ReceiverService* service = state->service;

            // Re-read the file so the dialog always shows what will be used
            // after a restart, even if it was edited in the meantime.
            service->loadReceiverConfig();
            if (service->m_IntegrityLevel.isEmpty() ||
                service->m_IntegrityLevel == QStringLiteral("unknown")) {
                service->m_IntegrityLevel = ReceiverService::integrityLevelName();
            }

            setControlText(dialog, kSettingsNameEdit, service->displayName());
            SetDlgItemInt(dialog, kSettingsControlPortEdit,
                          service->m_Config.controlPort, FALSE);
            SetDlgItemInt(dialog, kSettingsDiscoveryPortEdit,
                          service->m_Config.discoveryPort, FALSE);
            SendDlgItemMessageW(dialog, kSettingsNameEdit, EM_SETLIMITTEXT,
                                static_cast<WPARAM>(kMaxDisplayNameLength), 0);

            refreshStatus(dialog, state);

            if (state->report.detectionFailed && !state->detectionFailureLogged) {
                state->detectionFailureLogged = true;
                service->log(QStringLiteral("ERROR: firewall detection failed: %1")
                                     .arg(state->report.failure));
                setStatus(dialog, QStringLiteral("防火墙检测失败：%1")
                                        .arg(state->report.failure));
            }

            SetFocus(GetDlgItem(dialog, kSettingsNameEdit));
            SendDlgItemMessageW(dialog, kSettingsNameEdit, EM_SETSEL, 0, -1);
            return FALSE; // focus was set explicitly
        }

    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (notification != BN_CLICKED && id != IDOK && id != IDCANCEL) {
                break;
            }

            if (id == IDOK) {
                saveSettings(dialog, state);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dialog, 0);
                return TRUE;
            }
            if (id == kSettingsFixFirewallButton) {
                requestFirewallFix(dialog, state);
                return TRUE;
            }
            if (id == kSettingsOpenGuiButton) {
                openMainWindow(dialog, state);
                return TRUE;
            }
            break;
        }

    case WM_TIMER:
        if (wParam == kSettingsRefreshTimer && state != nullptr) {
            state->timerTicks++;
            refreshStatus(dialog, state);
            if (state->report.allowed() || state->timerTicks >= 5) {
                KillTimer(dialog, kSettingsRefreshTimer);
            }
            return TRUE;
        }
        break;

    case WM_CTLCOLORSTATIC:
        if (state != nullptr) {
            const int controlId = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            RuleStatus status = RuleStatus::Present;
            bool colored = true;

            switch (controlId) {
            case kSettingsFirewallSummary:
                status = state->report.detectionFailed ? RuleStatus::Failed
                         : (state->report.allowed() ? RuleStatus::Present
                                                       : RuleStatus::Missing);
                break;
            case kSettingsFirewallAppValue:
                status = state->report.app.status;
                break;
            case kSettingsFirewallTcpValue:
                status = state->report.tcp.status;
                break;
            case kSettingsFirewallUdpValue:
                status = state->report.udp.status;
                break;
            default:
                colored = false;
                break;
            }

            if (colored) {
                const HDC deviceContext = reinterpret_cast<HDC>(wParam);
                SetTextColor(deviceContext, ruleColor(status));
                SetBkMode(deviceContext, TRANSPARENT);
                return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
            }
        }
        break;

    case WM_CLOSE:
        EndDialog(dialog, 0);
        return TRUE;

    default:
        break;
    }

    return FALSE;
}

void ReceiverServiceSettingsDialog::refreshStatus(HWND dialog, SettingsDialogState* state)
{
    ReceiverService* service = state->service;
    state->report = queryFirewallStatus(state->controlPort, state->discoveryPort);

    setControlText(dialog, kSettingsIntegrityValue,
                   integrityLabel(service->m_IntegrityLevel));
    setControlText(dialog, kSettingsTrayValue,
                   service->m_TrayIconAdded ? QStringLiteral("已创建")
                                            : QStringLiteral("不可用"));

    if (state->report.detectionFailed) {
        setControlText(dialog, kSettingsFirewallSummary, QStringLiteral("规则状态未知（检测失败）"));
        setControlText(dialog, kSettingsFirewallAppValue,
                       ruleText(kFirewallRuleApp, state->report.app));
        setControlText(dialog, kSettingsFirewallTcpValue,
                       ruleText(kFirewallRuleTcp, state->report.tcp));
        setControlText(dialog, kSettingsFirewallUdpValue,
                       ruleText(kFirewallRuleUdp, state->report.udp));
        return;
    }

    setControlText(dialog, kSettingsFirewallSummary,
                   state->report.allowed()
                   ? (state->report.app.present()
                      ? QStringLiteral("正常")
                      : QStringLiteral("正常（端口规则已覆盖）"))
                   : QStringLiteral("未放行"));
    setControlText(dialog, kSettingsFirewallAppValue,
                   ruleText(kFirewallRuleApp, state->report.app));
    setControlText(dialog, kSettingsFirewallTcpValue,
                   ruleText(kFirewallRuleTcp, state->report.tcp));
    setControlText(dialog, kSettingsFirewallUdpValue,
                   ruleText(kFirewallRuleUdp, state->report.udp));

    if (!state->report.allowed() || !state->report.app.present()) {
        // Keep one line per distinct result in the service log: the dialog
        // refreshes this on a timer while a fix may be in flight.
        static QString lastReport;
        const QString line = QStringLiteral("Firewall check: %1; %2; %3")
                                     .arg(ruleText(kFirewallRuleApp, state->report.app))
                                     .arg(ruleText(kFirewallRuleTcp, state->report.tcp))
                                     .arg(ruleText(kFirewallRuleUdp, state->report.udp));
        if (line != lastReport) {
            lastReport = line;
            service->log(line);
        }
    }
}

void ReceiverServiceSettingsDialog::setStatus(HWND dialog, const QString& text)
{
    setControlText(dialog, kSettingsStatusText, text);
}

void ReceiverServiceSettingsDialog::saveSettings(HWND dialog, SettingsDialogState* state)
{
    ReceiverService* service = state->service;
    const QString name = readControlText(dialog, kSettingsNameEdit).trimmed();

    if (name.isEmpty() || name.size() > kMaxDisplayNameLength) {
        setStatus(dialog, QStringLiteral("保存失败：会议室名称必须为 1-%1 个字符。")
                                   .arg(kMaxDisplayNameLength));
        return;
    }
    if (name.contains(QLatin1Char('"')) || name.contains(QLatin1Char('\''))) {
        setStatus(dialog, QStringLiteral("保存失败：名称不能包含引号。"));
        return;
    }
    if (hasControlCharacter(name)) {
        setStatus(dialog, QStringLiteral("保存失败：名称不能包含换行或控制字符。"));
        return;
    }

    BOOL controlOk = FALSE;
    BOOL discoveryOk = FALSE;
    const UINT controlPort = GetDlgItemInt(dialog, kSettingsControlPortEdit, &controlOk, FALSE);
    const UINT discoveryPort = GetDlgItemInt(dialog, kSettingsDiscoveryPortEdit,
                                             &discoveryOk, FALSE);
    if (!controlOk || controlPort < 1 || controlPort > 65535) {
        setStatus(dialog, QStringLiteral("保存失败：控制端口必须是 1-65535 之间的整数。"));
        return;
    }
    if (!discoveryOk || discoveryPort < 1 || discoveryPort > 65535) {
        setStatus(dialog, QStringLiteral("保存失败：发现端口必须是 1-65535 之间的整数。"));
        return;
    }

    QString error;
    if (!writeReceiverConfigFile(ReceiverService::configFilePath(), name,
                                 static_cast<quint16>(controlPort),
                                 static_cast<quint16>(discoveryPort), &error)) {
        setStatus(dialog, QStringLiteral("保存失败：%1").arg(error));
        service->log(QStringLiteral("ERROR: saving receiver.conf failed: %1").arg(error));
        return;
    }

    service->loadReceiverConfig();
    service->log(QStringLiteral("Settings saved from the receiver dialog: name '%1', "
                                "control port %2, discovery port %3")
                         .arg(name)
                         .arg(controlPort)
                         .arg(discoveryPort));

    const bool restartNeeded = controlPort != service->m_ControlPort ||
                               discoveryPort != service->m_DiscoveryPort;
    setStatus(dialog, restartNeeded
                      ? QStringLiteral("已保存。端口修改需重启接收端后生效。")
                      : QStringLiteral("已保存。"));
}

void ReceiverServiceSettingsDialog::requestFirewallFix(HWND dialog,
                                                       SettingsDialogState* state)
{
    if (firewallQueryRunsAtLowIntegrity()) {
        setStatus(dialog, QStringLiteral("当前接收端为低完整性（low），请先退出并从普通桌面/资源管理器以中完整性（medium）重新启动，再点击放行防火墙；外部脚本放行不会解除当前进程限制。"));
        return;
    }
    BOOL controlOk = FALSE;
    BOOL discoveryOk = FALSE;
    const UINT controlPort = GetDlgItemInt(dialog, kSettingsControlPortEdit, &controlOk, FALSE);
    const UINT discoveryPort = GetDlgItemInt(dialog, kSettingsDiscoveryPortEdit,
                                             &discoveryOk, FALSE);
    if (!controlOk || controlPort < 1 || controlPort > 65535 ||
        !discoveryOk || discoveryPort < 1 || discoveryPort > 65535) {
        setStatus(dialog, QStringLiteral("放行防火墙失败：请先填写有效的控制/发现端口。"));
        return;
    }

    // Relaunch this executable elevated with the internal helper switch. UAC
    // is the only privilege escalation path; if the user says no,
    // ShellExecuteW returns an error code and nothing is changed.
    const INT_PTR code = requestFirewallElevation(dialog, static_cast<quint16>(controlPort),
                                                  static_cast<quint16>(discoveryPort));
    if (code <= 32) {
        state->service->log(QStringLiteral("Firewall fix request failed to start: %1")
                                   .arg(win32ErrorText(static_cast<DWORD>(code))));
        if (code == ERROR_CANCELLED || code == SE_ERR_ACCESSDENIED) {
            setStatus(dialog, QStringLiteral("已取消：放行防火墙需要管理员权限（UAC 被拒绝）。"));
        }
        else {
            setStatus(dialog, QStringLiteral("无法启动管理员进程：%1")
                                       .arg(win32ErrorText(static_cast<DWORD>(code))));
        }
        return;
    }

    state->service->log(QStringLiteral("Firewall fix requested with administrator rights "
                                       "(control port %1, discovery port %2)")
                               .arg(controlPort)
                               .arg(discoveryPort));
    setStatus(dialog, QStringLiteral("已请求管理员权限，请在 UAC 提示中确认；"
                                     "规则创建后状态会自动刷新。"));

    // The elevated helper is a separate process; poll until the rules show up.
    state->timerTicks = 0;
    SetTimer(dialog, kSettingsRefreshTimer, 2000, nullptr);
}

void ReceiverServiceSettingsDialog::openMainWindow(HWND dialog, SettingsDialogState* state)
{
    if (!state->service->activateMainWindow()) {
        MessageBoxW(dialog, L"当前为后台模式，请用 standby --ui 启动。",
                    L"局域网投屏", MB_OK | MB_ICONINFORMATION);
    }
}

void ReceiverService::showSettingsDialog()
{
#if defined(_WIN32)
    if (m_TrayUiBusy) {
        return;
    }

    SettingsDialogState state;
    state.service = this;
    // The firewall must be checked for the ports the service actually uses;
    // when it is not running (--show-settings) the configured ports are what
    // the user is about to start with.
    state.controlPort = m_ControlServer != nullptr ? m_ControlPort : m_Config.controlPort;
    state.discoveryPort = m_DiscoverySocket != nullptr ? m_DiscoveryPort
                                                       : m_Config.discoveryPort;

    const QByteArray dialogTemplate = buildSettingsDialogTemplate();
    if (dialogTemplate.isEmpty()) {
        log(QStringLiteral("ERROR: settings dialog template could not be built"));
        return;
    }

    m_TrayUiBusy = true;
    const INT_PTR result = DialogBoxIndirectParamW(
                GetModuleHandleW(nullptr),
                reinterpret_cast<LPCDLGTEMPLATEW>(dialogTemplate.constData()),
                reinterpret_cast<HWND>(m_TrayHwnd),
                &ReceiverServiceSettingsDialog::dialogProc,
                reinterpret_cast<LPARAM>(&state));
    m_TrayUiBusy = false;

    if (result == -1) {
        log(QStringLiteral("ERROR: settings dialog could not be created (%1)")
                    .arg(win32ErrorText(static_cast<DWORD>(GetLastError()))));
    }

    updateTrayUi();
#endif
}

bool ReceiverService::showSettingsDialogStandalone()
{
#if defined(_WIN32)
    loadReceiverConfig();
    if (m_IntegrityLevel.isEmpty() ||
        m_IntegrityLevel == QStringLiteral("unknown")) {
        m_IntegrityLevel = integrityLevelName();
    }

    showSettingsDialog();
    return true;
#else
    return false;
#endif
}

bool ReceiverService::activateMainWindow()
{
#if defined(_WIN32)
    MainWindowSearch search = { GetCurrentProcessId(),
                                reinterpret_cast<HWND>(m_TrayHwnd),
                                nullptr };
    EnumWindows(&findProcessMainWindow, reinterpret_cast<LPARAM>(&search));

    if (search.found == nullptr) {
        log(QStringLiteral("No Moonlight GUI window found "
                           "(the receiver is running in background mode)"));
        return false;
    }

    if (IsIconic(search.found)) {
        ShowWindow(search.found, SW_RESTORE);
    }
    else {
        ShowWindow(search.found, SW_SHOW);
    }
    SetForegroundWindow(search.found);
    log(QStringLiteral("Brought the Moonlight main window to the foreground"));
    return true;
#else
    return false;
#endif
}

void ReceiverService::requestExistingInstanceWindow()
{
#if defined(_WIN32)
    // The hidden tray window of the running instance is a top-level window,
    // so a registered broadcast message reaches it. This is only an
    // activation hint: the second process still exits because the ports could
    // not be bound.
    const UINT message = RegisterWindowMessageW(L"LANProjection.ActivateMainWindow");
    if (message == 0) {
        return;
    }

    appendToReceiverLog(QStringLiteral("Asking any running receiver instance to show its main window"));

    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, message, 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &result);
#endif
}

bool ReceiverService::fixFirewall(quint16 controlPort, quint16 discoveryPort)
{
#if defined(_WIN32)
    appendToReceiverLog(QStringLiteral("--fix-firewall: control port %1, discovery port %2")
                                .arg(controlPort)
                                .arg(discoveryPort));

    if (!hasElevatedToken()) {
        appendToReceiverLog(QStringLiteral("ERROR: --fix-firewall needs administrator "
                                           "rights. Start it from the receiver settings "
                                           "dialog (\"放行防火墙\")."));
        return false;
    }

    ComApartment apartment;

    ComPtr<INetFwPolicy2> policy;
    HRESULT hr = CoCreateInstance(kClsidNetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER,
                                  kIidNetFwPolicy2,
                                  reinterpret_cast<void**>(&policy.ptr));
    if (FAILED(hr) || !policy) {
        appendToReceiverLog(QStringLiteral("ERROR: --fix-firewall: cannot create the "
                                           "firewall policy object (%1)")
                                    .arg(hresultText(hr)));
        return false;
    }

    ComPtr<INetFwRules> rules;
    hr = policy->get_Rules(&rules.ptr);
    if (FAILED(hr) || !rules) {
        appendToReceiverLog(QStringLiteral("ERROR: --fix-firewall: cannot open the "
                                           "firewall rules (%1)")
                                    .arg(hresultText(hr)));
        return false;
    }

    const QString executablePath =
            QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

    // The TCP/UDP rule names are shared with the sender. Merge the ports the
    // existing rule already allows instead of replacing them, so running this
    // helper on one side never drops the other side's port.
    const auto existingPortTokens = [&rules](const wchar_t* ruleName) -> QStringList {
        QStringList tokens;

        BSTR name = SysAllocString(ruleName);
        ComPtr<INetFwRule> rule;
        const HRESULT itemResult = rules->Item(name, &rule.ptr);
        SysFreeString(name);
        if (FAILED(itemResult) || !rule) {
            return tokens;
        }

        BSTR portsValue = nullptr;
        if (FAILED(rule->get_LocalPorts(&portsValue))) {
            return tokens;
        }

        const QString localPorts = takeBstr(portsValue);
        const QRegularExpression separator(QStringLiteral("[,;\\s]+"));
        const QRegularExpression portToken(
                QStringLiteral("^(\\*|\\d{1,5}|\\d{1,5}-\\d{1,5})$"));
        for (const QString& token : localPorts.split(separator, Qt::SkipEmptyParts)) {
            // Keep only tokens this helper can safely merge; service names or
            // port groups would be lost by a plain string replace.
            if (portToken.match(token).hasMatch()) {
                tokens.append(token);
            }
        }
        return tokens;
    };

    QStringList tcpPortList;
    for (int port : kFixedTcpPorts) {
        tcpPortList.append(QString::number(port));
    }
    if (!tcpPortList.contains(QString::number(controlPort))) {
        tcpPortList.append(QString::number(controlPort));
    }
    for (const QString& token : existingPortTokens(kFirewallRuleTcp)) {
        if (!tcpPortList.contains(token)) {
            tcpPortList.append(token);
        }
    }
    const QString tcpPorts = tcpPortList.contains(QStringLiteral("*"))
            ? QStringLiteral("*")
            : tcpPortList.join(QLatin1Char(','));

    QStringList udpPortList;
    udpPortList.append(QStringLiteral("%1-%2").arg(kStreamingUdpFirstPort)
                                              .arg(kStreamingUdpLastPort));
    if (discoveryPort < kStreamingUdpFirstPort || discoveryPort > kStreamingUdpLastPort) {
        udpPortList.append(QString::number(discoveryPort));
    }
    for (const QString& token : existingPortTokens(kFirewallRuleUdp)) {
        if (!udpPortList.contains(token)) {
            udpPortList.append(token);
        }
    }
    const QString udpPorts = udpPortList.contains(QStringLiteral("*"))
            ? QStringLiteral("*")
            : udpPortList.join(QLatin1Char(','));

    // Replace our own rules so a stale port list never lingers; unrelated
    // rules (including the user's own) are left untouched.
    const auto removeRule = [&rules](const wchar_t* ruleName) {
        BSTR name = SysAllocString(ruleName);
        const HRESULT removeResult = rules->Remove(name);
        SysFreeString(name);

        if (SUCCEEDED(removeResult)) {
            appendToReceiverLog(QStringLiteral("--fix-firewall: removed old rule %1")
                                        .arg(QString::fromWCharArray(ruleName)));
        }
        else if (removeResult != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
                 removeResult != HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS)) {
            appendToReceiverLog(QStringLiteral("--fix-firewall: could not remove %1 (%2), "
                                               "continuing")
                                        .arg(QString::fromWCharArray(ruleName))
                                        .arg(hresultText(removeResult)));
        }
    };

    removeRule(kFirewallRuleApp);
    removeRule(kFirewallRuleTcp);
    removeRule(kFirewallRuleUdp);

    // The bare LANProjection-App name is shared with the sender. Only delete it
    // when it still allows this executable; otherwise it belongs to the sender.
    {
        BSTR legacyName = SysAllocString(kFirewallRuleAppLegacy);
        ComPtr<INetFwRule> legacyRule;
        const HRESULT itemResult = rules->Item(legacyName, &legacyRule.ptr);
        SysFreeString(legacyName);

        if (SUCCEEDED(itemResult) && legacyRule) {
            BSTR application = nullptr;
            const bool pointsAtThisProgram =
                    SUCCEEDED(legacyRule->get_ApplicationName(&application)) &&
                    sameFilePath(takeBstr(application), executablePath);
            if (pointsAtThisProgram) {
                removeRule(kFirewallRuleAppLegacy);
            }
            else {
                appendToReceiverLog(QStringLiteral("--fix-firewall: keeping legacy rule %1 "
                                                   "(it points at another program)")
                                            .arg(QString::fromWCharArray(kFirewallRuleAppLegacy)));
            }
        }
    }

    // Create the rules through netsh instead of INetFwRule::Add(). The COM path
// returns 0x80070057 (E_INVALIDARG) on Windows 11 24H2 builds even with
// administrator rights, while netsh accepts the same values; netsh also gives
// us a readable message in the log when something is rejected.
    const auto runNetsh = [](const QStringList& arguments) -> bool {
        QProcess process;
        process.start(QStringLiteral("netsh.exe"), arguments);
        if (!process.waitForStarted(15000)) {
            appendToReceiverLog(QStringLiteral("ERROR: --fix-firewall: cannot start "
                                               "netsh.exe (%1)")
                                        .arg(process.errorString()));
            return false;
        }

        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished(5000);
            appendToReceiverLog(QStringLiteral("ERROR: --fix-firewall: netsh timed out"));
            return false;
        }

        const QString output =
                QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        const QString errors =
                QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;

        appendToReceiverLog(QStringLiteral("--fix-firewall: netsh %1 -> exit %2%3%4")
                                    .arg(arguments.join(QLatin1Char(' ')))
                                    .arg(process.exitCode())
                                    .arg(output.isEmpty() ? QString()
                                                          : QStringLiteral(" | out: ") + output)
                                    .arg(errors.isEmpty() ? QString()
                                                          : QStringLiteral(" | err: ") + errors));
        return ok;
    };

    const auto addRule = [&](const wchar_t* ruleName, long protocol, const QString& localPorts,
                             bool programScope) -> bool {
        QStringList arguments;
        arguments << QStringLiteral("advfirewall") << QStringLiteral("firewall")
                  << QStringLiteral("add") << QStringLiteral("rule")
                  << QStringLiteral("name=%1").arg(QString::fromWCharArray(ruleName))
                  << QStringLiteral("dir=in") << QStringLiteral("action=allow")
                  << QStringLiteral("enable=yes") << QStringLiteral("profile=any");

        // Shared port-only rules apply to both sender and receiver. Omitting
        // program= is netsh's "any program" default; an empty program= is invalid.
        if (programScope) {
            if (executablePath.isEmpty()) {
                appendToReceiverLog(QStringLiteral("ERROR: --fix-firewall: empty executable path for program rule"));
                return false;
            }
            arguments << QStringLiteral("program=%1").arg(executablePath);
        }

        if (programScope || protocol == NET_FW_IP_PROTOCOL_ANY) {
            arguments << QStringLiteral("protocol=any");
        }
        else {
            arguments << (protocol == NET_FW_IP_PROTOCOL_TCP ? QStringLiteral("protocol=TCP")
                                                             : QStringLiteral("protocol=UDP"))
                      << QStringLiteral("localport=%1").arg(localPorts);
        }

        if (!runNetsh(arguments)) {
            appendToReceiverLog(QStringLiteral("ERROR: --fix-firewall: adding rule %1 failed")
                                        .arg(QString::fromWCharArray(ruleName)));
            return false;
        }

        appendToReceiverLog(QStringLiteral("--fix-firewall: added rule %1 (protocol %2, "
                                           "ports '%3')")
                                    .arg(QString::fromWCharArray(ruleName))
                                    .arg(protocol)
                                    .arg(programScope ? QStringLiteral("<program>")
                                                      : localPorts));
        return true;
    };

    bool allOk = true;
    allOk = addRule(kFirewallRuleApp,
                    NET_FW_IP_PROTOCOL_ANY, QString(), true) && allOk;
    allOk = addRule(kFirewallRuleTcp,
                    NET_FW_IP_PROTOCOL_TCP, tcpPorts, false) && allOk;
    allOk = addRule(kFirewallRuleUdp,
                    NET_FW_IP_PROTOCOL_UDP, udpPorts, false) && allOk;

    appendToReceiverLog(allOk
                        ? QStringLiteral("--fix-firewall: all LANProjection rules created")
                        : QStringLiteral("ERROR: --fix-firewall: some rules could not be "
                                         "created (is the Windows Firewall service running?)"));
    return allOk;
#else
    Q_UNUSED(controlPort);
    Q_UNUSED(discoveryPort);
    return false;
#endif
}

#else // _WIN32

// The service also builds for POSIX. There is no notification area there, so
// the tray entry points are inert.

bool ReceiverService::createTray()
{
    return false;
}

void ReceiverService::destroyTray()
{
}

void ReceiverService::removeTrayIcon()
{
}

void ReceiverService::updateTrayUi()
{
}

void ReceiverService::onTrayCallback(unsigned int)
{
}

void ReceiverService::showTrayMenu()
{
}

void ReceiverService::handleTrayCommand(unsigned int)
{
}

void ReceiverService::showTrayStatus()
{
}

void ReceiverService::openLogDirectory()
{
}

// POSIX has no Win32 settings dialog or firewall COM API in this build.
void ReceiverService::showSettingsDialog()
{
}

bool ReceiverService::activateMainWindow()
{
    return false;
}

bool ReceiverService::showSettingsDialogStandalone()
{
    return false;
}

void ReceiverService::requestExistingInstanceWindow()
{
}

bool ReceiverService::fixFirewall(quint16 controlPort, quint16 discoveryPort)
{
    Q_UNUSED(controlPort);
    Q_UNUSED(discoveryPort);
    return false;
}

#endif // _WIN32

bool ReceiverService::saveRoomName(const QString& requestedName)
{
    const QString name = requestedName.trimmed();
    if (name.isEmpty() || name.size() > kMaxDisplayNameLength ||
        name.contains(QLatin1Char('"')) || name.contains(QLatin1Char('\'')) ||
        hasControlCharacter(name)) {
        m_SettingsMessage = QStringLiteral("保存失败：会议室名称必须为 1-%1 个字符且不能包含引号或控制字符。")
                .arg(kMaxDisplayNameLength);
        emit receiverSettingsChanged();
        return false;
    }

    QString error;
    if (!writeReceiverConfigFile(configFilePath(), name, m_Config.controlPort,
                                 m_Config.discoveryPort, &error, true)) {
        m_SettingsMessage = QStringLiteral("保存失败：%1").arg(error);
        log(QStringLiteral("ERROR: saving receiver.conf failed: %1").arg(error));
        emit receiverSettingsChanged();
        return false;
    }

    m_DisplayName = name;
    m_Config.name = name;
    m_Config.present = true;
    m_SettingsMessage = QStringLiteral("已保存会议室名称：%1").arg(name);
    updateTrayUi();
    emit receiverSettingsChanged();
    return true;
}

QString ReceiverService::refreshFirewallStatus()
{
#if defined(_WIN32)
    const quint16 controlPort = m_ControlServer != nullptr ? m_ControlPort : m_Config.controlPort;
    const quint16 discoveryPort = m_DiscoverySocket != nullptr ? m_DiscoveryPort : m_Config.discoveryPort;
    const FirewallReport report = queryFirewallStatus(controlPort, discoveryPort);
    if (report.detectionFailed) {
        m_FirewallStatus = QStringLiteral("防火墙规则状态未知：%1（不代表端到端连通性。）")
                .arg(report.failure);
    }
    else if (report.allowed()) {
        m_FirewallStatus = QStringLiteral("防火墙已放行（控制 TCP %1、发现 UDP %2；仅表示本机规则状态，不代表端到端连通性。）")
                .arg(controlPort).arg(discoveryPort);
    }
    else {
        m_FirewallStatus = QStringLiteral("防火墙未完整放行（控制 TCP %1、发现 UDP %2；仅表示本机规则状态，不代表端到端连通性。）")
                .arg(controlPort).arg(discoveryPort);
    }
#else
    m_FirewallStatus = QStringLiteral("当前平台不支持 Windows 防火墙检测（不代表端到端连通性）。");
#endif
    emit receiverSettingsChanged();
    return m_FirewallStatus;
}

QString ReceiverService::allowFirewall()
{
#if defined(_WIN32)
    if (firewallQueryRunsAtLowIntegrity()) {
        m_SettingsMessage = QStringLiteral("当前接收端为低完整性（low），请先退出并从普通桌面/资源管理器以中完整性（medium）重新启动，再点击放行防火墙；外部脚本放行不会解除当前进程限制。");
        emit receiverSettingsChanged();
        return m_SettingsMessage;
    }
    const quint16 controlPort = m_ControlServer != nullptr ? m_ControlPort : m_Config.controlPort;
    const quint16 discoveryPort = m_DiscoverySocket != nullptr ? m_DiscoveryPort : m_Config.discoveryPort;
    const INT_PTR code = requestFirewallElevation(nullptr, controlPort, discoveryPort);
    if (code <= 32) {
        m_SettingsMessage = QStringLiteral("无法请求管理员权限放行防火墙：%1").arg(win32ErrorText(static_cast<DWORD>(code)));
        emit receiverSettingsChanged();
        return m_SettingsMessage;
    }
    m_SettingsMessage = QStringLiteral("已请求管理员权限放行防火墙，状态将自动刷新。");
    m_FirewallRefreshAttempts = 5;
    m_FirewallRefreshTimer->start();
#else
    m_SettingsMessage = QStringLiteral("当前平台不支持 Windows 防火墙放行。");
#endif
    emit receiverSettingsChanged();
    return m_SettingsMessage;
}

