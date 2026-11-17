#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QString>

class ComputerManager;
class NvComputer;
class QTcpServer;
class QTcpSocket;
class QTimer;
class QUdpSocket;

/**
 * @brief Headless receiver (standby) service.
 *
 * Runs without any GUI or QML: it listens for control requests over TCP,
 * answers LAN discovery probes over UDP, and launches a streaming child
 * process of Moonlight.exe when the sender asks for it.
 *
 * If the sender's computer is not paired yet, the service pairs with it
 * automatically (using the derived enterprise PIN) before starting the
 * stream. The whole handshake is asynchronous: the HTTP request stays
 * pending until pairing completes, fails, or times out.
 *
 * Only QtCore/QtNetwork/QProcess are used, so the service can run before
 * any QGuiApplication/QQmlApplicationEngine exists.
 */
class ReceiverService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString roomName READ roomName NOTIFY receiverSettingsChanged)
    Q_PROPERTY(QString receiverStatus READ receiverStatus NOTIFY receiverSettingsChanged)
    Q_PROPERTY(QString firewallStatus READ firewallStatus NOTIFY receiverSettingsChanged)
    Q_PROPERTY(QString settingsMessage READ settingsMessage NOTIFY receiverSettingsChanged)
    Q_PROPERTY(bool available READ available NOTIFY receiverSettingsChanged)

public:
    explicit ReceiverService(QObject* parent = nullptr, ComputerManager* computerManager = nullptr);
    ~ReceiverService() override;

    /**
     * @brief Start listening for control and discovery traffic.
     * @return false if either port cannot be bound (reason is logged).
     */
    bool start();

    /**
     * @brief Stop the streaming child process (if any) and close the sockets.
     */
    void stop();

    /**
     * @brief Show the receiver settings dialog without starting the service.
     *
     * Standalone/headless fallback for inspecting the Win32 dialog and firewall
     * state without binding the control/discovery ports. GUI instances expose
     * these settings through QML. Returns false on platforms that have no
     * native dialog implementation.
     */
    bool showSettingsDialogStandalone();

    /**
     * @brief Create (or replace) the receiver's LANProjection-* firewall rules.
     *
     * The program rule is named "LANProjection-App-Moonlight" so the sender's
     * helper cannot overwrite it, while the port rules keep the shared
     * "LANProjection-TCP"/"LANProjection-UDP" names (their port lists are merged
     * with whatever an existing rule already allows). A legacy
     * "LANProjection-App" rule is removed only when it still points at this
     * executable. This is the body of the internal
     * "--fix-firewall <control> <discovery>" helper; it requires an elevated
     * token, the settings dialog starts it via ShellExecuteW("runas"). Returns
     * true only when all rules were added.
     */
    static bool fixFirewall(quint16 controlPort, quint16 discoveryPort);

    /**
     * @brief Ask a receiver that is already running to show its main window.
     *
     * The request is broadcast to the hidden tray window of the other
     * instance. Does nothing when no instance (or no GUI window) exists.
     */
    static void requestExistingInstanceWindow();

    QString roomName() const;
    QString receiverStatus() const;
    QString firewallStatus() const;
    QString settingsMessage() const;
    bool available() const;

    Q_INVOKABLE bool saveRoomName(const QString& name);
    Q_INVOKABLE QString refreshFirewallStatus();
    Q_INVOKABLE QString allowFirewall();

signals:
    void receiverSettingsChanged();
    void openNativeSettingsRequested();

private slots:
    void handleNewControlConnection();
    void handleControlReadyRead();
    void handleControlDisconnected();
    void handleDiscoveryDatagrams();
    void handleStreamProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleStreamProcessError(QProcess::ProcessError error);
    void handleComputerAddCompleted(QString address, NvComputer* computer,
                                    bool success, bool detectedPortBlocking);
    void handlePairingCompleted(NvComputer* computer, QString error);
    void handleAutoPairTimeout();

