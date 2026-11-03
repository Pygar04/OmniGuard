#include "StreamWorker.h"
#include <QThread>
#include <QDateTime>
#include <QDebug>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

StreamWorker::StreamWorker(const CameraConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_thread(std::make_unique<QThread>())
    , m_mutex(std::make_unique<QMutex>())
    , m_waitCondition(std::make_unique<QWaitCondition>()) {
    
    moveToThread(m_thread.get());
    
    connect(m_thread.get(), &QThread::started, this, &StreamWorker::run);
    connect(m_thread.get(), &QThread::finished, this, &QObject::deleteLater);
    
    qRegisterMetaType<FrameData>("FrameData");
    qRegisterMetaType<CameraState>("CameraState");
}

StreamWorker::~StreamWorker() {
    stop();
    if (m_thread->isRunning()) {
        m_thread->wait(5000);
    }
    cleanupFFmpeg();
}

void StreamWorker::start() {
    if (m_running.load(std::memory_order_acquire)) {
        qWarning() << "Worker already running for camera" << m_config.id;
        return;
    }
    
    m_stopRequested.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread->start(QThread::NormalPriority);
}

void StreamWorker::stop() {
    m_stopRequested.store(true, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    m_reconnectRequested.store(true, std::memory_order_release);
    m_waitCondition->wakeAll();
}

void StreamWorker::requestReconnect() {
    m_reconnectRequested.store(true, std::memory_order_release);
    m_waitCondition->wakeAll();
}

CameraStatus StreamWorker::getStatus() const {
    CameraStatus status;
    status.state = m_state.load(std::memory_order_acquire);
    status.consecutiveFailures = m_consecutiveFailures.load(std::memory_order_relaxed);
    status.totalFrames = m_totalFrames.load(std::memory_order_relaxed);
    status.droppedFrames = m_droppedFrames.load(std::memory_order_relaxed);
    status.lastFrameTime = m_lastFrameTime.load(std::memory_order_relaxed);
    status.lastError = m_lastError;
    return status;
}

void StreamWorker::run() {
    qInfo() << "Stream worker started for camera" << m_config.id << "(" << m_config.name << ")";
    
    if (!initFFmpeg()) {
        setState(CameraState::Error);
        emit errorOccurred("Failed to initialize FFmpeg");
        m_running.store(false, std::memory_order_release);
        return;
    }
    
    // === LONG-RUNNING PROTECTIONS ===
    // Watchdog: timestamp ultimo frame ricevuto con successo
    qint64 lastSuccessfulFrameTime = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 WATCHDOG_TIMEOUT_MS = 15000;  // 15 secondi senza frame = force reconnect
    
    // Forced periodic reconnect: ogni 6 ore ricrea connessione per pulire stato FFmpeg
    constexpr qint64 FORCED_RECONNECT_INTERVAL_MS = 6 * 60 * 60 * 1000;  // 6 ore
    qint64 connectionStartTime = QDateTime::currentMSecsSinceEpoch();
    
    // Statistiche periodiche (ogni ora)
    constexpr qint64 STATS_LOG_INTERVAL_MS = 60 * 60 * 1000;  // 1 ora
    qint64 lastStatsLogTime = QDateTime::currentMSecsSinceEpoch();
    
    // Backpressure
    int framesSinceLastCheck = 0;
    
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        setState(CameraState::Connecting);
        
        if (openStream()) {
            setState(CameraState::Connected);
            m_consecutiveFailures.store(0, std::memory_order_relaxed);
            lastSuccessfulFrameTime = QDateTime::currentMSecsSinceEpoch();
            connectionStartTime = QDateTime::currentMSecsSinceEpoch();
            lastStatsLogTime = QDateTime::currentMSecsSinceEpoch();
            framesSinceLastCheck = 0;
            emit connected();
            
            while (!m_stopRequested.load(std::memory_order_acquire) && 
                   !m_reconnectRequested.load(std::memory_order_acquire)) {
                
                if (!decodeFrame()) {
                    if (m_stopRequested.load(std::memory_order_acquire)) break;
                    
                    m_consecutiveFailures.fetch_add(1, std::memory_order_relaxed);
                    m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
                    
                    QString err = QString("Frame decode failed (failures: %1)").arg(m_consecutiveFailures.load());
                    emit errorOccurred(err);
                    
                    if (m_consecutiveFailures.load() >= 10) {
                        qWarning() << "Too many decode failures, forcing reconnect for camera" << m_config.id;
                        break;
                    }
                    
                    QThread::msleep(100);
                    continue;
                }
                
                // Frame decodificato con successo - aggiorna watchdog
                lastSuccessfulFrameTime = QDateTime::currentMSecsSinceEpoch();
                
                m_totalFrames.fetch_add(1, std::memory_order_relaxed);
                m_frameCounter.fetch_add(1, std::memory_order_relaxed);
                m_lastFrameTime.store(lastSuccessfulFrameTime, std::memory_order_relaxed);
                
                // BACKPRESSURE: ogni 30 frame yield per non saturare coda eventi
                framesSinceLastCheck++;
                if (framesSinceLastCheck >= 30) {
                    framesSinceLastCheck = 0;
                    QThread::yieldCurrentThread();
                }
                
                // === CONTROLLI PERIODICI (ogni frame) ===
                qint64 now = QDateTime::currentMSecsSinceEpoch();
                
                // 1. WATCHDOG: se nessun frame per troppo tempo, forza reconnect
                if (now - lastSuccessfulFrameTime > WATCHDOG_TIMEOUT_MS) {
                    qWarning() << "Watchdog timeout (" << WATCHDOG_TIMEOUT_MS << "ms) - no frames for camera" << m_config.id << "- forcing reconnect";
                    m_lastError = QString("Watchdog timeout: no frames for %1ms").arg(WATCHDOG_TIMEOUT_MS);
                    break;
                }
                
                // 2. FORCED RECONNECT: ogni 6 ore ricrea connessione (pulizia stato FFmpeg)
                if (now - connectionStartTime > FORCED_RECONNECT_INTERVAL_MS) {
                    qInfo() << "Periodic reconnect (6h) for camera" << m_config.id << "- refreshing FFmpeg state";
                    m_lastError = "Periodic maintenance reconnect";
                    break;
                }
                
                // 3. STATS LOG: ogni ora logga statistiche per debug
                if (now - lastStatsLogTime > STATS_LOG_INTERVAL_MS) {
                    lastStatsLogTime = now;
                    uint64_t total = m_totalFrames.load(std::memory_order_relaxed);
                    uint64_t dropped = m_droppedFrames.load(std::memory_order_relaxed);
                    int failures = m_consecutiveFailures.load(std::memory_order_relaxed);
                    qint64 uptimeSec = (now - connectionStartTime) / 1000;
                    double avgFps = uptimeSec > 0 ? (double)total / uptimeSec : 0;
                    double dropRate = total > 0 ? (double)dropped / total * 100 : 0;
                    
                    qInfo() << QString("Camera %1 stats: uptime=%2h frames=%3 dropped=%4 (%.1f%%) fps=%.1f failures=%5")
                        .arg(m_config.id)
                        .arg(uptimeSec / 3600)
                        .arg(total)
                        .arg(dropped)
                        .arg(dropRate)
                        .arg(avgFps)
                        .arg(failures);
                }
            }
            
            closeStream();
            
            if (m_stopRequested.load(std::memory_order_acquire)) {
                break;
            }
            
            m_reconnectRequested.store(false, std::memory_order_release);
            emit disconnected("Connection lost or reconnect requested");
        } else {
            m_consecutiveFailures.fetch_add(1, std::memory_order_relaxed);
            setState(CameraState::Reconnecting);
            
            QString err = QString("Connection failed (attempt %1): %2").arg(m_consecutiveFailures.load()).arg(m_lastError);
            emit errorOccurred(err);
        }
        
        if (!m_stopRequested.load(std::memory_order_acquire)) {
            setState(CameraState::Reconnecting);
            int waitTime = m_config.reconnectIntervalMs;
            
            int failures = m_consecutiveFailures.load();
            if (failures > 5) {
                waitTime = std::min(waitTime * (1 << std::min(failures - 5, 5)), 60000);
            }
            
            QMutexLocker locker(m_mutex.get());
            m_waitCondition->wait(m_mutex.get(), waitTime);
        }
    }
    
    closeStream();
    cleanupFFmpeg();
    m_running.store(false, std::memory_order_release);
    setState(CameraState::Disconnected);
    
    qInfo() << "Stream worker stopped for camera" << m_config.id;
}

bool StreamWorker::initFFmpeg() {
    av_log_set_level(AV_LOG_WARNING);
    return true;
}

void StreamWorker::cleanupFFmpeg() {
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    
    if (m_rgbBuffer) {
        av_free(m_rgbBuffer);
        m_rgbBuffer = nullptr;
        m_rgbBufferSize = 0;
    }
    
    if (m_frameRGB) {
        av_frame_free(&m_frameRGB);
        m_frameRGB = nullptr;
    }
    
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }
    
    m_videoStreamIndex = -1;
}

