#ifndef OMNIGUARD_STREAM_WORKER_H
#define OMNIGUARD_STREAM_WORKER_H

#include "Common.h"
#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QImage>
#include <QSize>
#include <atomic>
#include <memory>

// Forward declare FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVDictionary;

// Risultato di un test di connessione (usato dal dialog di configurazione)
struct ProbeResult {
    bool ok = false;
    QString message;    // descrizione errore oppure "H264 1920x1080 @ 25 fps"
};

// ============================================================================
// StreamWorker
//
// Modello di threading:
//  - L'oggetto StreamWorker vive nel thread GUI (NON viene spostato con
//    moveToThread). Possiede un QThread che esegue run() in loop.
//  - Il thread worker comunica con la GUI solo tramite:
//      * atomics (stato, contatori, flag di stop/reconnect)
//      * una "casella" con l'ultimo frame (m_frameMutex): la GUI prende sempre
//        il frame più recente, i frame vecchi vengono sovrascritti. La coda
//        eventi Qt contiene al massimo UN evento frame per camera, quindi non
//        può mai crescere e la GUI non può restare indietro.
//      * segnali emessi in QueuedConnection.
//  - Ogni chiamata FFmpeg bloccante (open, read) è interrompibile tramite
//    interrupt_callback con scadenza: un RTSP che si "pianta" non blocca mai
//    il thread e stop() termina sempre in tempi brevi.
// ============================================================================
class StreamWorker : public QObject {
    Q_OBJECT
public:
    explicit StreamWorker(const CameraConfig& config, QObject* parent = nullptr);
    ~StreamWorker() override;

    StreamWorker(const StreamWorker&) = delete;
    StreamWorker& operator=(const StreamWorker&) = delete;

    // Avvia il thread di acquisizione
    void start();

    // Chiede l'arresto (non bloccante) e attende la fine del thread (bloccante, max qualche centinaio di ms)
    void stop();
    void requestStop();
    bool wait(unsigned long ms);

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }
    int cameraId() const { return m_cameraId; }
    CameraState getState() const { return m_state.load(std::memory_order_acquire); }
    CameraStatus getStatus() const;

    void requestReconnect();

    // Thread-safe: la nuova configurazione viene applicata dal thread worker alla prossima iterazione
    void updateConfig(const CameraConfig& config);

    // Thread-safe: dimensione (in pixel fisici) in cui la GUI disegnerà il video.
    // Il worker ridimensiona direttamente in sws_scale: la GUI non deve più scalare nulla.
    void setTargetSize(const QSize& size);

    // Chiamato dalla GUI dopo frameAvailable(): restituisce l'ultimo frame (può essere nullo)
    QImage takeLatestFrame();

    // Test di connessione sincrono (da chiamare in un thread secondario!).
    // 'cancel' può essere impostato da un altro thread per interrompere subito.
    static ProbeResult probe(const CameraConfig& config, const std::atomic<bool>* cancel);

signals:
    // Esiste un nuovo frame da prendere con takeLatestFrame()
    void frameAvailable(int cameraId);
    void stateChanged(CameraState newState);
    void errorOccurred(const QString& message);
    void connected();
    void disconnected(const QString& reason);

private:
    void run();

    bool openStream();
    void closeStream();

    // Legge un pacchetto e lo decodifica. Ritorna false se la connessione va ricreata.
    bool readAndDecode();
    void handleDecodedFrame(AVFrame* frame);

    void configureCodecContext(const AVCodec* codec, bool useVaapiDevice);
    AVDictionary* createFormatOptions() const;

    void setState(CameraState newState);
    void setLastError(const QString& err);
    void reportError(const QString& err);   // emette errorOccurred con rate limit

    // Interrupt callback FFmpeg
    static int interruptCallback(void* opaque);
    void armDeadline(int timeoutMs);
    void disarmDeadline();

    bool waitInterruptible(int ms);  // ritorna false se è stato chiesto lo stop
    void applyPendingConfig();

    const int m_cameraId;

    // Configurazione: m_config è usata SOLO dal thread worker.
    // La GUI scrive m_pendingConfig sotto mutex.
    CameraConfig m_config;
    QMutex m_configMutex;
    std::unique_ptr<CameraConfig> m_pendingConfig;
    std::atomic<bool> m_hasPendingConfig{false};

    // Thread control
    std::unique_ptr<QThread> m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_reconnectRequested{false};
    QMutex m_waitMutex;
    QWaitCondition m_waitCondition;

    // Scadenza per l'operazione FFmpeg in corso (ns steady clock, 0 = nessuna)
    std::atomic<int64_t> m_deadlineNs{0};

    // FFmpeg (solo thread worker)
    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_swFrame = nullptr;     // per i frame scaricati dalla GPU (VAAPI)
    AVPacket* m_packet = nullptr;
    SwsContext* m_swsCtx = nullptr;
    int m_videoStreamIndex = -1;
    int m_decodeErrorsInRow = 0;
    int64_t m_lastFrameNs = 0;        // ultimo frame decodificato (steady clock)
    int64_t m_lastEmitNs = 0;         // ultimo frame consegnato alla GUI

    // Dimensione desiderata dalla GUI (w << 32 | h)
    std::atomic<uint64_t> m_targetSize{0};

    // Casella "ultimo frame"
    QMutex m_frameMutex;
    QImage m_latestFrame;
    std::atomic<bool> m_frameNotifyPending{false};

    // Status (letto dalla GUI)
    std::atomic<CameraState> m_state{CameraState::Disconnected};
    std::atomic<int> m_consecutiveFailures{0};
    std::atomic<uint64_t> m_totalFrames{0};
    std::atomic<uint64_t> m_displayedFrames{0};
    std::atomic<uint64_t> m_droppedFrames{0};
    std::atomic<qint64> m_lastFrameTime{0};
    mutable QMutex m_errorMutex;
    QString m_lastError;

    // Rate limit errori verso GUI/log
    QString m_lastReportedError;
    int64_t m_lastReportNs = 0;
};

#endif // OMNIGUARD_STREAM_WORKER_H
