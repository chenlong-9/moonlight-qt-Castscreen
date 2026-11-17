// Isolated native regression runner. Production sources are linked unchanged.
#include <QtCore>
#include <QtNetwork>
#include <QtQml>
#include <QProcess>
#include <QReadWriteLock>
#include <QThreadPool>
#define private public
#include "backend/receiverservice.h"
#include "backend/computermanager.h"
#undef private
#include "backend/nvcomputer.h"
#include "backend/identitymanager.h"
#include "settings/streamingpreferences.h"
#include <functional>
#include <stdexcept>

QAtomicInt g_AsyncLoggingEnabled(0);
// MSVC encodes member access in symbol names. Alias only test references;
// the production translation units retain their original access and ABI.
#ifdef _MSC_VER
#pragma comment(linker, "/alternatename:?loadReceiverConfig@ReceiverService@@QEAAXXZ=?loadReceiverConfig@ReceiverService@@AEAAXXZ")
#pragma comment(linker, "/alternatename:?handleNewControlConnection@ReceiverService@@QEAAXXZ=?handleNewControlConnection@ReceiverService@@AEAAXXZ")
#pragma comment(linker, "/alternatename:?handleDiscoveryDatagrams@ReceiverService@@QEAAXXZ=?handleDiscoveryDatagrams@ReceiverService@@AEAAXXZ")
#pragma comment(linker, "/alternatename:?handleComputerAddCompleted@ReceiverService@@QEAAXVQString@@PEAVNvComputer@@_N2@Z=?handleComputerAddCompleted@ReceiverService@@AEAAXVQString@@PEAVNvComputer@@_N2@Z")
#pragma comment(linker, "/alternatename:?handlePairingCompleted@ReceiverService@@QEAAXPEAVNvComputer@@VQString@@@Z=?handlePairingCompleted@ReceiverService@@AEAAXPEAVNvComputer@@VQString@@@Z")
#pragma comment(linker, "/alternatename:?handleAutoPairTimeout@ReceiverService@@QEAAXXZ=?handleAutoPairTimeout@ReceiverService@@AEAAXXZ")
#pragma comment(linker, "/alternatename:?ensureComputerManager@ReceiverService@@QEAAXXZ=?ensureComputerManager@ReceiverService@@AEAAXXZ")
#pragma comment(linker, "/alternatename:?isComputerPaired@ReceiverService@@QEBA_NPEAVNvComputer@@@Z=?isComputerPaired@ReceiverService@@AEBA_NPEAVNvComputer@@@Z")
#pragma comment(linker, "/alternatename:?failAutoPair@ReceiverService@@QEAAXAEBVQString@@@Z=?failAutoPair@ReceiverService@@AEAAXAEBVQString@@@Z")
#endif

