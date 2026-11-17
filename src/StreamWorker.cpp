#include "StreamWorker.h"
#include <QDateTime>
#include <QDebug>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

namespace {

// Orologio monotono: non salta con NTP / cambio ora legale (fondamentale per 24/7)
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr int64_t MS = 1'000'000;            // ns in 1 ms
constexpr int64_t SEC = 1000 * MS;

constexpr int64_t FORCED_RECONNECT_INTERVAL_NS = 6LL * 3600 * SEC;  // pulizia stato FFmpeg ogni 6h
constexpr int64_t STATS_LOG_INTERVAL_NS = 3600 * SEC;
constexpr int64_t ERROR_REPORT_INTERVAL_NS = 10 * SEC;
constexpr int MAX_DECODE_ERRORS_IN_ROW = 150;      // ~6s di pacchetti corrotti a 25fps
constexpr int CLOSE_TIMEOUT_MS = 2000;              // tempo massimo per il TEARDOWN RTSP

QString avErrorString(int err) {
    return FFmpegError::fromAvError(err).message;
}

// Thread semplice che esegue una funzione (riavviabile, a differenza di QThread::create)
class FunctionThread : public QThread {
public:
    explicit FunctionThread(std::function<void()> fn) : m_fn(std::move(fn)) {}
protected:
    void run() override { m_fn(); }
private:
    std::function<void()> m_fn;
};

// Opzioni comuni a worker e test di connessione
AVDictionary* buildFormatOptions(const CameraConfig& cfg) {
    AVDictionary* opts = nullptr;

    av_dict_set(&opts, "rtsp_transport", cfg.enableTcp ? "tcp" : "udp", 0);

    // Timeout socket in microsecondi.
    // FFmpeg >= 5 (libavformat 59) usa "timeout"; in FFmpeg 4.x "timeout" significava
    // "attendi connessioni in ingresso" (modalità listen!) e si usava "stimeout".
    const QByteArray sockTimeout = QByteArray::number(qint64(cfg.receiveTimeoutMs) * 1000);
#if LIBAVFORMAT_VERSION_MAJOR >= 59
    av_dict_set(&opts, "timeout", sockTimeout.constData(), 0);
#else
    av_dict_set(&opts, "stimeout", sockTimeout.constData(), 0);
#endif

    // Bassa latenza, scarta pacchetti corrotti
    av_dict_set(&opts, "fflags", "nobuffer+discardcorrupt", 0);
    av_dict_set(&opts, "reorder_queue_size", cfg.enableBuffering ? "64" : "16", 0);
    av_dict_set(&opts, "max_delay", cfg.enableBuffering ? "500000" : "100000", 0);
    av_dict_set(&opts, "buffer_size", cfg.enableBuffering ? "1048576" : "262144", 0);

    // Solo video: niente audio da ricevere/decodificare
    av_dict_set(&opts, "allowed_media_types", "video", 0);
    return opts;
}

struct ProbeInterrupt {
    const std::atomic<bool>* cancel = nullptr;
    int64_t deadlineNs = 0;
};

int probeInterruptCb(void* opaque) {
    auto* p = static_cast<ProbeInterrupt*>(opaque);
    if (p->cancel && p->cancel->load(std::memory_order_relaxed)) return 1;
    return (p->deadlineNs != 0 && nowNs() > p->deadlineNs) ? 1 : 0;
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

StreamWorker::StreamWorker(const CameraConfig& config, QObject* parent)
    : QObject(parent)
    , m_cameraId(config.id)
    , m_config(config)
    , m_thread(std::make_unique<FunctionThread>([this] { run(); })) {
    m_thread->setObjectName(QString("cam%1").arg(m_cameraId));
    qRegisterMetaType<CameraState>("CameraState");
}

StreamWorker::~StreamWorker() {
    stop();
    closeStream();  // nel caso il thread non fosse mai partito
}

void StreamWorker::start() {
    if (m_thread->isRunning()) {
        qWarning() << "Worker already running for camera" << m_cameraId;
        return;
    }
    m_stopRequested.store(false, std::memory_order_release);
    m_reconnectRequested.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread->start(QThread::NormalPriority);
}

void StreamWorker::requestStop() {
    QMutexLocker locker(&m_waitMutex);
    m_stopRequested.store(true, std::memory_order_release);
    m_waitCondition.wakeAll();
}

void StreamWorker::stop() {
    requestStop();
    // Tutte le chiamate FFmpeg bloccanti sono interrompibili: il thread esce in pochi ms.
    if (m_thread->isRunning()) {
        m_thread->wait();
    }
}

bool StreamWorker::wait(unsigned long ms) {
    return m_thread->wait(QDeadlineTimer(qint64(ms)));
}

void StreamWorker::requestReconnect() {
    QMutexLocker locker(&m_waitMutex);
    m_reconnectRequested.store(true, std::memory_order_release);
    m_waitCondition.wakeAll();
}

void StreamWorker::updateConfig(const CameraConfig& config) {
    if (config.id != m_cameraId) return;
    {
        QMutexLocker locker(&m_configMutex);
        m_pendingConfig = std::make_unique<CameraConfig>(config);
        m_hasPendingConfig.store(true, std::memory_order_release);
    }
    requestReconnect();
}

void StreamWorker::applyPendingConfig() {
    if (!m_hasPendingConfig.load(std::memory_order_acquire)) return;
    QMutexLocker locker(&m_configMutex);
    if (m_pendingConfig) {
        m_config = *m_pendingConfig;
        m_pendingConfig.reset();
    }
    m_hasPendingConfig.store(false, std::memory_order_release);
}

void StreamWorker::setTargetSize(const QSize& size) {
    const uint64_t w = uint64_t(std::max(0, size.width()));
    const uint64_t h = uint64_t(std::max(0, size.height()));
    m_targetSize.store((w << 32) | h, std::memory_order_relaxed);
}

QImage StreamWorker::takeLatestFrame() {
    QMutexLocker locker(&m_frameMutex);
    QImage img = std::move(m_latestFrame);
    m_latestFrame = QImage();
    m_frameNotifyPending.store(false, std::memory_order_release);
    return img;
}

CameraStatus StreamWorker::getStatus() const {
    CameraStatus status;
    status.state = m_state.load(std::memory_order_acquire);
    status.consecutiveFailures = m_consecutiveFailures.load(std::memory_order_relaxed);
    status.totalFrames = m_totalFrames.load(std::memory_order_relaxed);
    status.displayedFrames = m_displayedFrames.load(std::memory_order_relaxed);
    status.droppedFrames = m_droppedFrames.load(std::memory_order_relaxed);
    status.lastFrameTime = m_lastFrameTime.load(std::memory_order_relaxed);
    QMutexLocker locker(&m_errorMutex);
    status.lastError = m_lastError;
    return status;
}

// ============================================================================
// Main loop (thread worker)
// ============================================================================

void StreamWorker::run() {
    qInfo() << "Stream worker started for camera" << m_cameraId;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        applyPendingConfig();
        m_reconnectRequested.store(false, std::memory_order_release);

        setState(CameraState::Connecting);
        bool reconnectImmediately = false;

        if (openStream()) {
            setState(CameraState::Connected);
            m_consecutiveFailures.store(0, std::memory_order_relaxed);
            setLastError(QString());
            emit connected();

            const int64_t connectionStart = nowNs();
            int64_t lastStatsLog = connectionStart;
            m_lastFrameNs = connectionStart;
            m_lastEmitNs = 0;
            m_decodeErrorsInRow = 0;
            const int64_t watchdogNs = std::max<int64_t>(15 * SEC, 2LL * m_config.receiveTimeoutMs * MS);
            QString reason;

            while (!m_stopRequested.load(std::memory_order_acquire) &&
                   !m_reconnectRequested.load(std::memory_order_acquire)) {

                if (!readAndDecode()) {
                    QMutexLocker locker(&m_errorMutex);
                    reason = m_lastError;
                    break;
                }

                const int64_t now = nowNs();

                // WATCHDOG: arrivano pacchetti ma nessun frame decodificabile
                if (now - m_lastFrameNs > watchdogNs) {
                    reason = QString("Nessun frame valido da %1 s").arg(watchdogNs / SEC);
                    qWarning() << "Camera" << m_cameraId << "watchdog:" << reason;
                    setLastError(reason);
                    break;
                }

                if (now - connectionStart > FORCED_RECONNECT_INTERVAL_NS) {
                    qInfo() << "Camera" << m_cameraId << "periodic reconnect (6h)";
                    reason = "Riconnessione periodica di manutenzione";
                    reconnectImmediately = true;
                    break;
                }

                if (now - lastStatsLog > STATS_LOG_INTERVAL_NS) {
                    lastStatsLog = now;
                    qInfo().noquote() << QString("Camera %1 stats: uptime=%2h decoded=%3 displayed=%4 dropped=%5")
                        .arg(m_cameraId)
                        .arg((now - connectionStart) / (3600 * SEC))
                        .arg(m_totalFrames.load())
                        .arg(m_displayedFrames.load())
                        .arg(m_droppedFrames.load());
                }
            }

            if (m_reconnectRequested.load(std::memory_order_acquire)) {
                reason = "Riconnessione richiesta (configurazione cambiata)";
                reconnectImmediately = true;
            }

            closeStream();
            if (m_stopRequested.load(std::memory_order_acquire)) break;
            emit disconnected(reason.isEmpty() ? QString("Connessione persa") : reason);
        } else {
            closeStream();
            if (m_stopRequested.load(std::memory_order_acquire)) break;
            if (m_reconnectRequested.load(std::memory_order_acquire)) continue;  // config cambiata durante l'apertura

            const int failures = m_consecutiveFailures.fetch_add(1, std::memory_order_relaxed) + 1;
            QString err;
            {
                QMutexLocker locker(&m_errorMutex);
                err = m_lastError;
            }
            reportError(QString("Connessione fallita (tentativo %1): %2").arg(failures).arg(err));
        }

        if (m_stopRequested.load(std::memory_order_acquire)) break;
        if (reconnectImmediately) continue;

        setState(CameraState::Reconnecting);

        // Backoff esponenziale dopo 5 fallimenti, max 60s
        int waitTime = std::max(500, m_config.reconnectIntervalMs);
        const int failures = m_consecutiveFailures.load(std::memory_order_relaxed);
        if (failures > 5) {
            waitTime = std::min(waitTime * (1 << std::min(failures - 5, 5)), 60000);
        }
        if (!waitInterruptible(waitTime)) break;
    }

    closeStream();
    m_running.store(false, std::memory_order_release);
    setState(CameraState::Disconnected);
    qInfo() << "Stream worker stopped for camera" << m_cameraId;
}

bool StreamWorker::waitInterruptible(int ms) {
    QMutexLocker locker(&m_waitMutex);
    // I flag vengono controllati sotto lo stesso mutex usato da stop()/requestReconnect():
    // impossibile perdere la sveglia (prima si poteva attendere fino a 60s in chiusura).
    if (m_stopRequested.load(std::memory_order_acquire)) return false;
    if (m_reconnectRequested.load(std::memory_order_acquire)) return true;
    m_waitCondition.wait(&m_waitMutex, QDeadlineTimer(qint64(ms)));
    return !m_stopRequested.load(std::memory_order_acquire);
}

// ============================================================================
// Interrupt callback: rende interrompibile ogni I/O bloccante di FFmpeg
// ============================================================================

int StreamWorker::interruptCallback(void* opaque) {
    auto* self = static_cast<StreamWorker*>(opaque);
    if (self->m_stopRequested.load(std::memory_order_relaxed)) return 1;
    if (self->m_reconnectRequested.load(std::memory_order_relaxed)) return 1;
    const int64_t deadline = self->m_deadlineNs.load(std::memory_order_relaxed);
    return (deadline != 0 && nowNs() > deadline) ? 1 : 0;
}

void StreamWorker::armDeadline(int timeoutMs) {
    m_deadlineNs.store(nowNs() + int64_t(std::max(500, timeoutMs)) * MS, std::memory_order_relaxed);
}

void StreamWorker::disarmDeadline() {
    m_deadlineNs.store(0, std::memory_order_relaxed);
}

// ============================================================================
// FFmpeg
// ============================================================================

AVDictionary* StreamWorker::createFormatOptions() const {
    return buildFormatOptions(m_config);
}

bool StreamWorker::openStream() {
    closeStream();

    m_formatCtx = avformat_alloc_context();
    if (!m_formatCtx) {
        setLastError("Impossibile allocare il format context");
        return false;
    }
    m_formatCtx->interrupt_callback.callback = &StreamWorker::interruptCallback;
    m_formatCtx->interrupt_callback.opaque = this;
    // NB: niente AVFMT_FLAG_NONBLOCK: faceva tornare EAGAIN che veniva contato come
    // errore e provocava riconnessioni inutili. La reattività ora la dà l'interrupt callback.
    m_formatCtx->flags |= AVFMT_FLAG_DISCARD_CORRUPT;
    m_formatCtx->max_analyze_duration = 2 * AV_TIME_BASE;
    m_formatCtx->probesize = 500000;

    AVDictionary* opts = createFormatOptions();
    const QByteArray url = m_config.buildStreamUrl().toUtf8();

    armDeadline(m_config.connectionTimeoutMs);
    int ret = avformat_open_input(&m_formatCtx, url.constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        disarmDeadline();
        // avformat_open_input libera il contesto in caso di errore
        m_formatCtx = nullptr;
        setLastError(ret == AVERROR_EXIT ? QString("Timeout di connessione") : avErrorString(ret));
        qWarning() << "Camera" << m_cameraId << "failed to open stream:" << m_lastError;
        return false;
    }

    ret = avformat_find_stream_info(m_formatCtx, nullptr);
    disarmDeadline();
    if (ret < 0) {
        setLastError(ret == AVERROR_EXIT ? QString("Timeout lettura informazioni stream") : avErrorString(ret));
        qWarning() << "Camera" << m_cameraId << "failed to find stream info:" << m_lastError;
        return false;
    }

    const AVCodec* swCodec = nullptr;
    m_videoStreamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &swCodec, 0);
    if (m_videoStreamIndex < 0 || !swCodec) {
        setLastError("Nessuno stream video trovato");
        return false;
    }
    // Ignora gli altri stream a livello di demuxer
    for (unsigned i = 0; i < m_formatCtx->nb_streams; ++i) {
        if (int(i) != m_videoStreamIndex) m_formatCtx->streams[i]->discard = AVDISCARD_ALL;
    }