bool StreamWorker::openStream() {
    cleanupFFmpeg();
    
    m_formatCtx = avformat_alloc_context();
    if (!m_formatCtx) {
        m_lastError = "Failed to allocate format context";
        return false;
    }
    
    AVDictionary* opts = nullptr;
    
    if (m_config.enableTcp) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    } else {
        av_dict_set(&opts, "rtsp_transport", "udp", 0);
    }
    
    av_dict_set(&opts, "stimeout", QString::number(m_config.connectionTimeoutMs * 1000).toUtf8().constData(), 0);
    av_dict_set(&opts, "rw_timeout", QString::number(m_config.receiveTimeoutMs * 1000).toUtf8().constData(), 0);
    
    if (m_config.enableBuffering) {
        av_dict_set(&opts, "buffer_size", "2097152", 0);  // 2MB buffer (era 1MB)
        av_dict_set(&opts, "max_delay", "1000000", 0);   // 1s max delay (era 500ms)
    }
    
    av_dict_set(&opts, "reorder_queue_size", "200", 0);  // Aumentato da 100
    
    // Opzioni extra per stabilità RTSP
    av_dict_set(&opts, "rtsp_flags", "prefer_tcp", 0);   // Forza TCP anche se UDP disponibile
    av_dict_set(&opts, "allowed_media_types", "video", 0);  // Solo video, niente audio
    
    m_formatCtx->flags |= AVFMT_FLAG_NONBLOCK | AVFMT_FLAG_DISCARD_CORRUPT;
    
    // Imposta max analyze duration per stream difficili (HEVC)
    m_formatCtx->max_analyze_duration = 5000000;  // 5 secondi (default 5M)
    m_formatCtx->probesize = 1000000;  // 1MB probe (default 5M)
    
    QString url = buildRtspUrl();
    QByteArray urlBytes = url.toUtf8();
    
    int ret = avformat_open_input(&m_formatCtx, urlBytes.constData(), nullptr, &opts);
    av_dict_free(&opts);
    
    if (ret < 0) {
        m_lastError = FFmpegError::fromAvError(ret).message;
        qWarning() << "Camera" << m_config.id << "failed to open stream:" << m_lastError;
        return false;
    }
    
    ret = avformat_find_stream_info(m_formatCtx, nullptr);
    if (ret < 0) {
        m_lastError = FFmpegError::fromAvError(ret).message;
        qWarning() << "Camera" << m_config.id << "failed to find stream info:" << m_lastError;
        return false;
    }
    
    const AVCodec* codecPtr = nullptr;
    m_videoStreamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codecPtr, 0);
    if (m_videoStreamIndex < 0) {
        m_lastError = "No video stream found";
        qWarning() << "Camera" << m_config.id << ": no video stream found";
        return false;
    }
    
    // Get stream codec parameters to determine codec ID
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    enum AVCodecID codecId = stream->codecpar->codec_id;
    
    // === DETERMINE CODEC (Hardware or Software) ===
    const AVCodec* selectedCodec = codecPtr;  // Default: software decoder
    
    if (m_config.enableHwAccel && !m_config.hwAccelType.isEmpty()) {
        QString hwType = m_config.hwAccelType.toLower();
        const char* hwDecoderName = nullptr;
        
        if (hwType == "qsv") {
            // Intel QuickSync via QSV
            hwDecoderName = (codecId == AV_CODEC_ID_HEVC) ? "hevc_qsv" :
                           (codecId == AV_CODEC_ID_H264) ? "h264_qsv" : nullptr;
        } else if (hwType == "cuda" || hwType == "nvdec") {
            // NVIDIA NVDEC
            hwDecoderName = (codecId == AV_CODEC_ID_HEVC) ? "hevc_cuvid" :
                           (codecId == AV_CODEC_ID_H264) ? "h264_cuvid" : nullptr;
        } else if (hwType == "vaapi") {
            // VAAPI (Intel/AMD on Linux)
            hwDecoderName = (codecId == AV_CODEC_ID_HEVC) ? "hevc_vaapi" :
                           (codecId == AV_CODEC_ID_H264) ? "h264_vaapi" : nullptr;
        }
        
        if (hwDecoderName) {
            const AVCodec* hwCodec = avcodec_find_decoder_by_name(hwDecoderName);
            if (hwCodec) {
                selectedCodec = hwCodec;
                qInfo() << "Camera" << m_config.id << ": Hardware acceleration enabled -" << m_config.hwAccelType.toUpper();
            } else {
                qWarning() << "Camera" << m_config.id << ": HW decoder" << hwDecoderName << "not available, using software";
            }
        }
    }
    
    m_codec = const_cast<AVCodec*>(selectedCodec);
    
    // Now configure and open with the SELECTED codec (HW or SW)
    configureCodecContext();
    
    ret = avcodec_open2(m_codecCtx, m_codec, nullptr);
    if (ret < 0) {
        m_lastError = FFmpegError::fromAvError(ret).message;
        qWarning() << "Camera" << m_config.id << "failed to open codec:" << m_lastError;
        
        // Fallback to software if HW failed
        if (selectedCodec != codecPtr) {
            qWarning() << "Camera" << m_config.id << ": HW decode failed, falling back to software";
            m_codec = const_cast<AVCodec*>(codecPtr);
            configureCodecContext();  // Reconfigure with software codec
            ret = avcodec_open2(m_codecCtx, m_codec, nullptr);
            if (ret < 0) {
                m_lastError = FFmpegError::fromAvError(ret).message;
                return false;
            }
        } else {
            return false;
        }
    }
    
    m_frame = av_frame_alloc();
    m_frameRGB = av_frame_alloc();
    m_packet = av_packet_alloc();
    
    if (!m_frame || !m_frameRGB || !m_packet) {
        m_lastError = "Failed to allocate frames/packet";
        return false;
    }
    
    m_swsCtx = sws_getContext(
        m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt,
        m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!m_swsCtx) {
        m_lastError = "Failed to create scaling context";
        return false;
    }
    
    m_rgbBufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, m_codecCtx->width, m_codecCtx->height, 1);
    m_rgbBuffer = static_cast<uint8_t*>(av_malloc(m_rgbBufferSize));
    
    if (!m_rgbBuffer) {
        m_lastError = "Failed to allocate RGB buffer";
        return false;
    }
    
    ret = av_image_fill_arrays(m_frameRGB->data, m_frameRGB->linesize, m_rgbBuffer, 
                               AV_PIX_FMT_RGB24, m_codecCtx->width, m_codecCtx->height, 1);
    if (ret < 0) {
        m_lastError = FFmpegError::fromAvError(ret).message;
        return false;
    }
    
    qInfo() << "Camera" << m_config.id << "connected:" << m_codecCtx->width << "x" << m_codecCtx->height 
            << "fps:" << av_q2d(m_codecCtx->framerate);
    
    return true;
}

