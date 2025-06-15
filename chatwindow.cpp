#include "chatwindow.h"
#include "ui_chatwindow.h"
#include <QMediaDevices>
#include <QVideoFrame>
#include <QUuid>
#include <QRandomGenerator>
#include <QNetworkInterface>
#include <QMessageBox>
#include <QDateTime>
#include <QBuffer>

ChatWindow::ChatWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::ChatWindow)
{
    ui->setupUi(this);
    setWindowTitle("Видеочат с улучшенной обработкой аудио");

    // Инициализация
    instanceId = QUuid::createUuid().toString();
    localNickname = "User_" + QString::number(QRandomGenerator::global()->bounded(1000));

    // Настройка сети
    udpSocket = new QUdpSocket(this);
    if (!udpSocket->bind(localPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        logMessage("Ошибка привязки сокета: " + udpSocket->errorString());
    }

    // Настройка таймеров
    setupTimers();

    // Настройка аудио/видео
    setupAudioVideo();

    // Кнопка статуса
    setupStatusButton();

    logMessage("Система готова. Ваш ник: " + localNickname);
    QTimer::singleShot(1000, this, &ChatWindow::sendDiscover);
}

ChatWindow::~ChatWindow()
{
    cleanupAudio();
    if (camera) {
        camera->stop();
        delete camera;
    }
    delete ui;
}

int ChatWindow::calculateAudioPacketSize() const
{
    return (audioFormat.sampleRate() * audioFormat.bytesPerFrame() * AUDIO_PACKET_MS) / 1000;
}

void ChatWindow::setupTimers()
{
    connectionTimer = new QTimer(this);
    connect(connectionTimer, &QTimer::timeout, this, [this](){
        if (isRemotePeerFound && ++missedPings > MAX_MISSED_PINGS) {
            logMessage("Таймаут соединения с " + remoteNickname);
            resetConnection();
        }
    });
    connectionTimer->start(5000);

    keepAliveTimer = new QTimer(this);
    connect(keepAliveTimer, &QTimer::timeout, this, &ChatWindow::sendKeepAlive);
    keepAliveTimer->start(2000);

    connect(udpSocket, &QUdpSocket::readyRead, this, &ChatWindow::readPendingDatagrams);
}

void ChatWindow::setupAudioVideo()
{
    initAudioDevices();
    initVideoDevices();
}

void ChatWindow::initAudioDevices()
{
    cleanupAudio();

    // Универсальный формат аудио
    audioFormat.setSampleRate(8000);
    audioFormat.setChannelCount(1);
    audioFormat.setSampleFormat(QAudioFormat::Int16);

    // Получаем устройства
    QAudioDevice inputDevice = QMediaDevices::defaultAudioInput();
    QAudioDevice outputDevice = QMediaDevices::defaultAudioOutput();

    // Проверяем и корректируем формат
    if (!inputDevice.isFormatSupported(audioFormat)) {
        audioFormat = inputDevice.preferredFormat();
        logMessage("Используется входной формат: " +
                   QString::number(audioFormat.sampleRate()) + "Hz, " +
                   QString::number(audioFormat.channelCount()) + " каналов");
    }

    // Приводим выходной формат к входному
    if (!outputDevice.isFormatSupported(audioFormat)) {
        QAudioFormat preferredOutFormat = outputDevice.preferredFormat();
        preferredOutFormat.setSampleRate(audioFormat.sampleRate());
        preferredOutFormat.setChannelCount(audioFormat.channelCount());

        if (outputDevice.isFormatSupported(preferredOutFormat)) {
            audioFormat = preferredOutFormat;
        } else {
            audioFormat = outputDevice.preferredFormat();
        }
        logMessage("Корректировка выходного формата");
    }

    audioBufferSize = calculateAudioPacketSize();

    // Инициализация входа
    audioInput = new QAudioSource(inputDevice, audioFormat, this);
    audioInput->setBufferSize(audioBufferSize * 2);
    audioInputDevice = audioInput->start();
    connect(audioInputDevice, &QIODevice::readyRead, this, &ChatWindow::sendAudioData);

    // Инициализация выхода
    audioOutput = new QAudioSink(outputDevice, audioFormat, this);
    audioOutput->setBufferSize(audioBufferSize * 4);
    audioOutputDevice = audioOutput->start();
}

void ChatWindow::initVideoDevices()
{
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    if (!cameras.isEmpty()) {
        camera = new QCamera(cameras.first(), this);
        captureSession = new QMediaCaptureSession(this);
        captureSession->setCamera(camera);

        videoSink = new QVideoSink(this);
        captureSession->setVideoOutput(videoSink);
        connect(videoSink, &QVideoSink::videoFrameChanged, this, &ChatWindow::videoFrameReady);

        camera->start();
    } else {
        logMessage("Камера не обнаружена");
    }
}

void ChatWindow::setupStatusButton()
{
    QPushButton *statusBtn = new QPushButton("Статус", this);
    connect(statusBtn, &QPushButton::clicked, this, &ChatWindow::showStatus);
    ui->verticalLayout->addWidget(statusBtn);
}

void ChatWindow::cleanupAudio()
{
    if (audioInput) {
        audioInput->stop();
        delete audioInput;
        audioInput = nullptr;
    }
    if (audioOutput) {
        audioOutput->stop();
        delete audioOutput;
        audioOutput = nullptr;
    }
    audioInputDevice = nullptr;
    audioOutputDevice = nullptr;
}

void ChatWindow::sendAudioData()
{
    if (!isRemotePeerFound || !audioInputDevice) return;

    const int packetSize = calculateAudioPacketSize();

    while (audioInputDevice->bytesAvailable() >= packetSize) {
        QByteArray audioData = audioInputDevice->read(packetSize);
        if (audioData.isEmpty() || audioData.size() != packetSize) {
            continue;
        }

        QByteArray packet;
        QDataStream stream(&packet, QIODevice::WriteOnly);
        stream << QString("AUDIO") << instanceId << localNickname << audioData;

        udpSocket->writeDatagram(packet, remoteAddress, remotePort);
    }
}

void ChatWindow::readPendingDatagrams()
{
    while (udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = udpSocket->receiveDatagram();
        if (!datagram.isValid() || isLocalAddress(datagram.senderAddress())) {
            continue;
        }

        QByteArray data = datagram.data();
        QDataStream stream(data);
        QString msgType;
        stream >> msgType;

        if (msgType == "AUDIO") {
            processAudioPacket(stream);
        }
        else if (msgType == "DISCOVER") {
            processDiscoverPacket(stream, datagram.senderAddress());
        }
        else if (msgType == "DISCOVER_REPLY") {
            processDiscoverReply(stream, datagram.senderAddress());
        }
        else if (msgType == "KEEPALIVE") {
            processKeepAlive(stream, datagram.senderAddress());
        }
        else if (msgType == "VIDEO") {
            processVideoPacket(stream);
        }
        else if (msgType == "MSG") {
            processTextMessage(stream);
        }
    }
}

void ChatWindow::processAudioPacket(QDataStream &stream)
{
    QString id, name;
    QByteArray audioData;
    stream >> id >> name >> audioData;

    if (id == instanceId || !audioOutputDevice) return;

    int expectedSize = calculateAudioPacketSize();
    if (abs(audioData.size() - expectedSize) > (expectedSize * 0.1)) {
        logMessage("Некорректный размер аудио пакета. Получено: " +
                   QString::number(audioData.size()) + ", ожидалось: " +
                   QString::number(expectedSize));
        return;
    }

    QMutexLocker locker(&audioMutex);
    audioOutputDevice->write(audioData.constData(), qMin(audioData.size(), expectedSize));
}

void ChatWindow::processDiscoverPacket(QDataStream &stream, const QHostAddress &senderAddr)
{
    QString id, name;
    stream >> id >> name;

    if (id == instanceId) return;

    QByteArray reply;
    QDataStream replyStream(&reply, QIODevice::WriteOnly);
    replyStream << QString("DISCOVER_REPLY") << instanceId << localNickname;
    udpSocket->writeDatagram(reply, senderAddr, remotePort);

    remoteAddress = senderAddr;
    remoteNickname = name;
    isRemotePeerFound = true;
    missedPings = 0;
    logMessage("Обнаружен участник: " + name + " (" + senderAddr.toString() + ")");
}

void ChatWindow::processDiscoverReply(QDataStream &stream, const QHostAddress &senderAddr)
{
    QString id, name;
    stream >> id >> name;

    if (id == instanceId) return;

    remoteAddress = senderAddr;
    remoteNickname = name;
    isRemotePeerFound = true;
    missedPings = 0;
    logMessage("Подключено к участнику: " + name + " (" + senderAddr.toString() + ")");
}

void ChatWindow::processKeepAlive(QDataStream &stream, const QHostAddress &senderAddr)
{
    QString id, name;
    stream >> id >> name;

    if (id == instanceId) return;

    missedPings = 0;
    if (!isRemotePeerFound || remoteAddress != senderAddr) {
        remoteAddress = senderAddr;
        remoteNickname = name;
        isRemotePeerFound = true;
    }
}

void ChatWindow::processVideoPacket(QDataStream &stream)
{
    QString id, name;
    QByteArray imageData;
    stream >> id >> name >> imageData;

    if (id == instanceId) return;

    QImage image;
    if (image.loadFromData(imageData, "JPEG")) {
        QPixmap pixmap = QPixmap::fromImage(image.scaled(ui->remoteVideoLabel->size(),
                                                         Qt::KeepAspectRatio, Qt::SmoothTransformation));
        ui->remoteVideoLabel->setPixmap(pixmap);
    }
}

void ChatWindow::processTextMessage(QDataStream &stream)
{
    QString id, name, text;
    stream >> id >> name >> text;

    if (id != instanceId) {
        ui->chatArea->append("<b>" + name + ":</b> " + text);
    }
}

void ChatWindow::sendDiscover()
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream << QString("DISCOVER") << instanceId << localNickname;

    udpSocket->writeDatagram(data, QHostAddress::Broadcast, localPort);

    foreach (const QNetworkInterface &interface, QNetworkInterface::allInterfaces()) {
        if (interface.flags() & QNetworkInterface::CanBroadcast) {
            foreach (const QNetworkAddressEntry &entry, interface.addressEntries()) {
                if (!entry.broadcast().isNull()) {
                    udpSocket->writeDatagram(data, entry.broadcast(), localPort);
                }
            }
        }
    }
}