static int checks = 0;
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
    qInfo().noquote() << "PASS" << message;
}
static bool spin(const std::function<bool()>& predicate, int timeout = 5000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}
static NvComputer* host(ComputerManager& manager, const QString& uuid) {
    QSettings settings;
    settings.beginGroup("fixture");
    settings.setValue("name", uuid);
    settings.setValue("uuid", uuid);
    settings.setValue("manualaddress", "127.0.0.1");
    settings.setValue("srvcert", IdentityManager::get()->getCertificate());
    auto* computer = new NvComputer(settings);
    settings.endGroup();
    computer->activeAddress = NvAddress("127.0.0.1", 47989);
    computer->activeHttpsPort = 47984;
    computer->appVersion = "7.1.0";
    computer->state = NvComputer::CS_ONLINE;
    computer->pairState = NvComputer::PS_NOT_PAIRED;
    manager.m_KnownHosts[uuid] = computer;
    return computer;
}
static void pending(ReceiverService& receiver, NvComputer* computer) {
    receiver.m_AutoPairPhase = ReceiverService::AutoPairPhase::PairingHost;
    receiver.m_PairAddress = "127.0.0.1";
    receiver.m_PairHostName = "fixture";
    receiver.m_PendingPairComputer = computer;
    receiver.m_PairingTasks.insert(computer);
}
static QByteArray http(quint16 port, const QByteArray& path) {
    QTcpSocket socket;
    QByteArray result;
    QObject::connect(&socket, &QTcpSocket::readyRead, &socket, [&] { result += socket.readAll(); });
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    if (!spin([&] { return socket.state() == QAbstractSocket::ConnectedState; })) return {};
    socket.write("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
    spin([&] { result += socket.readAll(); return socket.state() == QAbstractSocket::UnconnectedState; });
    result += socket.readAll();
    if (!result.startsWith("HTTP/1.1 200")) qCritical() << "HTTP diagnostics" << port << socket.errorString() << result;
    return result;
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // The actual ReceiverService spawns applicationFilePath(). Record the
    // arguments and exit instead of starting a decoder/network stream.
    if (app.arguments().value(1) == "stream") {
        QFile marker(qEnvironmentVariable("RECEIVER_REGRESSION_CHILD"));
        if (!marker.open(QIODevice::WriteOnly | QIODevice::Append)) return 4;
        marker.write(app.arguments().mid(1).join('|').toUtf8() + '\n');
        return 0;
    }
    try {
        QTemporaryDir temporary(QCoreApplication::applicationDirPath() + "/settings-XXXXXX");
        if (!temporary.isValid()) qCritical() << temporary.errorString();
        check(temporary.isValid(), "temporary settings directory created");
        QCoreApplication::setOrganizationName("LANProjectionRegression");
        QCoreApplication::setApplicationName("ReceiverRegression");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, temporary.path());
        QSettings().setValue("detectnetworkblocking", false);
        const auto marker = temporary.filePath("child.txt");
        qputenv("RECEIVER_REGRESSION_CHILD", marker.toUtf8());
        IdentityManager::get();
        // Prevent the real compatibility fetcher from reaching the internet.
        const QNetworkProxy blockedProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1);
        QNetworkProxy::setApplicationProxy(blockedProxy);
        ComputerManager manager(StreamingPreferences::get());
        manager.m_CompatFetcher.m_Nam->setProxy(blockedProxy);
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        manager.m_Prefs->detectNetworkBlocking = false;
        ReceiverService receiver(nullptr, &manager);
        receiver.ensureComputerManager();
        auto* a = host(manager, "regression-current-uuid");
        auto* b = host(manager, "regression-unrelated-uuid");
        check(!receiver.isComputerPaired(a), "cached certificate alone is not paired");
        check(manager.flushHostsNow(), "normal host flush syncs successfully");
        check(!manager.flushHostsNow(0), "zero-timeout flush reports incomplete generation");
        check(manager.m_DelayedFlushResults.isEmpty(), "timed-out flush releases generation result");
        check(manager.flushHostsNow(), "later generation succeeds after an earlier timeout");

        // A regular file used as a settings ancestor deterministically rejects
        // writes without ACL changes or touching the user's registry/settings.
        QFile obstruction(temporary.filePath("settings-obstruction"));
        check(obstruction.open(QIODevice::WriteOnly), "isolated settings obstruction created");
        obstruction.close();
        const QString blockedSettings = obstruction.fileName() + "/blocked";
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, blockedSettings);
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, blockedSettings);
        {
            QSettings rejected;
            rejected.setValue("probe", true);
            rejected.sync();
            check(rejected.status() == QSettings::AccessError, "invalid settings ancestor produces AccessError");
        }
        bool firstFlush = true;
        bool secondFlush = true;
        auto* firstWaiter = QThread::create([&] { firstFlush = manager.flushHostsNow(); });
        auto* secondWaiter = QThread::create([&] { secondFlush = manager.flushHostsNow(); });
        firstWaiter->start();
        secondWaiter->start();
        firstWaiter->wait();
        secondWaiter->wait();
        delete firstWaiter;
        delete secondWaiter;
        check(!firstFlush && !secondFlush, "concurrent host flushes report AccessError as failure");
        check(manager.m_DelayedFlushResults.isEmpty(), "completed flush waiters release generation results");
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, temporary.path());
        check(manager.flushHostsNow(), "host flush recovers after a previous settings AccessError");

        if (app.arguments().contains(QStringLiteral("--settings-only"))) {
            const QString config = QCoreApplication::applicationDirPath() + "/receiver.conf";
            check(!QFile::exists(config), "isolated settings test has no pre-existing receiver config");
            QFile fixture(config);
            check(fixture.open(QIODevice::WriteOnly), "receiver config fixture opened");
            fixture.write("# keep comment\nname = Before\nport = 47997\ndiscovery_port = 47996\nforce_video_args = 1\nfuture_key = retain\n");
            fixture.close();
            receiver.loadReceiverConfig();
            int notifications = 0;
            QObject::connect(&receiver, &ReceiverService::receiverSettingsChanged, &app,
                             [&] { ++notifications; });
            check(receiver.saveRoomName(QString::fromUtf8("三楼会议室")), "native bridge saves UTF-8 room name");
            check(receiver.roomName() == QString::fromUtf8("三楼会议室"), "live room name updated");
            check(fixture.open(QIODevice::ReadOnly), "saved config readable");
            const QByteArray saved = fixture.readAll(); fixture.close();
            check(saved.contains("port = 47997") && saved.contains("discovery_port = 47996") &&
                  saved.contains("force_video_args = 1") && saved.contains("future_key = retain") &&
                  saved.contains("# keep comment"), "name save preserves ports unknown keys and comments");
            check(!receiver.saveRoomName("bad\nname"), "invalid room name rejected");
            check(receiver.roomName() == QString::fromUtf8("三楼会议室"), "failed save retains room name");
            receiver.loadReceiverConfig();
            check(receiver.roomName() == QString::fromUtf8("三楼会议室"), "room name survives config reload");
            check(!receiver.refreshFirewallStatus().isEmpty(), "read-only firewall bridge reports status");
            check(notifications >= 3, "QML settings change notifications emitted");
            check(fixture.remove(), "isolated receiver config fixture removed");
            check(QThreadPool::globalInstance()->waitForDone(5000), "settings-only tasks drained");
            qInfo() << "SETTINGS RESULT:" << checks << "checks passed";
            return 0;
        }

        if (app.arguments().contains(QStringLiteral("--persistence-only"))) {
            check(QThreadPool::globalInstance()->waitForDone(5000), "persistence-only tasks drained");
            qInfo() << "PERSISTENCE RESULT:" << checks << "checks passed";
            return 0;
        }

        // Pairing uses real CM/HTTP worker against loopback only. The local
        // endpoint intentionally fails the handshake, without trusting a cert.
        QTcpServer fakeHost;
        check(fakeHost.listen(QHostAddress::LocalHost, 0), "loopback host bound");
        QObject::connect(&fakeHost, &QTcpServer::newConnection, &app, [&] {
            while (auto* socket = fakeHost.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    socket->readAll();
                    const QByteArray body = "<root status_code=\"200\"><paired>0</paired></root>";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        a->activeAddress = NvAddress("127.0.0.1", fakeHost.serverPort());
        receiver.m_AutoPairPhase = ReceiverService::AutoPairPhase::AddingHost;
        receiver.m_AddHostPending = true;
        receiver.m_PairAddress = "127.0.0.1";
        receiver.handleComputerAddCompleted("127.0.0.2", b, true, false);
        check(receiver.m_AddHostPending, "unrelated addressed add completion ignored");
        receiver.handleComputerAddCompleted("127.0.0.1", a, true, false);
        check(receiver.m_AutoPairPhase == ReceiverService::AutoPairPhase::PairingHost,
              "fresh add transitions AddingHost to PairingHost");
        check(receiver.m_PendingPairComputer == a, "exact discovered host tracked");
        receiver.handlePairingCompleted(b, QString());
        check(receiver.m_PendingPairComputer == a && !receiver.m_Streaming,
              "unrelated pairing success does not launch stream");
        check(spin([&] { return receiver.m_PairingTasks.isEmpty(); }), "real failed loopback pairing worker completed");
        check(receiver.m_AutoPairPhase == ReceiverService::AutoPairPhase::None && !receiver.m_Streaming,
              "failed pairing returns idle without stream");
        check(!QFile::exists(marker), "failed pairing spawned no child");

        pending(receiver, a);
        receiver.m_AutoPairPhase = ReceiverService::AutoPairPhase::VerifyingPairing;
        receiver.handlePairingCompleted(a, "verification rejected");
        check(!receiver.m_Streaming && !QFile::exists(marker), "failed cached verification spawned no child");
        pending(receiver, a);
        receiver.handleAutoPairTimeout();
        check(receiver.m_PairingTasks.contains(a) && receiver.m_PendingPairComputer == nullptr,
              "timeout retains worker marker and clears request identity");
        check(!receiver.m_Streaming && !QFile::exists(marker), "timeout spawned no child");
        receiver.handlePairingCompleted(a, QString());
        check(receiver.m_PairingTasks.isEmpty() && !receiver.m_Streaming,
              "late success drains marker without launching stream");
        pending(receiver, a);
        receiver.failAutoPair("cancelled");
        receiver.handlePairingCompleted(a, QString());
        check(!receiver.m_Streaming, "cancelled request ignores late success");

        // Exercise actual flush, UUID lookup, certificate comparison and child
        // launch. Completion is injected; crypto handshake success is not faked.
        pending(receiver, a);
        receiver.handlePairingCompleted(a, QString());
        check(spin([&] { return QFileInfo(marker).size() > 0; }), "successful persisted completion launched child");
        QFile child(marker); child.open(QIODevice::ReadOnly);
        check(child.readAll() == "stream|regression-current-uuid|Desktop\n",
              "child selected exact UUID rather than duplicate address");
        QSettings disk;
        bool found = false;
        const int size = disk.beginReadArray("hosts");
        for (int i = 0; i < size; ++i) {
            disk.setArrayIndex(i);
            if (disk.value("uuid").toString() == a->uuid)
                found = QSslCertificate(disk.value("srvcert").toByteArray()).toDer() == a->serverCert.toDer();
        }
        disk.endArray();
        check(found, "exact UUID and certificate persisted in isolated INI");
        check(spin([&] { return !receiver.m_Streaming; }), "test child exited cleanly");

        // Bind the real control/discovery handlers on ephemeral ports without
        // start() tray/config side effects. Deleting a saved host must not stop
        // the shared manager or either receiver listener.
        receiver.m_ControlServer = new QTcpServer(&receiver);
        receiver.m_DiscoverySocket = new QUdpSocket(&receiver);
        check(receiver.m_ControlServer->listen(QHostAddress::LocalHost, 0), "control listener bound to loopback ephemeral port");
        check(receiver.m_DiscoverySocket->bind(QHostAddress(QHostAddress::LocalHost), quint16(0)), "discovery listener bound to loopback ephemeral port");
        receiver.m_ControlPort = receiver.m_ControlServer->serverPort();
        receiver.m_DiscoveryPort = receiver.m_DiscoverySocket->localPort();
        QObject::connect(receiver.m_ControlServer, &QTcpServer::newConnection,
                         &receiver, &ReceiverService::handleNewControlConnection);
        QObject::connect(receiver.m_DiscoverySocket, &QUdpSocket::readyRead,
                         &receiver, &ReceiverService::handleDiscoveryDatagrams);
        // Record a baseline before any deletion, without allowing an
        // environment failure to prevent the independent lifetime assertions.
        QUdpSocket baseline;
        const bool baselineBound = baseline.bind(QHostAddress(QHostAddress::LocalHost), quint16(0));
        const qint64 baselineSent = baseline.writeDatagram(QByteArray("LANPROJ/1 DISCOVER"),
            QHostAddress(QHostAddress::LocalHost), receiver.m_DiscoveryPort);
        const bool baselineReceived = spin([&] { return baseline.hasPendingDatagrams(); }, 2000);
        qInfo() << "UDP BASELINE before deletion: bound=" << baselineBound
                << "sent=" << baselineSent << "reply=" << baselineReceived
                << "client=" << baseline.localAddress() << baseline.localPort()
                << "receiverPort=" << receiver.m_DiscoveryPort
                << "error=" << baseline.errorString();
        if (baselineReceived) qInfo() << "UDP BASELINE payload:" << baseline.receiveDatagram().data();
        manager.deleteHost(b);
        check(spin([&] { return !manager.getComputers().contains(b); }), "real asynchronous host deletion completed");
        check(http(receiver.m_ControlPort, "/state").startsWith("HTTP/1.1 200"), "state HTTP remains responsive after host deletion");
        auto* leased = host(manager, "regression-deferred-delete");
        leased->activeAddress = NvAddress("127.0.0.1", fakeHost.serverPort());
        check(manager.tryPairHost(leased, "0000"), "real pairing worker acquired host lease");
        check(!manager.tryPairHost(leased, "0000"), "duplicate pairing rejected without second completion");
        manager.deleteHost(leased);
        check(manager.m_DeferredPairingDeletes.contains(leased) && manager.getComputers().contains(leased),
              "GUI deletion retains in-flight pairing record");
        bool safeCompletion = false;
        QObject::connect(&manager, &ComputerManager::pairingCompleted, &app,
                         [&](NvComputer* completed, QString) {
            if (completed == leased)
                safeCompletion = manager.m_PairingHosts.value(leased) == 1 && completed->uuid == "regression-deferred-delete";
        });
        check(spin([&] { return !manager.getComputers().contains(leased); }),
              "deferred GUI deletion completes after worker result");
        check(safeCompletion, "pair completion delivered while exact host still leased and readable");
        check(http(receiver.m_ControlPort, "/state").startsWith("HTTP/1.1 200"),
              "state HTTP survives deferred pairing deletion");
        QUdpSocket discovery;
        discovery.bind(QHostAddress(QHostAddress::LocalHost), quint16(0));
        check(discovery.writeDatagram(QByteArray("LANPROJ/1 DISCOVER"), QHostAddress(QHostAddress::LocalHost),
                                      receiver.m_DiscoveryPort) > 0, "loopback discovery query sent");
        // Windows event pumping can coalesce readyRead during nested waits.
        // Wait on the actual socket, then invoke the same production slot.
        if (!receiver.m_DiscoverySocket->hasPendingDatagrams())
            receiver.m_DiscoverySocket->waitForReadyRead(1000);
        if (receiver.m_DiscoverySocket->hasPendingDatagrams())
            receiver.handleDiscoveryDatagrams();
        if (!discovery.hasPendingDatagrams()) discovery.waitForReadyRead(1000);
        const bool udpReceived = spin([&] { return discovery.hasPendingDatagrams(); });
        if (!udpReceived) qCritical() << "UDP diagnostics" << discovery.errorString()
            << discovery.localAddress() << discovery.localPort() << receiver.m_DiscoverySocket->errorString()
            << receiver.m_DiscoverySocket->hasPendingDatagrams() << receiver.m_DiscoveryPort;
        check(udpReceived, "UDP discovery remains responsive after host deletion");
        check(!discovery.receiveDatagram().data().isEmpty(), "UDP discovery returned payload");
        receiver.stop();
        check(QThreadPool::globalInstance()->waitForDone(5000), "all local thread-pool tasks drained");
        qInfo() << "RESULT:" << checks << "checks passed";
        return 0;
    } catch (const std::exception& exception) {
        qCritical() << "RESULT:" << checks << "checks passed before failure; FAIL:" << exception.what();
        return 1;
    }
}