    const AVCodecID codecId = m_formatCtx->streams[m_videoStreamIndex]->codecpar->codec_id;

    // === Selezione decoder (HW o SW) ===
    const AVCodec* codec = swCodec;
    bool useVaapi = false;
    if (m_config.enableHwAccel) {
        const QString hwType = m_config.hwAccelType.toLower();
        const char* hwName = nullptr;
        if (hwType == "qsv") {
            hwName = codecId == AV_CODEC_ID_HEVC ? "hevc_qsv" : codecId == AV_CODEC_ID_H264 ? "h264_qsv" : nullptr;
        } else if (hwType == "cuda" || hwType == "nvdec") {
            hwName = codecId == AV_CODEC_ID_HEVC ? "hevc_cuvid" : codecId == AV_CODEC_ID_H264 ? "h264_cuvid" : nullptr;
        } else if (hwType == "vaapi") {
            // VAAPI non è un decoder separato: si usa il decoder software con un device VAAPI
            useVaapi = true;
        }
        if (hwName) {
            if (const AVCodec* hw = avcodec_find_decoder_by_name(hwName)) {
                codec = hw;
            } else {
                qWarning() << "Camera" << m_cameraId << ": HW decoder" << hwName << "non disponibile, uso software";
            }
        }
    }

    configureCodecContext(codec, useVaapi);
    ret = m_codecCtx ? avcodec_open2(m_codecCtx, codec, nullptr) : AVERROR(ENOMEM);
    if (ret < 0 && (codec != swCodec || useVaapi)) {
        qWarning() << "Camera" << m_cameraId << ": decoder HW fallito (" << avErrorString(ret) << "), uso software";
        configureCodecContext(swCodec, false);  // libera il vecchio contesto (prima veniva perso)
        ret = m_codecCtx ? avcodec_open2(m_codecCtx, swCodec, nullptr) : AVERROR(ENOMEM);
    }
    if (ret < 0) {
        setLastError(QString("Apertura decoder fallita: %1").arg(avErrorString(ret)));
        return false;
    }

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_swFrame || !m_packet) {
        setLastError("Memoria insufficiente (frame/packet)");
        return false;
    }

    qInfo() << "Camera" << m_cameraId << "connected:" << m_codecCtx->codec->name
            << m_codecCtx->width << "x" << m_codecCtx->height;
    return true;
}

