#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include <QMainWindow>
#include <QUdpSocket>
#include <QAudioFormat>
#include <QAudioSource>
#include <QAudioSink>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QPushButton>
#include <QMutex>
#include <QQueue>
#include <QTimer>
#include <QNetworkDatagram>
#include <QScrollBar>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QBasicTimer>

QT_BEGIN_NAMESPACE
namespace Ui { class ChatWindow; }
QT_END_NAMESPACE

class ChatWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ChatWindow(QWidget *parent = nullptr);
    ~ChatWindow();

private slots:
    void readPendingDatagrams();
    void sendAudioData();
    void sendDiscover();
    void sendKeepAlive();
    void videoFrameReady(const QVideoFrame &frame);
    void on_sendButton_clicked();
    void showStatus();
    void on_applyBufferButton_clicked();
    void on_videoBufferCheckBox_stateChanged(int state);
    void on_audioBufferCheckBox_stateChanged(int state);

private:
    Ui::ChatWindow *ui;

    // Video buffering
    QQueue<QImage> videoBuffer;
    int maxBufferSize = 5; // Количество кадров в буфере
    QMutex videoMutex;

    bool videoBufferingEnabled = true;
    bool audioBufferingEnabled = true;

    void processBufferedVideo();

    // График битрейта
    QLineSeries *bitrateSeriesRx;
    QLineSeries *bitrateSeriesTx;
    QList<qreal> bitrateHistoryRx;
    QList<qreal> bitrateHistoryTx;
    QChart *bitrateChart;
    QValueAxis *axisX;
    QValueAxis *axisY;
    QElapsedTimer bitrateTimer;
    qint64 totalBytesSent = 0;
    qint64 totalBytesReceived = 0;
    qint64 lastUpdateBytesSent = 0;
    qint64 lastUpdateBytesReceived = 0;
    QBasicTimer videoTimer;

    // Методы
    void updateBitrateChart();
    void setupBitrateChart();
    void timerEvent(QTimerEvent *event) override;

    // Network
    QUdpSocket *udpSocket;
    QHostAddress remoteAddress;
    QString remoteNickname;
    QString instanceId;
    QString localNickname;
    bool isRemotePeerFound = false;
    int missedPings = 0;

    void checkAudioTiming();
    void updatePacketLossStats();

    // Audio
    QAudioFormat audioFormat;
    QAudioSource *audioInput = nullptr;
    QAudioSink *audioOutput = nullptr;
    QIODevice *audioInputDevice = nullptr;
    QIODevice *audioOutputDevice = nullptr;
    QByteArray audioBuffer;
    int audioBufferSize;
    QMutex audioMutex;

    // Video
    QCamera *camera = nullptr;
    QMediaCaptureSession *captureSession = nullptr;
    QVideoSink *videoSink = nullptr;

    // Timers
    QTimer *connectionTimer;
    QTimer *keepAliveTimer;

    // Constants
    const int localPort = 45454;
    const int remotePort = 45454;
    const int MAX_MISSED_PINGS = 3;
    const int AUDIO_PACKET_MS = 40;
    QQueue<QByteArray> audioQueue;
    QElapsedTimer audioTimer;
    int currentPacketMs;
    const int MIN_PACKET_MS = 20;
    const int MAX_PACKET_MS = 60;
    const int TARGET_QUEUE_SIZE = 3;

    double packetLossRate;
    int totalPackets;
    int lostPackets;
    qint64 lastSequence;

    void logConnectionQuality();

    void setupTimers();
    void setupAudioVideo();
    void initAudioDevices();
    void initVideoDevices();
    void cleanupAudio();

    void processAudioPacket(QDataStream &stream);
    void processDiscoverPacket(QDataStream &stream, const QHostAddress &senderAddr);
    void processDiscoverReply(QDataStream &stream, const QHostAddress &senderAddr);
    void processKeepAlive(QDataStream &stream, const QHostAddress &senderAddr);
    void processVideoPacket(QDataStream &stream);
    void processTextMessage(QDataStream &stream);

    void resetConnection();
    bool isLocalAddress(const QHostAddress &address);
    void logMessage(const QString &message);
    int calculateAudioPacketSize() const;
};

#endif // CHATWINDOW_H
