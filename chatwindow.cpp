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
#include <QFile>

ChatWindow::ChatWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::ChatWindow)
    , videoBufferTargetSize(20)
    , maxBufferSize(100)
    ,disconnectDetectionTimer(new QTimer(this))
    ,lastPacketTime()
{
    ui->setupUi(this);
    setWindowTitle("VladioChat");

    bitrateChart = new QChart();
    bitrateChart->setTitle("Битрейт (кбит/с)");
    bitrateChart->legend()->hide();
    bitrateChart->setBackgroundRoundness(0);

    bitrateSeries = new QLineSeries();
    bitrateChart->addSeries(bitrateSeries);

    axisX = new QValueAxis();
    axisX->setRange(0, 60);
    axisX->setLabelFormat("%d");
    axisX->setTitleText("Секунды");
    bitrateChart->addAxis(axisX, Qt::AlignBottom);
    bitrateSeries->attachAxis(axisX);

    axisY = new QValueAxis();
    axisY->setRange(0, 2000); // 0-2000 кбит/с
    axisY->setTitleText("кбит/с");
    bitrateChart->addAxis(axisY, Qt::AlignLeft);
    bitrateSeries->attachAxis(axisY);

    chartView = new QChartView(bitrateChart);
    chartView->setRenderHint(QPainter::Antialiasing);
    chartView->setMinimumSize(400, 200);

    ui->verticalLayout->insertWidget(0, chartView);

    bitrateTimer.start();
    QTimer *bitrateUpdateTimer = new QTimer(this);
    connect(bitrateUpdateTimer, &QTimer::timeout, this, &ChatWindow::updateBitrateChart);
    bitrateUpdateTimer->start(1000); // Обновление каждую секунду

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

    // Настройка кнопки статуса
    setupStatusButton();

    // Таймер воспроизведения видео
    videoPlaybackTimer = new QTimer(this);
    connect(videoPlaybackTimer, &QTimer::timeout, this, &ChatWindow::processVideoBuffer);

    videoPlaybackTimer->setTimerType(Qt::PreciseTimer);
    videoPlaybackTimer->start(40); // 25 FPS
    videoPlaybackTimer->setInterval(40);


    ui->remoteVideoLabel->setAttribute(Qt::WA_OpaquePaintEvent);
    ui->remoteVideoLabel->setAttribute(Qt::WA_NoSystemBackground);


    logMessage("Система готова. Ваш ник: " + localNickname);
    QTimer::singleShot(1000, this, &ChatWindow::sendDiscover);

    ui->bufferSizeSpinBox->setRange(1, 20); // Минимум 1, максимум 20 кадров
    ui->bufferSizeSpinBox->setValue(5);     // Значение по умолчанию
    connect(ui->bufferSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ChatWindow::setVideoBufferSize);

    connectionStatusLabel = new QLabel(this);
    connectionStatusLabel->setAlignment(Qt::AlignCenter);
    ui->verticalLayout->addWidget(connectionStatusLabel);

    QTimer *bufferAdjustTimer = new QTimer(this);
    connect(bufferAdjustTimer, &QTimer::timeout, this, &ChatWindow::adjustVideoBuffer);
    bufferAdjustTimer->start(5000); // Проверка каждые 5 секунд

    QTimer *statsTimer = new QTimer(this);
    connect(statsTimer, &QTimer::timeout, this, &ChatWindow::logNetworkStats);
    statsTimer->start(5000); // Логировать каждые 5 секунд
    frameTimer.start();

    videoBufferTargetSize = 15;  // Фиксированный размер
    maxBufferSize = 35;         // Максимальный предел
    isBuffering = true;

    QThread::currentThread()->setPriority(QThread::HighPriority);

    disconnectDetectionTimer->setInterval(2000);
    connect(disconnectDetectionTimer, &QTimer::timeout, this, [this](){
        if(isRemotePeerFound && lastPacketTime.elapsed() > 5000) {
            handleNetworkLoss();
        }
    });
    disconnectDetectionTimer->start();

    lastPacketTime.start(); // Запуск таймера

    connect(videoSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        QImage image = frame.toImage();
        if (!image.isNull()) {
            QPixmap pixmap = QPixmap::fromImage(image.scaled(
                ui->localVideoLabel->size(),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation
                ));
            ui->localVideoLabel->setPixmap(pixmap);
        }
    });

}