void StreamWorker::configureCodecContext(const AVCodec* codec, bool useVaapiDevice) {
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return;

    avcodec_parameters_to_context(m_codecCtx, m_formatCtx->streams[m_videoStreamIndex]->codecpar);

    // 1 thread per camera: con 3 camere su una CPU debole è la scelta giusta
    // (niente sovra-sottoscrizione dei core, nessun frame di latenza extra).
    m_codecCtx->thread_count = 1;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->err_recognition = AV_EF_CAREFUL;

    if (m_config.lowCpuDecode) {
        // Salta il filtro di deblocking: ~20-30% CPU in meno su H.264/H.265,
        // qualità leggermente inferiore (accettabile per videosorveglianza)
        m_codecCtx->skip_loop_filter = AVDISCARD_ALL;
    }

    if (useVaapiDevice) {
        AVBufferRef* hwDevice = nullptr;
        if (av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) >= 0) {
            m_codecCtx->hw_device_ctx = hwDevice;  // il codec ctx prende possesso del riferimento
            qInfo() << "Camera" << m_cameraId << ": VAAPI attivo";
        } else {
            qWarning() << "Camera" << m_cameraId << ": VAAPI non disponibile, uso software";
        }
    }
}

void StreamWorker::closeStream() {
    // Rilascia TUTTO, inclusa la connessione: durante l'attesa di riconnessione la
    // camera non resta con una sessione RTSP aperta (alcune Zosi accettano poche sessioni).
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_frame) av_frame_free(&m_frame);
    if (m_swFrame) av_frame_free(&m_swFrame);
    if (m_packet) av_packet_free(&m_packet);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    if (m_formatCtx) {
        armDeadline(CLOSE_TIMEOUT_MS);  // il TEARDOWN RTSP non deve poter bloccare
        avformat_close_input(&m_formatCtx);
        disarmDeadline();
    }
    m_videoStreamIndex = -1;
}

