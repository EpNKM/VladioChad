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
#include <QElapsedTimer>
#include <QThread>

// Константы для аудио
const int MIN_PACKET_MS = 20;
const int MAX_PACKET_MS = 60;
const int TARGET_QUEUE_SIZE = 3;

ChatWindow::ChatWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::ChatWindow)
    , currentPacketMs(40)
    , packetLossRate(0.0)
    , totalPackets(0)
    , lostPackets(0)
    , lastSequence(-1)
    , isConnectionBad(false)
    , lastPacketTime(0)
    ,audioInput(nullptr), audioOutput(nullptr),
    camera(nullptr), captureSession(nullptr),
    videoSink(nullptr)
{
    ui->setupUi(this);
    setWindowTitle("VladioChat");

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
    logConnectionQuality();
    QTimer::singleShot(1000, this, &ChatWindow::sendDiscover);

    connect(ui->bufferSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            [this](int value) {
                maxVideoBufferSize = value;
                logMessage(QString("Установлен размер видео буфера: %1 кадров").arg(value));
            });
    ui->bufferSizeSpinBox->setRange(5, 30);
    ui->bufferSizeSpinBox->setValue(maxVideoBufferSize);
    videoSendTimer = new QTimer(this);
    connect(videoSendTimer, &QTimer::timeout, this, &ChatWindow::sendBufferedVideo);
    videoSendTimer->setInterval(500);

    connectionCheckTimer = new QTimer(this);
    connect(connectionCheckTimer, &QTimer::timeout, this, &ChatWindow::checkConnectionActivity);
    connectionCheckTimer->start(1000);
}

ChatWindow::~ChatWindow()
{
    if (videoSendTimer) {
        videoSendTimer->stop();
        delete videoSendTimer;
    }

    if (connectionCheckTimer) {
        connectionCheckTimer->stop();
        delete connectionCheckTimer;
    }

    cleanupAudio();

    if (camera) {
        camera->stop();
        delete camera;
    }

    if (captureSession) delete captureSession;
    if (videoSink) delete videoSink;

    delete ui;
}

void ChatWindow::checkConnectionActivity()
{
    if (!isRemotePeerFound) return;

    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    qint64 timeSinceLastPacket = currentTime - lastPacketTime;

    if (timeSinceLastPacket > 3000) { // 3 секунды без пакетов
        if (isRemotePeerFound) {
            logMessage("Обрыв соединения обнаружен по таймауту");
            resetConnection();
        }
    }
    else if (timeSinceLastPacket > 1000) { // 1 секунда без пакетов
        if (totalPackets > 0) {
            lostPackets++;
            updatePacketLossStats();
        }
    }

    if (isConnectionBad && timeSinceLastPacket < 1000 && packetLossRate < 10.0) {
        isConnectionBad = false;
        logMessage("Соединение восстановлено после проблем");

        // Запускаем отправку буферизированных кадров
        if (!videoBuffer.isEmpty()) {
            QTimer::singleShot(0, this, [this]() {
                QMutexLocker locker(&videoBufferMutex);
                logMessage(QString("Начинаем отправку %1 кадров из буфера")
                               .arg(videoBuffer.size()));

                while (!videoBuffer.isEmpty()) {
                    sendVideoFrameNow(videoBuffer.dequeue());
                    QThread::msleep(30); // Пауза для избежания перегрузки
                }
            });
        }
    }
}



void ChatWindow::addToVideoBuffer(const QByteArray &frameData)
{
    if (frameData.isEmpty()) return;

    QMutexLocker locker(&videoBufferMutex);
    while (videoBuffer.size() >= maxVideoBufferSize) {
        videoBuffer.dequeue();
    }
    videoBuffer.enqueue(frameData);
}

void ChatWindow::sendBufferedVideo()
{
    if (!isRemotePeerFound || remoteAddress.isNull()) {
        videoSendTimer->stop();
        return;
    }

    QMutexLocker locker(&videoBufferMutex);
    if (videoBuffer.isEmpty()) return;

    // Отправляем не более 2 кадров за раз
    int framesToSend = qMin(2, videoBuffer.size());
    for (int i = 0; i < framesToSend; i++) {
        QByteArray frameData = videoBuffer.dequeue();
        QByteArray packet;
        QDataStream stream(&packet, QIODevice::WriteOnly);
        stream << QString("VIDEO") << instanceId << localNickname << frameData;

        if (udpSocket->writeDatagram(packet, remoteAddress, remotePort) == -1) {
            // При ошибке возвращаем кадр в буфер
            videoBuffer.enqueue(frameData);
            logMessage("Ошибка отправки видео из буфера");
            break;
        }
    }

    // Если буфер почти пуст, замедляем отправку
    if (videoBuffer.size() <= 3) {
        videoSendTimer->setInterval(800);
    }
}

