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
    , videoTotalPackets(0)
    , videoLostPackets(0)
    , videoPacketLossRate(0.0)
    , lastVideoSequence(-1)
{
    ui->setupUi(this);
    setWindowTitle("VladioChat");

    connect(ui->BufferCheckBox, &QCheckBox::stateChanged, this, &ChatWindow::on_BufferCheckBox_stateChanged);

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

    logMessage("Система готова. Ваш ник: " + localNickname);
    logConnectionQuality();
    QTimer::singleShot(1000, this, &ChatWindow::sendDiscover);

    connect(ui->applyBufferButton, &QPushButton::clicked,
            this, &ChatWindow::on_applyBufferButton_clicked);

    setupBitrateChart();

    showMaximized();

    ui->tabWidget->setCurrentWidget(ui->remoteVideoTab);
}


ChatWindow::~ChatWindow()
{
    cleanupAudio();
    if (camera) {
        camera->stop();
        delete camera;
    }
    videoTimer.stop();
    delete ui;
}

void ChatWindow::setupBitrateChart()
{
    bitrateChart = new QChart();
    bitrateChart->setTitle("Битрейт (Мбит/с)");
    bitrateChart->legend()->setVisible(true);
    bitrateChart->legend()->setAlignment(Qt::AlignBottom);
    bitrateChart->setBackgroundRoundness(0);
    bitrateChart->setMargins(QMargins(0, 0, 0, 0));

    // Серии для RX и TX
    bitrateSeriesRx = new QLineSeries();
    bitrateSeriesRx->setName("RX (входящий)");
    QPen bluePen(Qt::blue);
    bluePen.setWidth(2);
    bitrateSeriesRx->setPen(bluePen);

    bitrateSeriesTx = new QLineSeries();
    bitrateSeriesTx->setName("TX (исходящий)");
    QPen redPen(Qt::red);
    redPen.setWidth(2);
    bitrateSeriesTx->setPen(redPen);

    bitrateChart->addSeries(bitrateSeriesRx);
    bitrateChart->addSeries(bitrateSeriesTx);

    axisX = new QValueAxis();
    axisX->setRange(0, 60);
    axisX->setLabelFormat("%d");
    axisX->setTitleText("Секунды");
    axisX->setTickCount(7);
    bitrateChart->addAxis(axisX, Qt::AlignBottom);
    bitrateSeriesRx->attachAxis(axisX);
    bitrateSeriesTx->attachAxis(axisX);

    axisY = new QValueAxis();
    axisY->setRange(0, 10); // 0-10 Мбит/с по умолчанию
    axisY->setTitleText("Мбит/с");
    axisY->setTickCount(6);
    bitrateChart->addAxis(axisY, Qt::AlignLeft);
    bitrateSeriesRx->attachAxis(axisY);
    bitrateSeriesTx->attachAxis(axisY);

    ui->bitrateChartView->setChart(bitrateChart);
    ui->bitrateChartView->setRenderHint(QPainter::Antialiasing);

    bitrateTimer.start();
    QTimer *bitrateUpdateTimer = new QTimer(this);
    connect(bitrateUpdateTimer, &QTimer::timeout, this, &ChatWindow::updateBitrateChart);
    bitrateUpdateTimer->start(1000);
}

void ChatWindow::updateBitrateChart()
{
    qint64 elapsed = bitrateTimer.restart();
    if (elapsed == 0) return;

    // Получаем текущие значения
    qint64 currentSent = totalBytesSent;
    qint64 currentReceived = totalBytesReceived;

    // Рассчитываем битрейт (Мбит/с)
    qreal bitsSent = (currentSent - lastUpdateBytesSent) * 8;  // Байты → биты
    qreal secondsElapsed = elapsed / 1000.0;                  // мс → секунды
    qreal sendMbps = bitsSent / secondsElapsed / 1'000'000.0;

    qreal bitsReceived = (currentReceived - lastUpdateBytesReceived) * 8;
    qreal receivedMbps = bitsReceived / secondsElapsed / 1'000'000.0;

    lastUpdateBytesSent = currentSent;
    lastUpdateBytesReceived = currentReceived;

    // Сохраняем историю (60 секунд)
    bitrateHistoryRx.append(receivedMbps);
    bitrateHistoryTx.append(sendMbps);

    if (bitrateHistoryRx.size() > 60) {
        bitrateHistoryRx.removeFirst();
        bitrateHistoryTx.removeFirst();
    }

    // Обновляем график
    bitrateSeriesRx->clear();
    bitrateSeriesTx->clear();

    for (int i = 0; i < bitrateHistoryRx.size(); ++i) {
        bitrateSeriesRx->append(i, bitrateHistoryRx.at(i));
        bitrateSeriesTx->append(i, bitrateHistoryTx.at(i));
    }

    // Автомасштабирование оси Y с разумными пределами
    qreal maxRx = *std::max_element(bitrateHistoryRx.begin(), bitrateHistoryRx.end());
    qreal maxTx = *std::max_element(bitrateHistoryTx.begin(), bitrateHistoryTx.end());
    qreal max = qMax(maxRx, maxTx);

    // Устанавливаем верхний предел с запасом 20%, но не менее 2 Мбит/с
    qreal upperLimit = qMax(2.0, max * 1.2);
    // И не более 100 Мбит/с, чтобы не терять детализацию при обычных значениях
    upperLimit = qMin(100.0, upperLimit);

    axisY->setRange(0, upperLimit);

    // Обновляем статус в заголовке
    bitrateChart->setTitle(QString("Битрейт | TX: %1 Мбит/с RX: %2 Мбит/с")
                               .arg(sendMbps, 0, 'f', 2)
                               .arg(receivedMbps, 0, 'f', 2));
}