private:
    // The notification-area owner window procedure is implemented in
    // receiverservice.cpp. It is a friend so that this header does not have to
    // include <windows.h> (which would leak macros into every includer).
    friend struct ReceiverServiceTrayWindow;
    // The settings dialog is a native Win32 dialog implemented in
    // receiverservice.cpp; it needs the config/ports/tray state and must stay
    // out of this header so <windows.h> never leaks into includers.
    friend struct ReceiverServiceSettingsDialog;

    struct HttpRequest
    {
        QByteArray method;
        QByteArray path;
        QByteArray body;
    };

    // Phases of the automatic pairing handshake. While this is anything other
    // than None, the session is busy ("pairing" to /state) and /stop can still
    // cancel the handshake.
    enum class AutoPairPhase
    {
        None,
        AddingHost,
        PairingHost,
        // Re-running the pinless pairing against a host that already has a
        // pinned certificate. This is what keeps a stale pairing record from
        // producing a doomed streaming child ("computer has not been paired").
        VerifyingPairing,
    };

    // receiver.conf (a simple "key = value" file next to the executable).
    // "name" is the human-readable receiver name shown in discovery replies,
    // /state and the tray UI; the ports override the built-in defaults.
    struct ReceiverConfig
    {
        bool present;
        QString name;
        quint16 controlPort;
        quint16 discoveryPort;
        // "force_video_args = 1" keeps appending the historical fixed
        // --1080/--fps/--bitrate/--display-mode arguments to the streaming
        // child. Default off so the child honors the user's own Moonlight
        // StreamingPreferences (the settings page in --ui mode).
        bool forceVideoArgs;
    };

    static QString localHostName();
    static QString logFilePath();
    static QString configFilePath();

    void loadReceiverConfig();
    QString displayName() const;

    // Mandatory integrity level of this process ("low", "medium", "high" or
    // "unknown" when the token cannot be queried). rawRid receives the RID of
    // the last authority (0x1000 = low, 0x2000 = medium, 0x3000 = high).
    static QString integrityLevelName(int* rawRid = nullptr);
    void logIntegrityLevel();

    void log(const QString& message);
    void logStateChange(const QString& reason);

    // ---- Windows notification-area (tray) UI -----------------------------
    // Everything below runs on this object's thread and is driven by the
    // existing Qt event loop. The hidden Win32 window only receives the
    // WM_APP callback of the tray icon; no second message pump is created.
    void attemptTrayCreation(int attemptsLeft);
    bool createTray();
    void destroyTray();
    void removeTrayIcon();
    void updateTrayUi();
    void onTrayCallback(unsigned int event);
    void showTrayMenu();
    void showTrayStatus();
    void showSettingsDialog();
    bool activateMainWindow();
    void openLogDirectory();
    void quitFromTray();
    void handleTrayCommand(unsigned int command);

    QString serviceStateText() const;
    QString trayTooltipText() const;
    QString trayStatusDetails() const;

    quint16 resolvePort(const char* envVar, quint16 defaultPort);

    bool tryParseRequest(const QByteArray& buffer, HttpRequest& request, bool& malformed) const;
    QByteArray handleRequest(const HttpRequest& request, QTcpSocket* socket);
    QByteArray handleCast(const QByteArray& body, QTcpSocket* socket);
    QByteArray handleStop();

    void ensureComputerManager();
    NvComputer* findComputerByAddress(const QString& address) const;
    bool isComputerPaired(NvComputer* computer) const;
    bool startStream(const QString& host);

    void beginAutoPair(const QString& host, const QString& sender,
                       const QString& address, QTcpSocket* socket);
    bool startPairingWith(NvComputer* computer);
    // Reply to the pending /cast request and launch the exact persisted host.
    void finishCastRequest(const QString& address, const QString& hostId, bool reused);
    void failAutoPair(const QString& detail);
    void finishPendingRequest(int statusCode, const QJsonObject& body);

    QString stateString() const;
    QByteArray jsonResponse(int statusCode, const QJsonObject& body) const;

    QTcpServer* m_ControlServer;
    QUdpSocket* m_DiscoverySocket;
    quint16 m_ControlPort;
    quint16 m_DiscoveryPort;

    // Display name and ports resolved from receiver.conf at startup.
    ReceiverConfig m_Config;
    QString m_DisplayName;
    QString m_FirewallStatus = QStringLiteral("尚未检测防火墙规则。");
    QString m_SettingsMessage;
    QTimer* m_FirewallRefreshTimer;
    int m_FirewallRefreshAttempts = 0;

    // Buffers of the in-flight HTTP request per control connection
    QHash<QTcpSocket*, QByteArray> m_ControlBuffers;

    QProcess* m_StreamProcess;
    bool m_Streaming;
    QString m_StreamHost;

    // Auto-pairing state. Everything runs on the control server's thread; the
    // ComputerManager signals arrive through queued connections.
    ComputerManager* m_ComputerManager;
    bool m_OwnsComputerManager;
    bool m_ComputerManagerConnected = false;
    QTimer* m_AutoPairTimer;
    AutoPairPhase m_AutoPairPhase;
    NvComputer* m_PendingPairComputer = nullptr;
    // A timeout/cancel ends the HTTP request, not the thread-pool task. Keep
    // these markers until completion and do not reuse a still-running attempt.
    QSet<NvComputer*> m_PairingTasks;
    bool m_AddHostPending = false;
    QPointer<QTcpSocket> m_PendingSocket;
    QString m_PairHostName;
    QString m_PairAddress;
    QString m_PairSender;

    // Tray state. m_TrayHwnd is the hidden Win32 owner window, kept as an
    // integer so this header stays free of <windows.h>; 0 means "not created".
    quintptr m_TrayHwnd;
    bool m_TrayIconAdded;
    bool m_TrayUiBusy;
    bool m_TrayShowError;
    bool m_TrayRetryPending;
    int m_TrayAttempts;
    QString m_IntegrityLevel;
    QString m_LastError;
};