bool StreamWorker::readAndDecode() {
    armDeadline(m_config.receiveTimeoutMs);
    int ret = av_read_frame(m_formatCtx, m_packet);
    disarmDeadline();

    if (ret == AVERROR(EAGAIN)) {
        QThread::msleep(5);
        return true;
    }
    if (ret < 0) {
        if (ret == AVERROR_EXIT) {
            setLastError(m_stopRequested || m_reconnectRequested
                         ? QString("Interrotto")
                         : QString("Nessun dato ricevuto da %1 ms").arg(m_config.receiveTimeoutMs));
        } else if (ret == AVERROR_EOF) {
            setLastError("Stream terminato dalla camera");
        } else {
            setLastError(avErrorString(ret));
        }
        return false;
    }

    if (m_packet->stream_index != m_videoStreamIndex) {
        av_packet_unref(m_packet);
        return true;
    }

    ret = avcodec_send_packet(m_codecCtx, m_packet);
    av_packet_unref(m_packet);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        if (++m_decodeErrorsInRow >= MAX_DECODE_ERRORS_IN_ROW) {
            setLastError(QString("Troppi errori di decodifica: %1").arg(avErrorString(ret)));
            return false;
        }
        reportError(QString("Errore decodifica: %1").arg(avErrorString(ret)));
        return true;
    }

    while (true) {
        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            ++m_decodeErrorsInRow;
            break;
        }
        // Frame valido: azzera i contatori di errore (prima non succedeva mai durante
        // la sessione, quindi errori sporadici si accumulavano fino a forzare il reconnect)
        m_decodeErrorsInRow = 0;
        m_lastFrameNs = nowNs();
        m_totalFrames.fetch_add(1, std::memory_order_relaxed);
        m_lastFrameTime.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);

        handleDecodedFrame(m_frame);
        av_frame_unref(m_frame);
    }
    return true;
}