ChatWindow::~ChatWindow()
{
    cleanupAudio();
    if (camera) {
        camera->stop();
        delete camera;
    }
    delete chartView;
    delete bitrateChart;
    if(disconnectDetectionTimer) {
        disconnectDetectionTimer->stop();
        delete disconnectDetectionTimer;
    }
    delete ui;
}

void ChatWindow::handleNetworkLoss() {
    QMutexLocker locker(&videoMutex);

    // Проверяем, действительно ли нет пакетов
    if(lastPacketTime.elapsed() <= 7000) { // Добавляем гистерезис
        return;
    }

    isRemotePeerFound = false;
    logMessage("Обнаружена потеря сети");

    // Не очищаем буфер полностью, только уменьшаем размер
    if(videoBuffer.size() > 5) {
        QMap<qint64, VideoFrame> preserved;
        auto it = videoBuffer.end();
        for(int i = 0; i < 5 && it != videoBuffer.begin(); ++i) {
            --it;
            preserved.insert(it.key(), it.value());
        }
        videoBuffer = preserved;
    }
}

void ChatWindow::preserveKeyFrames()
{
    QMap<qint64, VideoFrame> preserved;
    for(auto it = videoBuffer.begin(); it != videoBuffer.end(); ++it) {
        if(it.value().isKeyFrame) {
            preserved.insert(it.key(), it.value());
        }
    }
    videoBuffer = preserved;
}

void ChatWindow::updateBitrateChart()
{
    qint64 elapsed = bitrateTimer.restart();
    if (elapsed == 0) return;

    // Получаем текущие значения
    qint64 currentSent = totalBytesSent;
    qint64 currentReceived = totalBytesReceived;

    // Рассчитываем битрейт (кбит/с)
    qreal sentKbps = (currentSent - lastUpdateBytesSent) * 8 / (qreal)elapsed / 1000.0;
    qreal receivedKbps = (currentReceived - lastUpdateBytesReceived) * 8 / (qreal)elapsed / 1000.0;
    qreal totalKbps = sentKbps + receivedKbps;

    lastUpdateBytesSent = currentSent;
    lastUpdateBytesReceived = currentReceived;

    // Сохраняем историю (60 секунд)
    bitrateHistory.append(totalKbps);
    if (bitrateHistory.size() > 60) {
        bitrateHistory.removeFirst();
    }

    // Обновляем график
    bitrateSeries->clear();
    for (int i = 0; i < bitrateHistory.size(); ++i) {
        bitrateSeries->append(i, bitrateHistory.at(i));
    }

    // Автомасштабирование оси Y
    qreal max = *std::max_element(bitrateHistory.begin(), bitrateHistory.end());
    axisY->setRange(0, qMax(100.0, max * 1.1));

    // Обновляем статус в заголовке
    bitrateChart->setTitle(QString("Битрейт (кбит/с) | TX: %1 RX: %2")
                               .arg(sentKbps, 0, 'f', 1)
                               .arg(receivedKbps, 0, 'f', 1));

    int bufferFillPercent = videoBufferTargetSize > 0 ?
                                (videoBuffer.size() * 100) / videoBufferTargetSize : 0;

    bitrateChart->setTitle(QString("Битрейт | TX: %1 кбит/с RX: %2 кбит/с | Буфер: %3%")
                               .arg(sentKbps, 0, 'f', 1)
                               .arg(receivedKbps, 0, 'f', 1)
                               .arg(bufferFillPercent));
}

void ChatWindow::recoveryQualityManagement()
{
    // Постепенное повышение качества
    QTimer::singleShot(30000, this, [this](){ // Через 30 сек
        if(isRemotePeerFound) {
            videoBufferTargetSize = 30; // Возвращаем нормальный размер
            maxBufferSize = 45;
        }
    });
}

void ChatWindow::setVideoBufferSize(int size) {
    QMutexLocker locker(&videoMutex);

    // Устанавливаем новые размеры
    videoBufferTargetSize = size;
    maxBufferSize = size + 25; // Максимальный размер на 25 больше целевого

    logMessage(QString("Ручная настройка: %1/%2 (цель/макс)")
                    .arg(videoBufferTargetSize).arg(maxBufferSize));

    // Очищаем избыточные кадры
    while (videoBuffer.size() > maxBufferSize) {
        if (!displayQueue.isEmpty()) {
            qint64 oldest = displayQueue.first();
            videoBuffer.remove(oldest);
            displayQueue.removeFirst();
        } else {
            videoBuffer.remove(videoBuffer.firstKey());
        }
    }

    // Обновляем статус буферизации
    if (videoBuffer.size() < videoBufferTargetSize) {
        isBuffering = true;
        bufferFillTimer.restart();
    }

    updateVideoStatus();
}

