#include "widget.h"
#include "ui_widget.h"
#include "character.h"
#include "message.h"
#include "utils.h"
#include "items.h"
#include "mobsParser.h"
#include "mob.h"

#if defined _WIN32 || defined WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

Widget::Widget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Widget),
    cmdPeer(new Player()),
    usedids(new bool[65536])
{
    tcpServer = new QTcpServer(this);
    udpSocket = new QUdpSocket(this);
    tcpReceivedDatas = new QByteArray();
    ui->setupUi(this);

    pingTimer = new QTimer(this);

    qsrand(QDateTime::currentMSecsSinceEpoch());
    srand(QDateTime::currentMSecsSinceEpoch());
}

/// Adds the message in the log, and sets it as the status message
void Widget::logStatusMessage(QString msg)
{
    ui->log->appendPlainText(msg);
    ui->status->setText(msg);
    ui->log->repaint();
    ui->status->repaint();
}

/// Adds the message to the log
void Widget::logMessage(QString msg)
{
    if (!logInfos)
        return;
    ui->log->appendPlainText(msg);
    ui->log->repaint();
}

/// Reads the config file (server.ini) and start the server accordingly
void Widget::startServer()
{
    ui->retranslateUi(this);

    logStatusMessage(tr("Private server v0.5.3-alpha2"));
#ifdef __APPLE__
    // this fixes the directory in OSX so we can use the relative CONFIGFILEPATH and etc properly
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFURLRef resourcesURL = CFBundleCopyBundleURL(mainBundle);
    char path[PATH_MAX];
    if (!CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8 *)path, PATH_MAX))
    {
        // error!
    }
    CFRelease(resourcesURL);
    // the path we get is to the .app folder, so we go up after we chdir
    chdir(path);
    chdir("..");
#endif
    lastNetviewId=0;
    lastId=1;

    /// Read config
    logStatusMessage(tr("Reading config file ..."));
    QSettings config(CONFIGFILEPATH, QSettings::IniFormat);
    loginPort = config.value("loginPort", 1031).toInt();
    gamePort = config.value("gamePort", 1039).toInt();
    maxConnected = config.value("maxConnected",128).toInt();
    maxRegistered = config.value("maxRegistered",2048).toInt();
    pingTimeout = config.value("pingTimeout", 15).toInt();
    pingCheckInterval = config.value("pingCheckInterval", 5000).toInt();
    logInfos = config.value("logInfosMessages", true).toBool();
    saltPassword = config.value("saltPassword", "Change Me").toString();
    enableSessKeyValidation = config.value("enableSessKeyValidation", true).toBool();
    enableLoginServer = config.value("enableLoginServer", true).toBool();
    enableGameServer = config.value("enableGameServer", true).toBool();
    enableMultiplayer = config.value("enableMultiplayer", true).toBool();
    syncInterval = config.value("syncInterval",DEFAULT_SYNC_INTERVAL).toInt();
    remoteLoginIP = config.value("remoteLoginIP", "127.0.0.1").toString();
    remoteLoginPort = config.value("remoteLoginPort", 1031).toInt();
    remoteLoginTimeout = config.value("remoteLoginTimeout", 5000).toInt();
    useRemoteLogin = config.value("useRemoteLogin", false).toBool();
    enableGetlog = config.value("enableGetlog", true).toBool();

    /// Init servers
    tcpClientsList.clear();
#if defined _WIN32 || defined WIN32
    startTimestamp = GetTickCount();
#elif __APPLE__
    timeval time;
    gettimeofday(&time, NULL);
    startTimestamp = (time.tv_sec * 1000) + (time.tv_usec / 1000);