void ChatWindow::sendKeepAlive()
{
    if (isRemotePeerFound && !remoteAddress.isNull()) {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        stream << QString("KEEPALIVE") << instanceId << localNickname;
        udpSocket->writeDatagram(data, remoteAddress, remotePort);
    } else {
        sendDiscover();
    }
}

void ChatWindow::on_sendButton_clicked()
{
    QString text = ui->messageEdit->text().trimmed();
    if (text.isEmpty()) return;

    if (!isRemotePeerFound) {
        logMessage("Нет подключения к участнику");
        return;
    }

    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream << QString("MSG") << instanceId << localNickname << text;

    if (udpSocket->writeDatagram(packet, remoteAddress, remotePort) == -1) {
        logMessage("Ошибка отправки сообщения");
    } else {
        ui->chatArea->append("<b>Я:</b> " + text);
        ui->messageEdit->clear();
    }
}

void ChatWindow::showStatus()
{
    QString status = QString("Статус системы:\n"
                             "Соединение: %1\n"
                             "Участник: %2\n"
                             "IP: %3\n"
                             "Формат аудио: %4 Hz, %5 каналов\n")
                         .arg(isRemotePeerFound ? "Подключено" : "Не подключено")
                         .arg(remoteNickname)
                         .arg(remoteAddress.toString())
                         .arg(audioFormat.sampleRate())
                         .arg(audioFormat.channelCount());

    QMessageBox::information(this, "Статус системы", status);
}