void ChatWindow::sendVideoFrameNow(const QByteArray &frameData)
{
    if (!udpSocket || remoteAddress.isNull() || frameData.isEmpty()) return;

    try {
        QByteArray packet;
        QDataStream stream(&packet, QIODevice::WriteOnly);
        stream << QString("VIDEO") << instanceId << localNickname << frameData;

        if (udpSocket->writeDatagram(packet, remoteAddress, remotePort) == -1) {
            if (!isConnectionBad) {
                isConnectionBad = true;
                logMessage("Ошибка отправки видео - активация буфера");
            }
            addToVideoBuffer(frameData);
        }
    } catch (...) {
        logMessage("Ошибка при отправке видео кадра");
    }
}

void ChatWindow::logConnectionQuality()
{
    QString quality;
    qint64 timeSinceLastPacket = QDateTime::currentMSecsSinceEpoch() - lastPacketTime;

    // Определяем качество соединения
    if (!isRemotePeerFound || timeSinceLastPacket > 3000) {
        quality = "Нет соединения";
        isConnectionBad = true;
        packetLossRate = 100.0;
    }
    else if (timeSinceLastPacket > 1000) {
        quality = "Качество связи: Нестабильное (пакеты не поступают)";
        isConnectionBad = true;
    }
    else if (packetLossRate < 2.0) {
        quality = "Качество связи: Отличное";
        isConnectionBad = false;
    }
    else if (packetLossRate < 5.0) {
        quality = "Качество связи: Хорошее";
        isConnectionBad = false;
    }
    else if (packetLossRate < 10.0) {
        quality = "Качество связи: Среднее";
        isConnectionBad = true;
    }
    else {
        quality = "Качество связи: Плохое";
        isConnectionBad = true;
    }

    // Получаем информацию о буфере
    int bufferSize;
    int bufferPercent;
    {
        QMutexLocker locker(&videoBufferMutex);
        bufferSize = videoBuffer.size();
        bufferPercent = (bufferSize * 100) / qMax(1, maxVideoBufferSize); // Защита от деления на 0
    }

    // Формируем строку статуса буфера
    QString bufferStatus = QString(" | Буфер: %1/%2 (%3%)")
                               .arg(bufferSize)
                               .arg(maxVideoBufferSize)
                               .arg(bufferPercent);

    // Формируем полное сообщение для лога
    QString fullMessage = quality +
                          QString(" (Потери: %1%, Пакетов: %2, Размер пакета: %3мс)")
                              .arg(packetLossRate, 0, 'f', 1)
                              .arg(totalPackets)
                              .arg(currentPacketMs) +
                          bufferStatus;

    logMessage(fullMessage);
}

void ChatWindow::checkAudioTiming()
{
    if (!audioTimer.isValid()) {
        audioTimer.start();
        return;
    }

    if (audioTimer.elapsed() > 2000) {
        QMutexLocker locker(&audioMutex);
        int currentSize = audioQueue.size();

        if (currentSize < TARGET_QUEUE_SIZE && currentPacketMs > MIN_PACKET_MS) {
            currentPacketMs = qMax(MIN_PACKET_MS, currentPacketMs - 5);
        }
        else if (currentSize > TARGET_QUEUE_SIZE * 1.5 && currentPacketMs < MAX_PACKET_MS) {
            currentPacketMs = qMin(MAX_PACKET_MS, currentPacketMs + 5);
        }

        audioTimer.restart();
        logConnectionQuality();
    }
}

void ChatWindow::updatePacketLossStats()
{
    if (totalPackets > 10) { // Не обновляем статистику для первых 10 пакетов
        packetLossRate = (double)lostPackets / totalPackets * 100.0;

        // Ограничиваем диапазон
        packetLossRate = qBound(0.0, packetLossRate, 100.0);

        logConnectionQuality();
    }
}

int ChatWindow::calculateAudioPacketSize() const
{
    return (audioFormat.sampleRate() * audioFormat.bytesPerFrame() * currentPacketMs) / 1000;
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

    // Таймер для проверки аудио буфера
    QTimer *audioCheckTimer = new QTimer(this);
    connect(audioCheckTimer, &QTimer::timeout, this, &ChatWindow::checkAudioTiming);
    audioCheckTimer->start(500);

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
    audioFormat.setSampleRate(48000);
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
    audioInput->setBufferSize(audioBufferSize * 3);
    audioInputDevice = audioInput->start();
    connect(audioInputDevice, &QIODevice::readyRead, this, &ChatWindow::sendAudioData);

    // Инициализация выхода
    audioOutput = new QAudioSink(outputDevice, audioFormat, this);
    audioOutput->setBufferSize(audioBufferSize * 6);
    audioOutputDevice = audioOutput->start();
}