#else
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    startTimestamp = tp.tv_sec*1000 + tp.tv_nsec/1000/1000;
#endif

    /// Read vortex DB
    if (enableGameServer)
    {
        bool corrupted=false;
        QDir vortexDir("data/vortex/");
        QStringList files = vortexDir.entryList(QDir::Files);
        int nVortex=0;
        for (int i=0; i<files.size(); i++) // For each vortex file
        {
            // Each file is a scene
            Scene scene(files[i].split('.')[0]);

            QFile file("data/vortex/"+files[i]);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                logStatusMessage(tr("Error reading vortex DB"));
                return;
            }
            QByteArray data = file.readAll();
            data.replace('\r', "");
            QList<QByteArray> lines = data.split('\n');

            // Each line is a vortex
            for (int j=0; j<lines.size(); j++)
            {
                if (lines[j].size() == 0) // Skip empty lines
                    continue;
                nVortex++;
                Vortex vortex;
                bool ok1, ok2, ok3, ok4;
                QList<QByteArray> elems = lines[j].split(' ');
                if (elems.size() < 5)
                {
                    logStatusMessage(tr("Vortex DB is corrupted. Incorrect line (%1 elems), file %2")
                                        .arg(elems.size()).arg(files[i]));
                    corrupted=true;
                    break;
                }
                vortex.id = elems[0].toInt(&ok1, 16);
                vortex.destName = elems[1];
                for (int j=2; j<elems.size() - 3;j++) // Concatenate the string between id and poss
                    vortex.destName += " "+elems[j];
                vortex.destPos.x = elems[elems.size()-3].toFloat(&ok2);
                vortex.destPos.y = elems[elems.size()-2].toFloat(&ok3);
                vortex.destPos.z = elems[elems.size()-1].toFloat(&ok4);
                if (!(ok1&&ok2&&ok3&&ok4))
                {
                    logStatusMessage(tr("Vortex DB is corrupted. Conversion failed, file %1").arg(files[i]));
                    corrupted=true;
                    break;
                }
                scene.vortexes << vortex;
                //win.logMessage("Add vortex "+QString().setNum(vortex.id)+" to "+vortex.destName+" "
                //               +QString().setNum(vortex.destPos.x)+" "
                //               +QString().setNum(vortex.destPos.y)+" "
                //               +QString().setNum(vortex.destPos.z));
            }
            scenes << scene;
        }

        if (corrupted)
        {
            stopServer();
            return;
        }

        logMessage(tr("Loaded %1 vortexes in %2 scenes").arg(nVortex).arg(scenes.size()));
    }

    /// Read/parse Items.xml
    if (enableGameServer)
    {
        QFile itemsFile("data/data/Items.xml");
        if (itemsFile.open(QIODevice::ReadOnly))
        {
            QByteArray data = itemsFile.readAll();
            wearablePositionsMap = parseItemsXml(data);
            win.logMessage(tr("Loaded %1 items").arg(wearablePositionsMap.size()));
        }
        else
        {
            win.logMessage(tr("Couln't open Items.xml"));
            stopServer();
            return;
        }
    }

    /// Read NPC/Quests DB
    if (enableGameServer)
    {
        try
        {
            unsigned nQuests = 0;
            QDir npcsDir("data/npcs/");
            QStringList files = npcsDir.entryList(QDir::Files);
            for (int i=0; i<files.size(); i++, nQuests++) // For each vortex file
            {
                try
                {
                    Quest quest("data/npcs/"+files[i], NULL);
                    quests << quest;
                    npcs << quest.npc;
                }
                catch (QString& error)
                {
                    win.logMessage(error);
                    win.stopServer();
                    throw error;
                }
            }
            logMessage(tr("Loaded %1 quests/npcs").arg(nQuests));
        }
        catch (QString& e)
        {
            enableGameServer = false;
        }
    }

    /// Read/parse mob zones
    if (enableGameServer)
    {
        try
        {
            QDir mobsDir(MOBSPATH);
            QStringList files = mobsDir.entryList(QDir::Files);
            for (int i=0; i<files.size(); i++) // For each mobzone file
            {
                QFile file(MOBSPATH+files[i]);
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                {
                    logStatusMessage(tr("Error reading mob zones"));
                    return;
                }
                QByteArray data = file.readAll();
                file.close();
                try {
                    parseMobzoneData(data); // Will fill our Mobzone and Mobs list
                }
                catch (QString& error)
                {
                    win.logMessage(error);
                    win.stopServer();
                    throw error;
                }
            }
            logMessage(tr("Loaded %1 mobs in %2 zones").arg(mobs.size()).arg(mobzones.size()));
        }
        catch (...) {}
    }

    if (enableLoginServer)
    {
//      logStatusMessage(tr("Loading players database ..."));
        tcpPlayers = Player::loadPlayers();
    }

    // TCP server
    if (enableLoginServer)
    {
        logStatusMessage(tr("Starting TCP login server on port %1...").arg(loginPort));
        if (!tcpServer->listen(QHostAddress::Any,loginPort))
        {
            logStatusMessage(tr("TCP: Unable to start server on port %1 : %2").arg(loginPort).arg(tcpServer->errorString()));
            stopServer();
            return;
        }

        // If we use a remote login server, try to open a connection preventively.
        if (useRemoteLogin)
            remoteLoginSock.connectToHost(remoteLoginIP, remoteLoginPort);
    }

    // UDP server
    if (enableGameServer)
    {
        logStatusMessage(tr("Starting UDP game server on port %1...").arg(gamePort));
        if (!udpSocket->bind(gamePort, QUdpSocket::ReuseAddressHint|QUdpSocket::ShareAddress))
        {
            logStatusMessage(tr("UDP: Unable to start server on port %1").arg(gamePort));
            stopServer();
            return;
        }
    }

    if (enableGameServer)
    {
        // Start ping timeout timer
        pingTimer->start(pingCheckInterval);
    }

    if (enableMultiplayer)
        sync.startSync();

    if (enableLoginServer || enableGameServer)
        logStatusMessage(tr("Server started"));

    connect(ui->sendButton, SIGNAL(clicked()), this, SLOT(sendCmdLine()));
    if (enableLoginServer)
        connect(tcpServer, SIGNAL(newConnection()), this, SLOT(tcpConnectClient()));
    if (enableGameServer)
    {
        connect(udpSocket, SIGNAL(readyRead()),this, SLOT(udpProcessPendingDatagrams()));
        connect(pingTimer, SIGNAL(timeout()), this, SLOT(checkPingTimeouts()));
    }
}

