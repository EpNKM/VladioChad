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
    int calculateAudioPacketSize() const;
};

#endif // CHATWINDOW_H