void ChatWindow::processVideoBuffer() {
    QMutexLocker locker(&videoMutex);

    static QElapsedTimer frameTimer;
    if (!frameTimer.isValid()) frameTimer.start();
    if (frameTimer.elapsed() < 33) return; // 30 FPS (33ms)
    frameTimer.restart();

    // Если буфер пуст - показываем последний успешный кадр
    if (videoBuffer.isEmpty()) {
        static QPixmap lastGoodFrame;
        if (!lastGoodFrame.isNull()) {
            ui->remoteVideoLabel->setPixmap(lastGoodFrame);
        } else {
            QPixmap blank(ui->remoteVideoLabel->size());
            blank.fill(Qt::black);
            ui->remoteVideoLabel->setPixmap(blank);
        }
        return;
    }

    // Берем самый свежий доступный кадр
    qint64 frameToShow = videoBuffer.lastKey();
    const VideoFrame& frame = videoBuffer[frameToShow];

    QImage image;
    if (image.loadFromData(frame.data, "JPEG")) {
        QPixmap pixmap = QPixmap::fromImage(image.scaled(
            ui->remoteVideoLabel->size(),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
            ));

        ui->remoteVideoLabel->setPixmap(pixmap);
        lastGoodFrame = pixmap; // Сохраняем последний успешный кадр
        framesDisplayed++;

        // Удаляем только если буфер переполнен
        if (videoBuffer.size() > maxBufferSize) {
            videoBuffer.remove(videoBuffer.firstKey());
        }
    } else {
        framesDropped++;
    }
}

void ChatWindow::resetConnectionState()
{
    QMutexLocker locker(&videoMutex);
    videoFrameQueue.clear();
    videoBuffer.clear();
    currentFrameId = 0;
    lastDisplayedFrameId = -1;
    framesDropped = 0;
    framesDisplayed = 0;
    isBuffering = true;
    bufferFillTimer.start(); // Перезапускаем таймер
    logMessage("Видео буфер сброшен");
}

void ChatWindow::updateVideoStatus()
{
    QString status;
    QColor color;

    if (!isRemotePeerFound) {
        status = "Нет соединения";
        color = Qt::red;
    }
    else if (isBuffering) {
        status = "Восстановление...";
        color = Qt::yellow;
    }
    else {
        status = "Соединение стабильно";
        color = Qt::green;
    }

    connectionStatusLabel->setText(status);
    connectionStatusLabel->setStyleSheet(QString("color: %1").arg(color.name()));
}

void ChatWindow::checkBufferState()
{
    if (videoBuffer.isEmpty()) {
        return;
    }

    if (isBuffering && videoBuffer.size() >= videoBufferTargetSize) {
        isBuffering = false;
        qint64 fillTime = bufferFillTimer.elapsed();
        logMessage(QString("Буфер заполнен до %1 кадров за %2 мс")
                       .arg(videoBuffer.size()).arg(fillTime));
        updateVideoStatus();
    }
    else if (!isBuffering && videoBuffer.size() < videoBufferTargetSize/2) {
        isBuffering = true;
        logMessage("Предупреждение: буфер опустошается!");
        updateVideoStatus();
    }
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

    audioFormat.setSampleRate(48000);
    audioFormat.setChannelCount(1);
    audioFormat.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice inputDevice = QMediaDevices::defaultAudioInput();
    QAudioDevice outputDevice = QMediaDevices::defaultAudioOutput();

    if (!inputDevice.isFormatSupported(audioFormat)) {
        audioFormat = inputDevice.preferredFormat();
        logMessage("Используется входной формат: " +
                   QString::number(audioFormat.sampleRate()) + "Hz, " +
                   QString::number(audioFormat.channelCount()) + " каналов");
    }

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

    audioInput = new QAudioSource(inputDevice, audioFormat, this);
    audioInput->setBufferSize(audioBufferSize * 2);
    audioInputDevice = audioInput->start();
    connect(audioInputDevice, &QIODevice::readyRead, this, &ChatWindow::sendAudioData);

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

        qint64 bytesSent = udpSocket->writeDatagram(packet, remoteAddress, remotePort);
        if (bytesSent != -1) {
            totalBytesSent += bytesSent; // Добавляем подсчет отправленных байт
        }
    }
}