void ChatWindow::resetConnection()
{
    isRemotePeerFound = false;
    remoteAddress = QHostAddress();
    remoteNickname.clear();
    missedPings = 0;

    logMessage("Соединение сброшено");
    ui->chatArea->append("<i>Соединение потеряно</i>");
}

bool ChatWindow::isLocalAddress(const QHostAddress &address)
{
    if (address.isLoopback()) return true;
    foreach (const QHostAddress &localAddress, QNetworkInterface::allAddresses()) {
        if (address == localAddress) {
            return true;
        }
    }
    return false;
}

void ChatWindow::logMessage(const QString &message)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    ui->debugArea->append("[" + timestamp + "] " + message);
    ui->debugArea->verticalScrollBar()->setValue(ui->debugArea->verticalScrollBar()->maximum());
}

void ChatWindow::videoFrameReady(const QVideoFrame &frame)
{
    // Локальное отображение
    QImage image = frame.toImage();
    if (!image.isNull()) {
        QPixmap pixmap = QPixmap::fromImage(image.scaled(ui->localVideoLabel->size(),
                                                         Qt::KeepAspectRatio, Qt::SmoothTransformation));
        ui->localVideoLabel->setPixmap(pixmap);
    }

    // Отправка удаленному участнику
    if (!isRemotePeerFound || remoteAddress.isNull()) return;

    image = image.scaled(640, 480, Qt::KeepAspectRatio);
    QByteArray imageData;
    QBuffer buffer(&imageData);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 80);

    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream << QString("VIDEO") << instanceId << localNickname << imageData;
    udpSocket->writeDatagram(packet, remoteAddress, remotePort);
}