void StreamWorker::closeStream() {
    if (m_codecCtx) {
        // Flush decoder: manda packet vuoto per svuotare buffer
        avcodec_send_packet(m_codecCtx, nullptr);
        AVFrame* flushFrame = av_frame_alloc();
        while (avcodec_receive_frame(m_codecCtx, flushFrame) == 0) {
            av_frame_unref(flushFrame);
        }
        av_frame_free(&flushFrame);
        
        avcodec_flush_buffers(m_codecCtx);
    }
}

bool StreamWorker::decodeFrame() {
    if (!m_formatCtx || !m_codecCtx || !m_frame || !m_packet) {
        return false;
    }
    
    int ret = av_read_frame(m_formatCtx, m_packet);
    if (ret < 0) {
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            return false;
        }
        m_lastError = FFmpegError::fromAvError(ret).message;
        return false;
    }
    
    bool result = true;
    
    if (m_packet->stream_index == m_videoStreamIndex) {
        ret = avcodec_send_packet(m_codecCtx, m_packet);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                result = true;
            } else {
                m_lastError = FFmpegError::fromAvError(ret).message;
                result = false;
            }
        } else {
            while (ret >= 0) {
                ret = avcodec_receive_frame(m_codecCtx, m_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    m_lastError = FFmpegError::fromAvError(ret).message;
                    result = false;
                    break;
                }
                
                if (!processFrame(m_frame)) {
                    result = false;
                    av_frame_unref(m_frame);
                    break;
                }
                
                // RILASCIA il frame dopo averlo processato (fondamentale!)
                av_frame_unref(m_frame);
            }
        }
    }
    
    av_packet_unref(m_packet);
    return result;
}