void ChatWindow::readPendingDatagrams()
{
    while (udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = udpSocket->receiveDatagram();
        totalBytesReceived += datagram.data().size();
        lastPacketTime.restart(); // Обновляем время последнего пакета

        if (!datagram.isValid() || isLocalAddress(datagram.senderAddress())) {
            continue;
        }

        QByteArray data = datagram.data();
        QDataStream stream(data);
        QString messageType;
        stream >> messageType;

        // Диагностика видео-пакетов
        if (messageType == "VIDEO") {
            if (videoBuffer.isEmpty()) {
                logMessage("Получен видео-пакет. Размер: " +
                           QString::number(datagram.data().size()) + " байт");
            }
        }

        // Обработка остальных типов сообщений
        if (messageType == "AUDIO") {
            processAudioPacket(stream);
        }
        else if (messageType == "DISCOVER") {
            processDiscoverPacket(stream, datagram.senderAddress());
        }
        else if (messageType == "DISCOVER_REPLY") {
            processDiscoverReply(stream, datagram.senderAddress());
        }
        else if (messageType == "KEEPALIVE") {
            processKeepAlive(stream, datagram.senderAddress());
        }
        else if (messageType == "VIDEO") {
            processVideoPacket(stream);
        }
        else if (messageType == "MSG") {
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

void ChatWindow::logNetworkStats() {
    double dropRate = framesDisplayed > 0 ?
                          (double)framesDropped / (framesDisplayed + framesDropped) * 100 : 0;

    logMessage(QString("Статистика:\n"
                       "Кадры: %1/сек | Потери: %2%\n"
                       "Буфер: %3/%4 | Последний пакет: %5 мс назад\n"
                       "Состояние: %6")
                   .arg(framesDisplayed / 5.0, 0, 'f', 1)
                   .arg(dropRate, 0, 'f', 1)
                   .arg(videoBuffer.size())
                   .arg(videoBufferTargetSize)
                   .arg(lastPacketTime.elapsed())
                   .arg(isRemotePeerFound ? "Подключено" : "Отключено"));

    framesDisplayed = 0;
    framesDropped = 0;
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

    if (id == instanceId || isRemotePeerFound) return; // Добавляем проверку isRemotePeerFound

    if(isRemotePeerFound && remoteNickname == name) {
        lastPacketTime.restart();
        return;
    }

    resetConnectionState();
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

    if (!isRemotePeerFound) {
        resetConnectionState(); // Сбрасываем состояние при восстановлении
    }

    missedPings = 0;
    remoteAddress = senderAddr;
    remoteNickname = name;
    isRemotePeerFound = true;
}

void ChatWindow::processVideoPacket(QDataStream &stream) {
    QString id, name;
    QByteArray imageData;
    qint64 sequence;
    bool isKeyFrame;
    stream >> id >> name >> sequence >> isKeyFrame >> imageData;

    if (id == instanceId) return;

    lastPacketTime.restart();

    QMutexLocker locker(&videoMutex);

    // Быстрая проверка целостности
    if (imageData.size() < 100 || !imageData.startsWith("\xFF\xD8")) {
        return;
    }

    // Ключевые кадры обрабатываем особо
    if (isKeyFrame) {
        videoBuffer.clear(); // Полная очистка только для ключевых кадров
        isBuffering = true;
        bufferFillTimer.start();
    }

    // Добавляем только новые кадры
    if (!videoBuffer.contains(sequence)) {
        VideoFrame newFrame(imageData, QDateTime::currentMSecsSinceEpoch(), isKeyFrame);
        videoBuffer.insert(sequence, newFrame);
        displayQueue.append(sequence);

        // Автоматическая регулировка буфера
        while (videoBuffer.size() > maxBufferSize) {
            if (!displayQueue.isEmpty() && videoBuffer.contains(displayQueue.first())) {
                videoBuffer.remove(displayQueue.first());
                displayQueue.removeFirst();
            } else {
                videoBuffer.remove(videoBuffer.firstKey());
            }
        }
    }

    // Обновление статуса буферизации
    if (isBuffering && videoBuffer.size() >= videoBufferTargetSize) {
        isBuffering = false;
        logMessage("Буфер заполнен. Начало воспроизведения.");
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

void ChatWindow::emergencySave()
{
    if(!videoBuffer.isEmpty()) {
        QFile frameCache("last_frame.dat");
        if(frameCache.open(QIODevice::WriteOnly)) {
            frameCache.write(videoBuffer.last().data);
        }
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

void ChatWindow::showStatus() {
    QString status = QString("Статус системы:\n"
                             "Соединение: %1\n"
                             "Участник: %2\n"
                             "IP: %3\n"
                             "Формат аудио: %4 Hz, %5 каналов\n"
                             "Видео буфер: %6/%7 (макс %8)\n"
                             "Ручной размер: %9 кадров\n")
                         .arg(isRemotePeerFound ? "Подключено" : "Не подключено")
                         .arg(remoteNickname)
                         .arg(remoteAddress.toString())
                         .arg(audioFormat.sampleRate())
                         .arg(audioFormat.channelCount())
                         .arg(videoBuffer.size())
                         .arg(videoBufferTargetSize)
                         .arg(maxBufferSize)
                         .arg(ui->bufferSizeSpinBox->value());

    QMessageBox::information(this, "Статус системы", status);
}

void ChatWindow::resetConnection()
{
    resetConnectionState(); // Используем новый метод

    isRemotePeerFound = false;
    remoteAddress = QHostAddress();
    remoteNickname.clear();
    missedPings = 0;

    QMetaObject::invokeMethod(this, &ChatWindow::clearRemoteVideo, Qt::QueuedConnection);
    logMessage("Соединение сброшено");
    ui->chatArea->append("<i>Соединение потеряно</i>");
}

void ChatWindow::clearRemoteVideo()
{
    ui->remoteVideoLabel->clear();
    ui->remoteVideoLabel->setText("Ожидание подключения...");
    ui->remoteVideoLabel->setStyleSheet("QLabel { color: red; }");
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

void ChatWindow::videoFrameReady(const QVideoFrame &frame) {
    static QElapsedTimer sendTimer;
    if (!sendTimer.isValid()) sendTimer.start();
    if (sendTimer.elapsed() < 40) return; // 25 FPS
    sendTimer.restart();

    QImage image = frame.toImage();
    if (image.isNull()) return;

    // Оптимальные параметры сжатия
    int quality = isBuffering ? 60 : 75;
    int resolution = isBuffering ? 480 : 640;

    image = image.scaled(resolution, resolution * 9/16,
                         Qt::KeepAspectRatio, Qt::FastTransformation);

    QByteArray imageData;
    QBuffer buffer(&imageData);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", quality);

    // Отправка только если подключены
    if (isRemotePeerFound) {
        static qint64 seqNum = 0;
        QByteArray packet;
        QDataStream stream(&packet, QIODevice::WriteOnly);
        stream << QString("VIDEO") << instanceId << localNickname
               << seqNum++ << (seqNum % 30 == 0) << imageData;

        udpSocket->writeDatagram(packet, remoteAddress, remotePort);
    }
}

void ChatWindow::adjustVideoBuffer() {
    QMutexLocker locker(&videoMutex);

    // Жесткое ограничение размера буфера
    while (videoBuffer.size() > maxBufferSize) {
        if (!displayQueue.isEmpty()) {
            qint64 oldest = displayQueue.first();
            videoBuffer.remove(oldest);
            displayQueue.removeFirst();
        } else {
            videoBuffer.remove(videoBuffer.firstKey());
        }
    }
}

void ChatWindow::paintEvent(QPaintEvent *event)
{
    QMainWindow::paintEvent(event);

    if (isRemotePeerFound) {
        if (ui->remoteVideoLabel->pixmap().isNull()) {
            QPixmap pixmap(ui->remoteVideoLabel->size());
            pixmap.fill(Qt::black);

            QPainter painter(&pixmap);
            painter.setPen(Qt::white);
            painter.drawText(pixmap.rect(), Qt::AlignCenter,
                             isBuffering ? "Буферизация..." : "Ожидание данных");

            ui->remoteVideoLabel->setPixmap(pixmap);
        }
    }
}

void ChatWindow::bufferVideoPacket(const QByteArray &data, qint64 sequence)
{
    QMutexLocker locker(&videoMutex);

    // Проверка целостности JPEG
    if (data.size() < 100 || !data.startsWith("\xFF\xD8")) {
        logMessage("Неверный формат JPEG");
        return;
    }

    // Добавляем только новые кадры
    if (!videoBuffer.contains(sequence)) {
        videoBuffer.insert(sequence, VideoFrame(data, QDateTime::currentMSecsSinceEpoch(), false));
        displayQueue.append(sequence); // Используем append() вместо enqueue()

        // Ограничение размера буфера
        while (videoBuffer.size() > maxBufferSize && !displayQueue.isEmpty()) {
            qint64 oldest = displayQueue.takeFirst(); // Используем takeFirst() вместо dequeue()
            videoBuffer.remove(oldest);
        }
    }
}

