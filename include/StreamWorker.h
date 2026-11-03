#ifndef OMNIGUARD_STREAM_WORKER_H
#define OMNIGUARD_STREAM_WORKER_H

#include "Common.h"
#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>
#include <memory>
#include <functional>

// Forward declare FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

class StreamWorker : public QObject {
    Q_OBJECT
public:
    explicit StreamWorker(const CameraConfig& config, QObject* parent = nullptr);
    ~StreamWorker() override;
    
    // Non-copyable, movable
    StreamWorker(const StreamWorker&) = delete;
    StreamWorker& operator=(const StreamWorker&) = delete;
    StreamWorker(StreamWorker&&) = delete;
    StreamWorker& operator=(StreamWorker&&) = delete;
    
    // Start the worker thread
    void start();
    
    // Stop the worker thread gracefully
    void stop();
    
    // Check if worker is running
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }
    
    // Get camera ID
    int cameraId() const { return m_config.id; }
    
    // Get current state (thread-safe)
    CameraState getState() const { return m_state.load(std::memory_order_acquire); }
    
    // Get status snapshot (thread-safe copy)
    CameraStatus getStatus() const;
    
    // Force reconnection attempt
    void requestReconnect();

signals:
    // Emitted when a new frame is decoded and ready for display
    // Uses Qt::QueuedConnection for thread safety
    void frameReady(FrameData frame);
    
    // Emitted when camera state changes
    void stateChanged(CameraState newState);
    
    // Emitted on error (non-fatal)
    void errorOccurred(const QString& message);
    
    // Emitted when connection is established
    void connected();
    
    // Emitted when connection is lost
    void disconnected(const QString& reason);

private:
    // Main worker loop
    void run();
    
    // Connection and streaming
    bool openStream();
    void closeStream();
    bool reconnect();
    
    // Frame processing
    bool decodeFrame();
    bool processFrame(AVFrame* frame);
    QImage convertFrame(AVFrame* frame);
    
    // FFmpeg initialization
    bool initFFmpeg();
    void cleanupFFmpeg();
    
    // Timeout handling
    bool waitForData(int timeoutMs);
    
    // Configuration
    CameraConfig m_config;
    
    // Thread control
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::unique_ptr<QThread> m_thread;
    std::unique_ptr<QMutex> m_mutex;
    std::unique_ptr<QWaitCondition> m_waitCondition;
    
    // FFmpeg contexts (owned by this worker)
    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVCodec* m_codec = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_frameRGB = nullptr;
    AVPacket* m_packet = nullptr;
    SwsContext* m_swsCtx = nullptr;
    uint8_t* m_rgbBuffer = nullptr;
    int m_rgbBufferSize = 0;
    int m_videoStreamIndex = -1;
    
    // Frame counter
    std::atomic<uint32_t> m_frameCounter{0};
    
    // Status tracking (atomic for thread-safe access)
    std::atomic<CameraState> m_state{CameraState::Disconnected};
    std::atomic<int> m_consecutiveFailures{0};
    std::atomic<uint64_t> m_totalFrames{0};
    std::atomic<uint64_t> m_droppedFrames{0};
    std::atomic<qint64> m_lastFrameTime{0};
    QString m_lastError;
    
    // Reconnection state
    std::atomic<bool> m_reconnectRequested{false};
    
    // Helper: set state and emit signal
    void setState(CameraState newState);
    
    // Helper: build RTSP URL with credentials
    QString buildRtspUrl() const;
    
    // Helper: configure FFmpeg options
    void configureFormatContext();
    void configureCodecContext();
};

#endif // OMNIGUARD_STREAM_WORKER_H