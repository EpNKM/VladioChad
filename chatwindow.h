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
#include <QElapsedTimer>
#include <QMap>
#include <QLabel>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

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
    void processVideoBuffer();
    void resetConnectionState();
    void checkBufferState();
    void updateVideoStatus();
    void adjustVideoBuffer();

private:
    Ui::ChatWindow *ui;

    // Network
    QUdpSocket *udpSocket;
    QHostAddress remoteAddress;
    QString remoteNickname;
    QString instanceId;
    QString localNickname;
    bool isRemotePeerFound = false;
    int missedPings = 0;

private:
    struct VideoFrame {
        QByteArray data;
        qint64 timestamp;
        bool isKeyFrame;

        VideoFrame() : data(), timestamp(0), isKeyFrame(false) {}
        VideoFrame(const QByteArray& d, qint64 ts, bool key)
            : data(d), timestamp(ts), isKeyFrame(key) {}
    };

    QMap<qint64, VideoFrame> videoBuffer;  // Основной буфер
    QQueue<qint64> displayQueue;          // Очередь на отображение
    int videoBufferTargetSize = 20;      // Размер буфера по умолчанию
    int maxBufferSize = 50;             // Максимальный размер буфера

    void paintEvent(QPaintEvent *event) override;
    QPixmap lastGoodFrame;

    QChart *bitrateChart;
    QLineSeries *bitrateSeries;
    QChartView *chartView;
    QValueAxis *axisX;
    QValueAxis *axisY;
    QList<qreal> bitrateHistory;
    QElapsedTimer bitrateTimer;
    qint64 lastBytesSent = 0;
    qint64 lastBytesReceived = 0;
    qint64 totalBytesSent = 0;
    qint64 totalBytesReceived = 0;
    qint64 lastUpdateBytesSent = 0;
    qint64 lastUpdateBytesReceived = 0;

    void updateBitrateChart();
    void logNetworkStats();

    bool isFirstVideoPacket = true;
    QElapsedTimer bufferFillTimer;

    int m_maxMissedPings = 3; // заменили MAX_MISSED_PINGS

    // Audio
    QAudioFormat audioFormat;
    QAudioSource *audioInput = nullptr;
    QAudioSink *audioOutput = nullptr;
    QIODevice *audioInputDevice = nullptr;
    QIODevice *audioOutputDevice = nullptr;
    int audioBufferSize;
    QMutex audioMutex;

    void checkConnection();

    // Video
    QCamera *camera = nullptr;
    QMediaCaptureSession *captureSession = nullptr;
    QVideoSink *videoSink = nullptr;
    QQueue<QByteArray> videoFrameQueue;
    QMutex videoMutex;
    QTimer *videoPlaybackTimer;
    qint64 lastVideoSequence = 0;
    qint64 currentFrameId = 0;
    qint64 lastDisplayedFrameId = -1;
    int framesDropped = 0;
    int framesDisplayed = 0;
    bool isBuffering = false;
    QLabel *connectionStatusLabel;
    QElapsedTimer frameTimer;

    const int MIN_BUFFER_SIZE = 10;
    const int MAX_BUFFER_SIZE = 50;
    const int IDEAL_BUFFER_SIZE = 20;

    qint64 lastDisplayedSequence = -1;
    qint64 lastPacketTimestamp = 0;

    // Timers
    QTimer *connectionTimer;
    QTimer *keepAliveTimer;

    QTimer* disconnectDetectionTimer;
    QElapsedTimer lastPacketTime; // Заменяем lastPacketTimestamp если нужно

    // Методы
    void handleNetworkLoss();
    void recoveryQualityManagement();
    void emergencySave();
    void preserveKeyFrames();

    // Constants
    const int localPort = 45454;
    const int remotePort = 45454;
    int MAX_MISSED_PINGS = 3;
    int AUDIO_PACKET_MS = 40;

    void checkConnectionQuality();
    void adaptiveKeepAlive();

    qint64 lastKeepAliveTime = 0;
    enum NetworkQuality { GOOD_QUALITY, DEGRADED, POOR_QUALITY };
    NetworkQuality networkQuality = GOOD_QUALITY;

    void bufferVideoPacket(const QByteArray &data, qint64 sequence);
    void setVideoBufferSize(int size);
    void setupTimers();
    void setupAudioVideo();
    void setupStatusButton();
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
    void clearRemoteVideo();
    int calculateAudioPacketSize() const;
};

#endif // CHATWINDOW_H