void ChatWindow::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == videoTimer.timerId()) {
        processBufferedVideo();

        // Останавливаем таймер если буфер опустел ниже порога
        QMutexLocker locker(&videoMutex);
        if (videoBuffer.size() < maxBufferSize/2) {
            videoTimer.stop();
        }
    } else {
        QMainWindow::timerEvent(event);
    }
}

void ChatWindow::on_applyBufferButton_clicked()
{
    int newSize = ui->bufferSizeSpinBox->value();
    if (newSize != maxBufferSize) {
        QMutexLocker locker(&videoMutex);
        maxBufferSize = newSize;
        ui->bufferStatusLabel->setText(QString("Текущий буфер: %1 кадров").arg(maxBufferSize));

        // Очищаем буфер при изменении размера
        videoBuffer.clear();
    }
}

void ChatWindow::logConnectionQuality()
{
    QString quality;
    if (!isRemotePeerFound) {
        quality = "Нет соединения";
    }
    else if (packetLossRate < 2.0 && videoPacketLossRate < 2.0) {
        quality = "Качество связи: Отличное";
    }
    else if (packetLossRate < 5.0 && videoPacketLossRate < 5.0) {
        quality = "Качество связи: Хорошее";
    }
    else if (packetLossRate < 10.0 && videoPacketLossRate < 10.0) {
        quality = "Качество связи: Среднее";
    }
    else {
        quality = "Качество связи: Плохое";
    }

    logMessage(quality + QString("\nАудио - Потери: %1%, Размер пакета: %2мс\nВидео - Потери: %3%")
                             .arg(packetLossRate, 0, 'f', 1)
                             .arg(currentPacketMs)
                             .arg(videoPacketLossRate, 0, 'f', 1));
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

void ChatWindow::on_BufferCheckBox_stateChanged(int state)
{
    BufferingEnabled = (state == Qt::Checked);


    if (!BufferingEnabled) {
        // Очищаем оба буфера при отключении
        QMutexLocker videoLocker(&videoMutex);
        videoBuffer.clear();

        QMutexLocker audioLocker(&audioMutex);
        audioQueue.clear();
    }

    logMessage(QString("Буферизация %1")
                   .arg(BufferingEnabled ? "включена" : "отключена"));
}

void ChatWindow::updatePacketLossStats()
{
    if (totalPackets > 0) {
        packetLossRate = (double)lostPackets / (totalPackets + lostPackets) * 100.0;
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
        stream << QString("AUDIO") << instanceId << localNickname << ++lastSequence << audioData;

        qint64 bytesSent = udpSocket->writeDatagram(packet, remoteAddress, remotePort);
        if (bytesSent != -1) {
            totalBytesSent += bytesSent;
        }
    }
}

void ChatWindow::readPendingDatagrams()
{
    while (udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = udpSocket->receiveDatagram();
        totalBytesReceived += datagram.data().size();
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
    qint64 sequence;
    QByteArray audioData;
    stream >> id >> name >> sequence >> audioData;

    if (id == instanceId) return;

    static int initialPackets = 0;
    if (initialPackets < 5) {
        initialPackets++;
        lastSequence = sequence;
        return;
    }

    totalPackets++;

    if (lastSequence == -1) {
        lastSequence = sequence;
        return;
    }

    // Подсчет потерь только если sequence > lastSequence
    if (sequence > lastSequence) {
        lostPackets += qMax(0LL, sequence - lastSequence - 1);
        lastSequence = sequence;
    }

    // Обновляем статистику потерь
    packetLossRate = (totalPackets > 0) ?
                         (double)lostPackets / (totalPackets + lostPackets) * 100.0 : 0.0;

    if (!audioOutputDevice) return;

    if (BufferingEnabled) {
        QMutexLocker locker(&audioMutex);
        audioQueue.enqueue(audioData);
        while (audioQueue.size() > TARGET_QUEUE_SIZE * 2) {
            audioQueue.dequeue();
        }
        if (audioQueue.size() >= TARGET_QUEUE_SIZE) {
            audioOutputDevice->write(audioQueue.dequeue());
        }
    } else {
        audioOutputDevice->write(audioData);
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

    remoteAddress = senderAddr;
    remoteNickname = name;
    isRemotePeerFound = true;
    missedPings = 0;
    logMessage("Подключено к участнику: " + name + " (" + senderAddr.toString() + ")");
    logConnectionQuality();
}

void ChatWindow::processKeepAlive(QDataStream &stream, const QHostAddress &senderAddr)
{
    QString id, name;
    qint64 timestamp;
    stream >> id >> name >> timestamp;

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

void ChatWindow::processBufferedVideo()
{
    if (videoBuffer.isEmpty()) return;

    // Берем самый старый кадр из буфера
    QImage image = videoBuffer.dequeue();

    // Отображаем кадр
    QPixmap pixmap = QPixmap::fromImage(image.scaled(ui->remoteVideoLabel->size(),
                                                     Qt::KeepAspectRatio, Qt::SmoothTransformation));
    ui->remoteVideoLabel->setPixmap(pixmap);

    // Если в буфере остались кадры, планируем следующий
    if (!videoBuffer.isEmpty()) {
        QTimer::singleShot(100, this, &ChatWindow::processBufferedVideo); // ~30 FPS
    }
}

void ChatWindow::processVideoPacket(QDataStream &stream)
{
    QString id, name;
    qint64 sequence;
    QByteArray imageData;
    stream >> id >> name >> sequence >> imageData;

    if (id == instanceId) return;

    videoTotalPackets++;

    if (lastVideoSequence == -1) {
        lastVideoSequence = sequence;
        return;
    }

    // Подсчет пропущенных пакетов
    if (sequence > lastVideoSequence + 1) {
        videoLostPackets += sequence - (lastVideoSequence + 1);
    }
    lastVideoSequence = sequence;

    // Обновляем статистику потерь
    videoPacketLossRate = (videoTotalPackets > 0) ?
                              (double)videoLostPackets / (videoTotalPackets + videoLostPackets) * 100.0 : 0.0;

    QImage image;
    if (image.loadFromData(imageData, "JPEG")) {
        if (BufferingEnabled) {
            QMutexLocker locker(&videoMutex);
            videoBuffer.enqueue(image);
            while (videoBuffer.size() > maxBufferSize) {
                videoBuffer.dequeue();
            }
            if (videoBuffer.size() >= maxBufferSize) {
                processBufferedVideo();
            }
        } else {
            QPixmap pixmap = QPixmap::fromImage(image.scaled(ui->remoteVideoLabel->size(),
                                                             Qt::KeepAspectRatio, Qt::SmoothTransformation));
            ui->remoteVideoLabel->setPixmap(pixmap);
        }
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

    qint64 bytesSent = udpSocket->writeDatagram(packet, remoteAddress, remotePort);
    if (bytesSent == -1) {
        logMessage("Ошибка отправки сообщения");
    } else {
        totalBytesSent += bytesSent;
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
    isRemotePeerFound = false;
    remoteAddress = QHostAddress();
    remoteNickname.clear();
    missedPings = 0;
    packetLossRate = 0.0;
    totalPackets = 0;
    lostPackets = 0;
    lastSequence = -1;  // Сбрасываем в начальное состояние

    {
        QMutexLocker locker(&audioMutex);
        audioQueue.clear();
    }

    logMessage("Соединение сброшено");
    logConnectionQuality();
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

    if (!isRemotePeerFound || remoteAddress.isNull()) return;

    static qint64 videoSequence = 0;
    image = image.scaled(640, 480, Qt::KeepAspectRatio);
    QByteArray imageData;
    QBuffer buffer(&imageData);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 80);

    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream << QString("VIDEO") << instanceId << localNickname << ++videoSequence << imageData;

    qint64 bytesSent = udpSocket->writeDatagram(packet, remoteAddress, remotePort);
    if (bytesSent != -1) {
        totalBytesSent += bytesSent;
    }
}