bool StreamWorker::processFrame(AVFrame* frame) {
    int h = sws_scale(m_swsCtx, frame->data, frame->linesize, 0, m_codecCtx->height,
                      m_frameRGB->data, m_frameRGB->linesize);
    if (h < 0) {
        m_lastError = "Frame scaling failed";
        return false;
    }
    
    QImage image(m_rgbBuffer, m_codecCtx->width, m_codecCtx->height, 
                 m_frameRGB->linesize[0], QImage::Format_RGB888);
    
    QImage imageCopy = image.copy();
    
    if (imageCopy.isNull()) {
        return false;
    }
    
    uint32_t frameNum = m_frameCounter.fetch_add(1, std::memory_order_relaxed);
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    
    FrameData frameData(m_config.id, std::move(imageCopy), timestamp, frameNum);
    emit frameReady(std::move(frameData));
    
    return true;
}

void StreamWorker::configureCodecContext() {
    if (!m_formatCtx || m_videoStreamIndex < 0) return;
    
    AVStream* stream = m_formatCtx->streams[m_videoStreamIndex];
    m_codecCtx = avcodec_alloc_context3(m_codec);
    
    if (!m_codecCtx) return;
    
    avcodec_parameters_to_context(m_codecCtx, stream->codecpar);
    
    // Limita thread decoder per evitare accumulo frame
    m_codecCtx->thread_count = 1;  // Single thread = meno buffering interno
    m_codecCtx->thread_type = FF_THREAD_FRAME;
    
    // Error resilience
    m_codecCtx->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    m_codecCtx->err_recognition = AV_EF_CAREFUL;  // Meno aggressivo di EXPLODE
    
    // Skip processing per frame corrotti
    m_codecCtx->skip_frame = AVDISCARD_DEFAULT;
    m_codecCtx->skip_idct = AVDISCARD_DEFAULT;
    m_codecCtx->skip_loop_filter = AVDISCARD_DEFAULT;
    
    // Low latency flags
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST | AV_CODEC_FLAG2_LOCAL_HEADER;
    
    // Limita buffer interno decoder (FFmpeg 4.4+)
    // Imposta max frame buffer per evitare accumulo memoria
    av_opt_set_int(m_codecCtx, "max_frame_buffer", 3, 0);
    
    // Per HEVC/H.265: abilita error resilience extra
    if (m_codecCtx->codec_id == AV_CODEC_ID_HEVC || m_codecCtx->codec_id == AV_CODEC_ID_H265) {
        av_opt_set_int(m_codecCtx, "error_recognition", AV_EF_CAREFUL, 0);
    }
    
    // VAAPI hardware device context (if using VAAPI decoder)
    if (m_config.enableHwAccel && m_config.hwAccelType.toLower() == "vaapi") {
        AVBufferRef* hwDeviceCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
        if (ret >= 0) {
            m_codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
            av_buffer_unref(&hwDeviceCtx);
            qInfo() << "Camera" << m_config.id << ": VAAPI device context created";
        } else {
            qWarning() << "Camera" << m_config.id << ": Failed to create VAAPI device context";
        }
    }
}

void StreamWorker::setState(CameraState newState) {
    CameraState oldState = m_state.load(std::memory_order_acquire);
    if (oldState != newState) {
        m_state.store(newState, std::memory_order_release);
        emit stateChanged(newState);
    }
}

QString StreamWorker::buildRtspUrl() const {
    // Use sub-stream if enabled and available
    QString url = m_config.getActiveUrl();
    
    if (url.contains("@")) {
        return url;
    }
    
    if (!m_config.username.isEmpty() && !m_config.password.isEmpty()) {
        int schemeEnd = url.indexOf("://");
        if (schemeEnd != -1) {
            int hostStart = schemeEnd + 3;
            url.insert(hostStart, m_config.username + ":" + m_config.password + "@");
        }
    }
    
    return url;
}
