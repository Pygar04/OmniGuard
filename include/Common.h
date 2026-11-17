#ifndef OMNIGUARD_COMMON_H
#define OMNIGUARD_COMMON_H

#include <QtGlobal>
#include <QMetaType>
#include <QImage>
#include <QString>
#include <atomic>
#include <memory>

// Include FFmpeg headers first for error handling
extern "C" {
#include <libavutil/error.h>
#include <libavutil/avutil.h>
}

// Forward declarations
class QImage;

// Camera connection states
enum class CameraState : uint8_t {
    Disconnected = 0,
    Connecting     = 1,
    Connected      = 2,
    Reconnecting   = 3,
    Error          = 4
};

// Camera configuration structure
struct CameraConfig {
    int id = -1;                    // Camera index (0, 1, 2)
    QString name;                   // Display name
    QString rtspUrl;                // RTSP stream URL (main stream)
    QString subStreamUrl;           // RTSP sub-stream URL (low-res for monitoring)
    QString username;               // Optional username
    QString password;               // Optional password
    int reconnectIntervalMs = 3000; // Reconnection attempt interval
    int connectionTimeoutMs = 10000; // Connection timeout
    int receiveTimeoutMs = 5000;    // Receive timeout
    bool enableTcp = true;          // Use TCP for RTSP (more reliable)
    bool enableBuffering = true;    // Enable network buffering
    bool useSubStream = false;      // Use sub-stream instead of main stream
    bool enableHwAccel = false;     // Enable hardware acceleration (Intel QSV/NVDEC)
    QString hwAccelType;            // "qsv" (Intel), "cuda" (NVIDIA), "vaapi" (AMD/Intel)
    bool lowCpuDecode = false;      // Salta il deblocking del decoder (meno CPU, qualità un po' inferiore)
    
    // Video settings
    int width = 1920;               // Non usato dal worker (mantenuto per compatibilità JSON)
    int height = 1080;              // Non usato dal worker (mantenuto per compatibilità JSON)
    int fps = 25;                   // FPS massimi mostrati a schermo (il decoder legge comunque tutto)
    
    CameraConfig() = default;
    CameraConfig(int id_, const QString& name_, const QString& url_)
        : id(id_), name(name_), rtspUrl(url_) {}
    
    // Returns the active stream URL based on useSubStream setting
    QString getActiveUrl() const {
        if (useSubStream && !subStreamUrl.isEmpty()) {
            return subStreamUrl;
        }
        return rtspUrl;
    }
    
    // URL effettivo con credenziali inserite (se non già presenti nell'URL).
    // Usato sia dal worker che dal "Testa connessione": devono comportarsi uguale.
    QString buildStreamUrl() const {
        QString url = getActiveUrl();
        if (username.isEmpty() || password.isEmpty()) return url;
        const int schemeEnd = url.indexOf("://");
        if (schemeEnd < 0) return url;
        const int hostStart = schemeEnd + 3;
        const int pathStart = url.indexOf('/', hostStart);
        const QString authority = url.mid(hostStart, pathStart < 0 ? -1 : pathStart - hostStart);
        if (authority.contains('@')) return url;  // credenziali già nell'URL
        url.insert(hostStart, username + ":" + password + "@");
        return url;
    }
    
    bool operator==(const CameraConfig&) const = default;
};

// Frame data structure for thread-safe signal/slot communication
// Must be copyable for Qt's meta-type system (QueuedConnection)
struct FrameData {
    int cameraId = -1;
    QImage image;                   // Implicitly shared (copy-on-write)
    qint64 timestamp = 0;           // Frame timestamp (ms since epoch)
    uint32_t frameNumber = 0;       // Monotonic frame counter
    
    FrameData() = default;
    FrameData(int id, const QImage& img, qint64 ts, uint32_t fn)
        : cameraId(id), image(img), timestamp(ts), frameNumber(fn) {}
    FrameData(int id, QImage&& img, qint64 ts, uint32_t fn)
        : cameraId(id), image(std::move(img)), timestamp(ts), frameNumber(fn) {}
    
    // Copyable for Qt meta-type system
    FrameData(const FrameData&) = default;
    FrameData& operator=(const FrameData&) = default;
    FrameData(FrameData&&) = default;
    FrameData& operator=(FrameData&&) = default;
    
    bool isValid() const { return !image.isNull() && cameraId >= 0; }
};

// Camera status snapshot (copyable for signal/slot)
struct CameraStatus {
    CameraState state = CameraState::Disconnected;
    int consecutiveFailures = 0;
    uint64_t totalFrames = 0;       // frame decodificati
    uint64_t displayedFrames = 0;   // frame consegnati alla GUI
    uint64_t droppedFrames = 0;
    qint64 lastFrameTime = 0;
    QString lastError;
    
    CameraStatus() = default;
    CameraStatus(const CameraStatus&) = default;
    CameraStatus& operator=(const CameraStatus&) = default;
};

// FFmpeg error codes wrapper
struct FFmpegError {
    int code = 0;
    QString message;
    
    FFmpegError() = default;
    FFmpegError(int c, const char* msg) : code(c), message(msg) {}
    FFmpegError(int c, const QString& msg) : code(c), message(msg) {}
    
    bool isOk() const { return code >= 0; }
    explicit operator bool() const { return isOk(); }
    
    static FFmpegError fromAvError(int avErr) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(avErr, errbuf, sizeof(errbuf));
        return FFmpegError(avErr, QString::fromUtf8(errbuf));
    }
};

Q_DECLARE_METATYPE(FrameData)
Q_DECLARE_METATYPE(CameraState)
Q_DECLARE_METATYPE(CameraConfig)
Q_DECLARE_METATYPE(CameraStatus)

#endif // OMNIGUARD_COMMON_H