void StreamWorker::handleDecodedFrame(AVFrame* frame) {
    const int64_t now = nowNs();

    // 1) La GUI non ha ancora preso il frame precedente: è occupata, non sprecare CPU
    //    a convertirne un altro. (Il decoder deve comunque decodificare tutto.)
    if (m_frameNotifyPending.load(std::memory_order_acquire)) {
        m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 2) Limite FPS di visualizzazione (con tolleranza del 25% per il jitter di rete)
    const int64_t minInterval = SEC / std::clamp(m_config.fps, 1, 60);
    if (m_lastEmitNs != 0 && now - m_lastEmitNs < minInterval - minInterval / 4) {
        m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 3) Frame in memoria GPU (VAAPI): scaricalo in RAM
    AVFrame* src = frame;
    if (frame->hw_frames_ctx) {
        av_frame_unref(m_swFrame);
        if (av_hwframe_transfer_data(m_swFrame, frame, 0) < 0) {
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        src = m_swFrame;
    }

    const int srcW = src->width;
    const int srcH = src->height;
    if (srcW <= 0 || srcH <= 0) return;

    // 4) Dimensione di uscita = dimensione del riquadro a schermo (mai ingrandire).
    //    Ridimensionare qui, nel thread della camera, toglie tutto il lavoro alla GUI.
    int dstW = srcW, dstH = srcH;
    const uint64_t packed = m_targetSize.load(std::memory_order_relaxed);
    const int tw = int(packed >> 32), th = int(packed & 0xffffffffu);
    if (tw > 0 && th > 0) {
        const double scale = std::min({double(tw) / srcW, double(th) / srcH, 1.0});
        dstW = std::max(2, int(std::lround(srcW * scale)));
        dstH = std::max(2, int(std::lround(srcH * scale)));
    }

    // sws_getCachedContext gestisce anche cambi di risoluzione a metà stream
    // (prima il contesto era fisso e un cambio di risoluzione leggeva fuori buffer)
    m_swsCtx = sws_getCachedContext(m_swsCtx,
                                    srcW, srcH, AVPixelFormat(src->format),
                                    dstW, dstH, AV_PIX_FMT_RGB32,
                                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        reportError("Conversione colore non supportata");
        return;
    }

    // Converte direttamente nel buffer della QImage (niente copia extra).
    // AV_PIX_FMT_RGB32 ha lo stesso layout di QImage::Format_RGB32: disegno senza conversioni.
    QImage image(dstW, dstH, QImage::Format_RGB32);
    if (image.isNull()) return;
    uint8_t* dstData[4] = {image.bits(), nullptr, nullptr, nullptr};
    int dstStride[4] = {int(image.bytesPerLine()), 0, 0, 0};
    if (sws_scale(m_swsCtx, src->data, src->linesize, 0, srcH, dstData, dstStride) <= 0) {
        return;
    }

    bool notify = false;
    {
        QMutexLocker locker(&m_frameMutex);
        m_latestFrame = std::move(image);
        notify = !m_frameNotifyPending.exchange(true, std::memory_order_acq_rel);
    }
    m_lastEmitNs = now;
    m_displayedFrames.fetch_add(1, std::memory_order_relaxed);
    if (notify) {
        emit frameAvailable(m_cameraId);
    }
}

// ============================================================================
// Helpers
// ============================================================================

void StreamWorker::setState(CameraState newState) {
    const CameraState old = m_state.exchange(newState, std::memory_order_acq_rel);
    if (old != newState) {
        emit stateChanged(newState);
    }
}

void StreamWorker::setLastError(const QString& err) {
    QMutexLocker locker(&m_errorMutex);
    m_lastError = err;
}

void StreamWorker::reportError(const QString& err) {
    // Rate limit: in 24h un errore ripetuto per ogni pacchetto riempirebbe log e coda eventi
    const int64_t now = nowNs();
    if (err == m_lastReportedError && now - m_lastReportNs < ERROR_REPORT_INTERVAL_NS) return;
    m_lastReportedError = err;
    m_lastReportNs = now;
    emit errorOccurred(err);
}

// ============================================================================
// Test connessione (thread secondario, mai nel thread GUI)
// ============================================================================

ProbeResult StreamWorker::probe(const CameraConfig& config, const std::atomic<bool>* cancel) {
    ProbeResult result;
    const QString url = config.buildStreamUrl();
    if (url.isEmpty()) {
        result.message = "URL vuoto";
        return result;
    }

    ProbeInterrupt irq;
    irq.cancel = cancel;

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        result.message = "Memoria insufficiente";
        return result;
    }
    fmt->interrupt_callback.callback = &probeInterruptCb;
    fmt->interrupt_callback.opaque = &irq;
    fmt->max_analyze_duration = 2 * AV_TIME_BASE;
    fmt->probesize = 500000;

    AVDictionary* opts = buildFormatOptions(config);
    irq.deadlineNs = nowNs() + int64_t(std::max(1000, config.connectionTimeoutMs)) * MS;
    const QByteArray urlBytes = url.toUtf8();
    int ret = avformat_open_input(&fmt, urlBytes.constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        result.message = ret == AVERROR_EXIT ? QString("Timeout di connessione") : avErrorString(ret);
        return result;  // fmt già liberato da FFmpeg
    }

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        result.message = ret == AVERROR_EXIT ? QString("Timeout lettura stream") : avErrorString(ret);
    } else {
        const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (idx < 0) {
            result.message = "Connesso, ma nessuno stream video trovato";
        } else {
            AVStream* st = fmt->streams[idx];
            const double fps = st->avg_frame_rate.den ? av_q2d(st->avg_frame_rate) : 0.0;
            result.ok = true;
            result.message = QString("%1  %2×%3%4")
                .arg(QString::fromUtf8(avcodec_get_name(st->codecpar->codec_id)).toUpper())
                .arg(st->codecpar->width)
                .arg(st->codecpar->height)
                .arg(fps > 0 ? QString("  @ %1 fps").arg(fps, 0, 'f', 0) : QString());
        }
    }

    irq.deadlineNs = nowNs() + CLOSE_TIMEOUT_MS * MS;
    avformat_close_input(&fmt);
    return result;
}