int Widget::getNewNetviewId()
{
    int i;

    for (i = 0; i < 65536; i++) {
        usedids[i] = false;
    }

    for (int c = 0; c < npcs.size(); c++) {
        usedids[npcs[c]->netviewId] = true;
    }
    for (int c = 0; c < mobs.size(); c++) {
        usedids[mobs[c]->netviewId] = true;
    }
    for (int c = 0; c < udpPlayers.size(); c++) {
        usedids[udpPlayers[c]->pony.netviewId] = true;
    }

    i = 0;
    while (usedids[i]) i++;

    return i;
}

int Widget::getNewId()
{
    int i;

    for (i = 0; i < 65536; i++) {
        usedids[i] = false;
    }

    for (int c = 0; c < npcs.size(); c++) {
        usedids[npcs[c]->id] = true;
    }
    for (int c = 0; c < mobs.size(); c++) {
        usedids[mobs[c]->id] = true;
    }
    for (int c = 0; c < udpPlayers.size(); c++) {
        usedids[udpPlayers[c]->pony.id] = true;
    }

    i = 0;
    while (usedids[i]) i++;

    return i;
}

void Widget::stopServer()
{
    stopServer(true);
}

void Widget::stopServer(bool log)
{
    if (log)
        logStatusMessage(tr("Stopping all server operations"));
    pingTimer->stop();
    tcpServer->close();
    for (int i=0;i<tcpClientsList.size();i++)
        tcpClientsList[i].first->close();
    udpSocket->close();

    sync.stopSync();

    enableLoginServer = false;
    enableGameServer = false;

    disconnect(ui->sendButton, SIGNAL(clicked()), this, SLOT(sendCmdLine()));
    disconnect(udpSocket, SIGNAL(readyRead()),this, SLOT(udpProcessPendingDatagrams()));
    disconnect(tcpServer, SIGNAL(newConnection()), this, SLOT(tcpConnectClient()));
    disconnect(pingTimer, SIGNAL(timeout()), this, SLOT(checkPingTimeouts()));
    disconnect(this);
}

// Disconnect players, free the sockets, and exit quickly
// Does NOT run the atexits
Widget::~Widget()
{
    logInfos=false; // logMessage while we're trying to destroy would crash.
    //logMessage(tr("UDP: Disconnecting all players"));
    for (;udpPlayers.size();)
    {
        Player* player = udpPlayers[0];
        sendMessage(player, MsgDisconnect, "Connection closed by the server admin");

        // Save the pony
        QList<Pony> ponies = Player::loadPonies(player);
        for (int i=0; i<ponies.size(); i++)
            if (ponies[i].ponyData == player->pony.ponyData)
                ponies[i] = player->pony;
        Player::savePonies(player, ponies);
        player->pony.saveInventory();
        player->pony.saveQuests();

        // Free
        delete player;
        udpPlayers.removeFirst();
    }

    for (int i=0; i<quests.size(); i++)
    {
        delete quests[i].commands;
        delete quests[i].name;
        delete quests[i].descr;
    }

    stopServer(false);
    delete tcpServer;
    delete tcpReceivedDatas;
    delete udpSocket;
    delete pingTimer;
    delete cmdPeer;
    delete[] usedids;

    delete ui;

    // We freed everything that was important, so don't waste time in atexits
#if defined WIN32 || defined _WIN32 || defined __APPLE__
    _exit(EXIT_SUCCESS);
#else
    quick_exit(EXIT_SUCCESS);
#endif
}