void ChatWindow::initVideoDevices()
{
    try {
        const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
        if (cameras.isEmpty()) {
            logMessage("Камера не обнаружена");
            return;
        }

        camera = new QCamera(cameras.first(), this);
        captureSession = new QMediaCaptureSession(this);
        captureSession->setCamera(camera);

        videoSink = new QVideoSink(this);
        captureSession->setVideoOutput(videoSink);

        connect(videoSink, &QVideoSink::videoFrameChanged,
                this, &ChatWindow::videoFrameReady, Qt::DirectConnection);

        camera->start();
    } catch (...) {
        logMessage("Ошибка инициализации камеры");
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
    if (!isRemotePeerFound || !audioInputDevice || !udpSocket) return;

    try {
        const int packetSize = calculateAudioPacketSize();
        while (audioInputDevice->bytesAvailable() >= packetSize) {
            QByteArray audioData = audioInputDevice->read(packetSize);
            if (audioData.isEmpty() || audioData.size() != packetSize) {
                continue;
            }

            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream << QString("AUDIO") << instanceId << localNickname << ++lastSequence << audioData;

            if (udpSocket->writeDatagram(packet, remoteAddress, remotePort) == -1) {
                logMessage("Ошибка отправки аудио");
                break;
            }
        }
    } catch (...) {
        logMessage("Ошибка при отправке аудио данных");
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

void ChatWindow::resetConnectionStats()
{
    QMutexLocker locker(&audioMutex);  // Защищаем доступ к общим данным

    totalPackets = 0;
    lostPackets = 0;
    lastSequence = -1;
    packetLossRate = 0.0;
    lastPacketTime = 0;

    // Можно добавить лог для отладки
    logMessage("Статистика соединения сброшена");
}

void ChatWindow::processAudioPacket(QDataStream &stream)
{
    QString id, name;
    qint64 sequence;
    QByteArray audioData;
    stream >> id >> name >> sequence >> audioData;

    // Сбрасываем счетчик пропущенных пакетов
    missedPings = 0;

    // Обновляем время последнего полученного пакета
    lastPacketTime = QDateTime::currentMSecsSinceEpoch();

    // Если это первый пакет после разрыва, сбрасываем статистику
    if (!isRemotePeerFound) {
        resetConnectionStats();
        isRemotePeerFound = true;
    }

    totalPackets++;
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    lastPacketTime = currentTime;

    // Проверка пропущенных пакетов
    if (lastSequence != -1 && sequence > lastSequence + 1) {
        lostPackets += sequence - lastSequence - 1;
        updatePacketLossStats();
    }
    lastSequence = sequence;

    if (id == instanceId || !audioOutputDevice) return;

    QMutexLocker locker(&audioMutex);
    audioQueue.enqueue(audioData);

    // Поддерживаем оптимальный размер очереди
    while (audioQueue.size() > TARGET_QUEUE_SIZE * 2) {
        audioQueue.dequeue();
    }

    // Воспроизводим, если накопилось достаточно данных
    if (audioQueue.size() >= TARGET_QUEUE_SIZE) {
        audioOutputDevice->write(audioQueue.dequeue());
    }
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
    logConnectionQuality();
}

void ChatWindow::processDiscoverReply(QDataStream &stream, const QHostAddress &senderAddr)
{
    QString id, name;
    stream >> id >> name;

    if (id == instanceId) return;

    // Сбрасываем статистику при новом подключении
    resetConnectionStats();

    remoteAddress = senderAddr;
    remoteNickname = name;
    isRemotePeerFound = true;
    missedPings = 0;
    lastPacketTime = QDateTime::currentMSecsSinceEpoch();

    logMessage("Подключено к участнику: " + name + " (" + senderAddr.toString() + ")");
    logConnectionQuality();
}

void ChatWindow::processKeepAlive(QDataStream &stream, const QHostAddress &senderAddr)
{
    QString id, name;
    qint64 timestamp;
    stream >> id >> name >> timestamp;

    lastPacketTime = QDateTime::currentMSecsSinceEpoch();


    // Рассчитываем сетевую задержку
    qint64 delay = QDateTime::currentMSecsSinceEpoch() - timestamp;

    // Адаптируем размер пакета на основе задержки
    if (delay > 100 && currentPacketMs < MAX_PACKET_MS) {
        currentPacketMs = qMin(MAX_PACKET_MS, currentPacketMs + 5);
    }
    else if (delay < 50 && currentPacketMs > MIN_PACKET_MS) {
        currentPacketMs = qMax(MIN_PACKET_MS, currentPacketMs - 5);
    }

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
        stream << QString("KEEPALIVE") << instanceId << localNickname << QDateTime::currentMSecsSinceEpoch();
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
                             "Качество связи: %2\n"
                             "Размер пакета: %3 мс\n"
                             "Потери пакетов: %4%\n"
                             "Участник: %5\n"
                             "IP: %6\n"
                             "Формат аудио: %7 Hz, %8 каналов\n")
                         .arg(isRemotePeerFound ? "Подключено" : "Не подключено")
                         .arg(packetLossRate < 2 ? "Отличное" :
                                  packetLossRate < 5 ? "Хорошее" : "Плохое")
                         .arg(currentPacketMs)
                         .arg(packetLossRate, 0, 'f', 1)
                         .arg(remoteNickname)
                         .arg(remoteAddress.toString())
                         .arg(audioFormat.sampleRate())
                         .arg(audioFormat.channelCount());

    QMessageBox::information(this, "Статус системы", status);
}

void ChatWindow::resetConnection()
{
    if (!isRemotePeerFound) return;

    isRemotePeerFound = false;
    remoteAddress = QHostAddress();
    remoteNickname.clear();
    missedPings = 0;
    packetLossRate = 100.0;

    {
        QMutexLocker locker(&audioMutex);
        totalPackets = 0;
        lostPackets = 0;
        lastSequence = -1;
        audioQueue.clear();
    }

    {
        QMutexLocker locker(&videoBufferMutex);
        videoBuffer.clear();
    }

    videoSendTimer->stop();
    isConnectionBad = false;

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
    QString bufferStatus;

    {
        QMutexLocker locker(&videoBufferMutex);
        if (videoBuffer.size() > 0) {
            int bufferPercent = (videoBuffer.size() * 100) / maxVideoBufferSize;
            bufferStatus = QString(" | Буфер: %1/%2 (%3%)")
                               .arg(videoBuffer.size())
                               .arg(maxVideoBufferSize)
                               .arg(bufferPercent);
        } else {
            bufferStatus = " | Буфер пуст";
        }
    }

    QString fullMessage = QString("[%1] %2%3")
                              .arg(timestamp)
                              .arg(message)
                              .arg(bufferStatus);

    ui->debugArea->append(fullMessage);
    ui->debugArea->verticalScrollBar()->setValue(ui->debugArea->verticalScrollBar()->maximum());
}

void ChatWindow::videoFrameReady(const QVideoFrame &frame)
{
    if (!frame.isValid() || !ui || !ui->localVideoLabel) return;

    try {
        // Локальное отображение
        QImage image = frame.toImage();
        if (image.isNull()) return;

        QPixmap pixmap = QPixmap::fromImage(image.scaled(ui->localVideoLabel->size(),
                                                         Qt::KeepAspectRatio, Qt::SmoothTransformation));
        if (!pixmap.isNull()) {
            ui->localVideoLabel->setPixmap(pixmap);
        }

        // Подготовка кадра для отправки
        image = image.scaled(640, 480, Qt::KeepAspectRatio);
        QByteArray imageData;
        QBuffer buffer(&imageData);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", 80)) {
            logMessage("Ошибка кодирования видео");
            return;
        }

        // Безопасная проверка состояния соединения
        bool currentConnectionBad;
        {
            QMutexLocker locker(&audioMutex);
            currentConnectionBad = (packetLossRate > 30.0 && totalPackets > 50) ||
                                   !isRemotePeerFound ||
                                   (QDateTime::currentMSecsSinceEpoch() - lastPacketTime > 3000);
        }

        // Обработка изменения состояния
        if (currentConnectionBad != isConnectionBad) {
            isConnectionBad = currentConnectionBad;

            if (isConnectionBad) {
                logMessage(QString("Проблемы с соединением (потери: %1%), активация буфера")
                               .arg(packetLossRate, 0, 'f', 1));
                if (videoSendTimer) videoSendTimer->start(500);
            } else {
                logMessage("Соединение восстановлено");
                if (videoSendTimer) videoSendTimer->stop();

                // Асинхронная отправка буферизированных кадров
                QTimer::singleShot(0, this, [this]() {
                    QMutexLocker locker(&videoBufferMutex);
                    while (!videoBuffer.isEmpty()) {
                        sendVideoFrameNow(videoBuffer.dequeue());
                        QThread::msleep(30);
                    }
                });
            }
        }

        // Безопасная отправка/буферизация
        if (!isConnectionBad) {
            sendVideoFrameNow(imageData);
        } else {
            addToVideoBuffer(imageData);
        }
    } catch (...) {
        logMessage("Критическая ошибка в videoFrameReady");
    }
}
