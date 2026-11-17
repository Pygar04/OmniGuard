#include "MainWindow.h"
#include "StreamWorker.h"
#include "CameraConfig.h"
#include <QPainter>
#include <QApplication>
#include <QMessageBox>
#include <QSettings>
#include <QDebug>
#include <QDateTime>
#include <QPointer>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStyle>
#include <QThread>

namespace {

constexpr qint64 STALE_FRAME_MS = 5000;   // nessun frame da 5s con camera "connessa" → avviso a schermo

const char* stateText(CameraState s) {
    switch (s) {
        case CameraState::Disconnected: return "Disconnessa";
        case CameraState::Connecting:   return "Connessione...";
        case CameraState::Connected:    return "Connessa";
        case CameraState::Reconnecting: return "Riconnessione...";
        case CameraState::Error:        return "Errore";
    }
    return "";
}

QColor stateColor(CameraState s) {
    switch (s) {
        case CameraState::Connected:    return QColor(76, 195, 138);
        case CameraState::Connecting:   return QColor(240, 200, 80);
        case CameraState::Reconnecting: return QColor(255, 165, 0);
        case CameraState::Disconnected:
        case CameraState::Error:        return QColor(240, 97, 109);
    }
    return Qt::white;
}

} // namespace

// ============================================================================
// VideoWidget
// ============================================================================

VideoWidget::VideoWidget(int cameraId, const QString& name, QWidget* parent)
    : QWidget(parent)
    , m_cameraId(cameraId)
    , m_name(name) {
    setMinimumSize(240, 135);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);

    m_titleFont = font();
    m_titleFont.setPointSize(11);
    m_titleFont.setBold(true);
    m_smallFont = font();
    m_smallFont.setPointSize(9);
}

void VideoWidget::setImage(const QImage& image) {
    if (image.isNull()) return;
    m_currentFrame = image;
    m_lastFrameTimer.restart();
    m_lastFrameClock = QTime::currentTime().toString("HH:mm:ss");
    m_stale = false;
    update();
}

void VideoWidget::setState(CameraState state) {
    if (m_state == state) return;
    m_state = state;
    if (state == CameraState::Connected) m_errorText.clear();
    update();
}

void VideoWidget::setError(const QString& error) {
    m_errorText = error;
    update();
}

void VideoWidget::setName(const QString& name) {
    if (m_name == name) return;
    m_name = name;
    update();
}

void VideoWidget::setStale(bool stale) {
    if (m_stale == stale) return;
    m_stale = stale;
    update();
}

qint64 VideoWidget::msSinceLastFrame() const {
    return m_lastFrameTimer.isValid() ? m_lastFrameTimer.elapsed() : -1;
}

void VideoWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (!m_currentFrame.isNull()) {
        // Il frame arriva già alla dimensione giusta: di norma è un blit 1:1.
        // Solo durante un ridimensionamento della finestra (finché il worker non si adegua)
        // scaliamo qui, con trasformazione veloce.
        // (Se la sorgente è più piccola del riquadro il worker non ingrandisce: lo fa drawImage.)
        const QSize fitted = m_currentFrame.size().scaled(size(), Qt::KeepAspectRatio);
        QRect target(QPoint(0, 0), fitted);
        target.moveCenter(rect().center());
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(target, m_currentFrame);
    }

    const QColor color = stateColor(m_state);

    // Bordo di stato
    painter.setPen(QPen(m_stale ? QColor(240, 97, 109) : color, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(1, 1, -1, -1));

    // Barra superiore semitrasparente con nome e stato
    const int barH = 26;
    painter.fillRect(QRect(2, 2, width() - 4, barH), QColor(0, 0, 0, 150));

    painter.setFont(m_titleFont);
    painter.setPen(Qt::white);
    painter.drawText(QRect(10, 2, width() - 20, barH), Qt::AlignLeft | Qt::AlignVCenter, m_name);

    painter.setFont(m_smallFont);
    QString right;
    if (m_state == CameraState::Connected && !m_stale) {
        right = QString("● LIVE  %1").arg(m_lastFrameClock);
    } else {
        right = QString("● %1").arg(stateText(m_state));
    }
    painter.setPen(m_stale ? QColor(240, 97, 109) : color);
    painter.drawText(QRect(10, 2, width() - 20, barH), Qt::AlignRight | Qt::AlignVCenter, right);

    // Messaggi centrali
    QString center;
    if (m_stale && m_state == CameraState::Connected) {
        center = QString("Segnale assente\nultimo frame alle %1").arg(m_lastFrameClock);
    } else if (m_state != CameraState::Connected) {
        center = stateText(m_state);
        if (!m_errorText.isEmpty()) center += "\n" + m_errorText;
    }
    if (!center.isEmpty()) {
        QRect box = rect().adjusted(20, barH + 10, -20, -10);
        painter.setPen(Qt::white);
        QRect textRect = painter.boundingRect(box, Qt::AlignCenter | Qt::TextWordWrap, center);
        painter.fillRect(textRect.adjusted(-12, -8, 12, 8), QColor(0, 0, 0, 170));
        painter.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, center);
    }
}

void VideoWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    emit targetSizeChanged((QSizeF(size()) * devicePixelRatioF()).toSize());
}

// ============================================================================
// MainWindow
// ============================================================================

MainWindow::MainWindow(CameraConfigManager* configManager, QWidget* parent)
    : QMainWindow(parent)
    , m_configManager(configManager) {

    setupUI();
    setupMenuBar();
    setupStatusBar();
    createCameraWidgets();

    connect(m_configManager, &CameraConfigManager::configChanged,
            this, &MainWindow::onConfigChanged);

    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    restoreState(settings.value("window/state").toByteArray());

    m_uptime.start();
    QTimer::singleShot(100, this, &MainWindow::startWorkers);
}

MainWindow::~MainWindow() {
    stopWorkers();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    stopWorkers();
    event->accept();
}

void MainWindow::setupUI() {
    setWindowTitle("OmniGuard - Videosorveglianza Zosi (3 Camere)");
    resize(1280, 720);
    setMinimumSize(640, 400);
    setWindowIcon(QIcon(":/omniguard.svg"));

    // NB: niente regola generica "QWidget { background: black }": veniva ereditata
    // dal dialog di configurazione (figlio della finestra) rendendolo illeggibile.
    setStyleSheet(
        "QMainWindow { background-color: black; }"
        "QWidget#centralWidget { background-color: black; }"
        "QStatusBar { background-color: #15171a; color: #e6e8eb; }"
        "QStatusBar QLabel { color: #e6e8eb; padding: 0 6px; }"
        "QStatusBar::item { border: none; }"
        "QMenuBar { background-color: #15171a; color: #e6e8eb; }"
        "QMenuBar::item:selected { background-color: #2c3036; }"
        "QMenu { background-color: #1e2126; color: #e6e8eb; border: 1px solid #2c3036; }"
        "QMenu::item:selected { background-color: #2c3036; }");

    m_centralWidget = new QWidget(this);
    m_centralWidget->setObjectName("centralWidget");
    m_gridLayout = new QGridLayout(m_centralWidget);
    m_gridLayout->setSpacing(4);
    m_gridLayout->setContentsMargins(4, 4, 4, 4);
    setCentralWidget(m_centralWidget);
}

void MainWindow::setupMenuBar() {
    QMenuBar* menuBar = this->menuBar();

    QMenu* fileMenu = menuBar->addMenu("&File");
    QAction* configAction = fileMenu->addAction("&Configurazione Camere...", this, &MainWindow::openCameraConfig);
    configAction->setShortcut(QKeySequence::Preferences);
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction("&Esci", this, &QWidget::close);
    quitAction->setShortcut(QKeySequence::Quit);

    QMenu* viewMenu = menuBar->addMenu("&Visualizza");
    QAction* fullscreenAction = viewMenu->addAction("&Schermo Intero", this, [this]() {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });
    fullscreenAction->setShortcut(Qt::Key_F11);
    // Funziona anche a schermo intero (quando la barra menu non è cliccabile)
    addAction(fullscreenAction);
    addAction(configAction);

    viewMenu->addAction("&Riconnetti tutte le camere", this, [this]() {
        for (auto& w : m_workers) if (w) w->requestReconnect();
    });

    QMenu* helpMenu = menuBar->addMenu("&Aiuto");
    helpMenu->addAction("&Informazioni", this, [this]() {
        QMessageBox::about(this, "OmniGuard",
            "OmniGuard v1.0\n"
            "Sistema di videosorveglianza per 3 telecamere IP Zosi\n"
            "Basato su Qt 6 e FFmpeg");
    });
}

void MainWindow::openCameraConfig() {
    CameraConfigDialog dialog(m_configManager, this);
    // Le modifiche arrivano tramite CameraConfigManager::configChanged, emesso
    // solo per le camere effettivamente modificate (le altre non si riconnettono).
    dialog.exec();
}

void MainWindow::setupStatusBar() {
    m_statusBar = statusBar();
    m_statusBar->setSizeGripEnabled(false);

    for (int i = 0; i < 3; ++i) {
        m_statusLabels[i] = new QLabel(QString("Cam %1: avvio...").arg(i + 1));
        m_statusBar->addWidget(m_statusLabels[i]);
    }

    m_uptimeLabel = new QLabel();
    m_statusBar->addPermanentWidget(m_uptimeLabel);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(1000);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    m_statusTimer->start();
    m_fpsTimer.start();
}

void MainWindow::createCameraWidgets() {
    auto configs = m_configManager->getAllConfigs();

    for (int i = 0; i < 3; ++i) {
        QString name = configs[i].has_value() ? configs[i]->name : QString("Camera %1").arg(i + 1);
        m_videoWidgets[i] = std::make_unique<VideoWidget>(i, name);
        m_gridLayout->addWidget(m_videoWidgets[i].get(), i / 2, i % 2);
    }

    m_gridLayout->setRowStretch(0, 1);
    m_gridLayout->setRowStretch(1, 1);
    m_gridLayout->setColumnStretch(0, 1);
    m_gridLayout->setColumnStretch(1, 1);
}

void MainWindow::startWorkers() {
    auto configs = m_configManager->getAllConfigs();

    for (int i = 0; i < 3; ++i) {
        CameraConfig config = configs[i].has_value()
            ? configs[i].value()
            : CameraConfigManager::createDefaults()[i];

        m_workers[i] = std::make_unique<StreamWorker>(config);
        StreamWorker* worker = m_workers[i].get();
        VideoWidget* widget = m_videoWidgets[i].get();

        // Tutti i segnali dal thread della camera arrivano in coda alla GUI
        connect(worker, &StreamWorker::frameAvailable,
                this, &MainWindow::onFrameAvailable, Qt::QueuedConnection);
        connect(worker, &StreamWorker::stateChanged,
                this, [this, i](CameraState state) { onStateChanged(i, state); }, Qt::QueuedConnection);
        connect(worker, &StreamWorker::errorOccurred,
                this, [this, i](const QString& error) { onErrorOccurred(i, error); }, Qt::QueuedConnection);
        connect(worker, &StreamWorker::disconnected,
                this, [this, i](const QString& reason) { onErrorOccurred(i, reason); }, Qt::QueuedConnection);

        // Il worker scala i frame alla dimensione del riquadro
        connect(widget, &VideoWidget::targetSizeChanged, worker, &StreamWorker::setTargetSize);
        worker->setTargetSize((QSizeF(widget->size()) * widget->devicePixelRatioF()).toSize());

        worker->start();
    }
}

void MainWindow::stopWorkers() {
    if (m_workersStopped) return;
    m_workersStopped = true;

    // Prima chiede a tutti di fermarsi (in parallelo), poi attende
    for (auto& w : m_workers) if (w) w->requestStop();
    for (auto& w : m_workers) if (w) w->stop();
    for (auto& w : m_workers) w.reset();
}

void MainWindow::applyConfig(const CameraConfig& config) {
    if (config.id < 0 || config.id >= 3) return;
    if (m_videoWidgets[config.id]) m_videoWidgets[config.id]->setName(config.name);
    if (m_workers[config.id]) m_workers[config.id]->updateConfig(config);
}

void MainWindow::onFrameAvailable(int cameraId) {
    if (cameraId < 0 || cameraId >= 3 || !m_workers[cameraId] || !m_videoWidgets[cameraId]) return;
    // Prende solo il frame PIÙ RECENTE: se la GUI è rimasta indietro i frame intermedi
    // sono già stati scartati dal worker, quindi non c'è mai accumulo di ritardo.
    QImage frame = m_workers[cameraId]->takeLatestFrame();
    if (!frame.isNull()) {
        m_videoWidgets[cameraId]->setImage(frame);
    }
}

void MainWindow::onStateChanged(int cameraId, CameraState state) {
    if (cameraId >= 0 && cameraId < 3 && m_videoWidgets[cameraId]) {
        m_videoWidgets[cameraId]->setState(state);
    }
}

void MainWindow::onErrorOccurred(int cameraId, const QString& error) {
    if (cameraId >= 0 && cameraId < 3 && m_videoWidgets[cameraId]) {
        m_videoWidgets[cameraId]->setError(error);
    }
    qWarning() << "Camera" << cameraId << ":" << error;
}

void MainWindow::updateStatusBar() {
    const double elapsedSec = std::max<qint64>(1, m_fpsTimer.restart()) / 1000.0;

    for (int i = 0; i < 3; ++i) {
        if (!m_workers[i] || !m_statusLabels[i]) continue;
        const CameraStatus status = m_workers[i]->getStatus();
        const double fps = (status.displayedFrames - m_lastDisplayedCounts[i]) / elapsedSec;
        m_lastDisplayedCounts[i] = status.displayedFrames;

        // Avviso "segnale assente" se la camera risulta connessa ma non arrivano frame
        VideoWidget* w = m_videoWidgets[i].get();
        const qint64 since = w ? w->msSinceLastFrame() : -1;
        const bool stale = status.state == CameraState::Connected && since > STALE_FRAME_MS;
        if (w) w->setStale(stale);

        const QColor c = stale ? stateColor(CameraState::Error) : stateColor(status.state);
        m_statusLabels[i]->setText(QString("<span style='color:%1'>●</span> Cam %2  %3")
            .arg(c.name())
            .arg(i + 1)
            .arg(status.state == CameraState::Connected
                 ? QString("%1 fps").arg(fps, 0, 'f', 1)
                 : QString(stateText(status.state))));
    }

    const qint64 s = m_uptime.elapsed() / 1000;
    m_uptimeLabel->setText(QString("Attivo da %1g %2:%3:%4")
        .arg(s / 86400)
        .arg((s / 3600) % 24, 2, 10, QChar('0'))
        .arg((s / 60) % 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0')));
}

void MainWindow::onConfigChanged(int, const CameraConfig& config) {
    applyConfig(config);
}

// ============================================================================
// CameraConfigDialog
// ============================================================================

namespace {

const char* kDialogStyle = R"(
QDialog { background-color: #15171a; }
QWidget#page, QScrollArea, QScrollArea > QWidget > QWidget { background-color: #15171a; }
QLabel { color: #e6e8eb; background: transparent; }
QLabel#title { font-size: 17px; font-weight: 600; }
QLabel#subtitle, QLabel#hint { color: #9aa0a6; }
QLabel#validation { color: #f0616d; font-weight: 600; }
QTabWidget::pane { border: 1px solid #2c3036; border-radius: 6px; top: -1px; background: #15171a; }
QTabBar::tab { background: #1e2126; color: #9aa0a6; padding: 8px 18px; margin-right: 2px;
               border: 1px solid #2c3036; border-bottom: none;
               border-top-left-radius: 6px; border-top-right-radius: 6px; }
QTabBar::tab:selected { background: #15171a; color: #e6e8eb; font-weight: 600; }
QTabBar::tab:hover:!selected { color: #e6e8eb; }
QGroupBox { color: #e6e8eb; font-weight: 600; border: 1px solid #2c3036; border-radius: 6px;
            margin-top: 14px; padding: 14px 10px 8px 10px; background: #1b1e22; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: #9aa0a6; }
QLineEdit, QSpinBox, QComboBox { background: #0f1113; color: #e6e8eb; border: 1px solid #2c3036;
            border-radius: 4px; padding: 5px 8px; min-height: 20px;
            selection-background-color: #3d8bfd; selection-color: white; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #3d8bfd; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: #5f656b; }
QLineEdit[invalid="true"] { border-color: #f0616d; }
QSpinBox { padding-right: 22px; }
QSpinBox::up-button, QSpinBox::down-button { subcontrol-origin: border; width: 20px; border: none;
            border-left: 1px solid #2c3036; background: #1e2126; }
QSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: 4px; }
QSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 4px; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #2c3036; }
QSpinBox::up-arrow { image: url(:/arrow-up.png); width: 10px; height: 6px; }
QSpinBox::down-arrow { image: url(:/arrow-down.png); width: 10px; height: 6px; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: url(:/arrow-down.png); width: 10px; height: 6px; }
QComboBox QAbstractItemView { background: #1e2126; color: #e6e8eb; selection-background-color: #2c3036; }
QCheckBox { color: #e6e8eb; spacing: 8px; background: transparent; }
QCheckBox:disabled { color: #5f656b; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #5f656b; border-radius: 3px; background: #0f1113; }
QCheckBox::indicator:hover { border-color: #3d8bfd; }
QCheckBox::indicator:checked { background: #3d8bfd; border-color: #3d8bfd;
    image: url(:/check.png); }
QPushButton { background: #2c3036; color: #e6e8eb; border: 1px solid #3a3f46; border-radius: 4px;
              padding: 7px 16px; }
QPushButton:hover { background: #353a41; }
QPushButton:pressed { background: #24282d; }
QPushButton:disabled { color: #5f656b; }
QPushButton#primary { background: #3d8bfd; border-color: #3d8bfd; color: white; font-weight: 600; }
QPushButton#primary:hover { background: #5a9cfd; }
QScrollBar:vertical { background: #15171a; width: 10px; }
QScrollBar::handle:vertical { background: #2c3036; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)";

QLabel* makeHint(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setObjectName("hint");
    l->setWordWrap(true);
    return l;
}

QSpinBox* makeMsSpin(int min, int max, int step, QWidget* parent) {
    auto* s = new QSpinBox(parent);
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setSuffix(" ms");
    s->setMaximumWidth(140);
    return s;
}

// Form con colonna etichette di larghezza fissa: i campi di tutti i gruppi restano allineati
class AlignedForm {
public:
    explicit AlignedForm(QGroupBox* box) : m_box(box), m_form(new QFormLayout(box)) {
        m_form->setSpacing(8);
        m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    }
    void addRow(const QString& label, QWidget* field) {
        auto* l = new QLabel(label, m_box);
        l->setFixedWidth(165);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_form->addRow(l, field);
    }
private:
    QGroupBox* m_box;
    QFormLayout* m_form;
};

} // namespace

CameraConfigDialog::CameraConfigDialog(CameraConfigManager* configManager, QWidget* parent)
    : QDialog(parent)
    , m_configManager(configManager)
    , m_testCancel(std::make_shared<std::atomic<bool>>(false)) {
    setWindowTitle("Configurazione Telecamere");
    setModal(true);
    // Deve entrare anche in uno schermo 1366x768: il contenuto scorre
    setMinimumSize(560, 420);
    resize(720, 640);

    setupUI();
    loadConfig();
}

CameraConfigDialog::~CameraConfigDialog() {
    // I test in corso terminano entro pochi ms (interrupt callback FFmpeg)
    m_testCancel->store(true);
}

void CameraConfigDialog::setupUI() {
    setStyleSheet(kDialogStyle);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(16, 16, 16, 16);

    auto* title = new QLabel("Configurazione telecamere", this);
    title->setObjectName("title");
    auto* subtitle = new QLabel("Salvando, si riconnettono solo le telecamere modificate. "
                                "Le altre continuano a trasmettere senza interruzioni.", this);
    subtitle->setObjectName("subtitle");
    subtitle->setWordWrap(true);
    mainLayout->addWidget(title);
    mainLayout->addWidget(subtitle);

    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setDocumentMode(false);
    for (int i = 0; i < 3; ++i) {
        auto* scroll = new QScrollArea(m_tabWidget);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(buildCameraPage(i));
        m_tabWidget->addTab(scroll, QString("Camera %1").arg(i + 1));
    }
    mainLayout->addWidget(m_tabWidget, 1);

    m_buttonBox = new QDialogButtonBox(this);
    QPushButton* saveBtn = m_buttonBox->addButton("Salva e applica", QDialogButtonBox::AcceptRole);
    saveBtn->setObjectName("primary");
    saveBtn->setDefault(true);
    m_buttonBox->addButton("Annulla", QDialogButtonBox::RejectRole);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &CameraConfigDialog::onSave);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(m_buttonBox);
}

QWidget* CameraConfigDialog::buildCameraPage(int i) {
    CameraTab& tab = m_cameraTabs[i];
    tab.page = new QWidget();
    tab.page->setObjectName("page");
    auto* layout = new QVBoxLayout(tab.page);
    layout->setContentsMargins(12, 4, 12, 12);
    layout->setSpacing(4);

    // --- Generale ---
    auto* generalBox = new QGroupBox("Generale", tab.page);
    AlignedForm generalForm(generalBox);
    tab.nameEdit = new QLineEdit(generalBox);
    tab.nameEdit->setPlaceholderText(QString("Camera %1").arg(i + 1));
    generalForm.addRow("Nome:", tab.nameEdit);
    connect(tab.nameEdit, &QLineEdit::textChanged, this, [this, i](const QString& t) {
        m_tabWidget->setTabText(i, t.trimmed().isEmpty() ? QString("Camera %1").arg(i + 1) : t.trimmed());
    });
    layout->addWidget(generalBox);

    // --- Stream ---
    auto* streamBox = new QGroupBox("Stream RTSP", tab.page);
    AlignedForm streamForm(streamBox);
    tab.rtspUrlEdit = new QLineEdit(streamBox);
    tab.rtspUrlEdit->setPlaceholderText("rtsp://192.168.1.xxx:554/stream1");
    streamForm.addRow("Stream principale:", tab.rtspUrlEdit);

    tab.subStreamUrlEdit = new QLineEdit(streamBox);
    tab.subStreamUrlEdit->setPlaceholderText("rtsp://192.168.1.xxx:554/stream2  (opzionale)");
    streamForm.addRow("Sub-stream:", tab.subStreamUrlEdit);

    tab.streamTypeCombo = new QComboBox(streamBox);
    tab.streamTypeCombo->addItem("Stream principale (alta risoluzione)");
    tab.streamTypeCombo->addItem("Sub-stream (consigliato su PC poco potenti)");
    streamForm.addRow("Da visualizzare:", tab.streamTypeCombo);
    streamForm.addRow("", makeHint("Il sub-stream ha una risoluzione più bassa: riduce CPU e banda "
                                    "di 5-10 volte ed è più che sufficiente per una griglia di 3 camere.",
                                    streamBox));
    for (QLineEdit* e : {tab.rtspUrlEdit, tab.subStreamUrlEdit}) {
        connect(e, &QLineEdit::textEdited, this, [this, e]() { markInvalid(e, false); });
    }
    layout->addWidget(streamBox);

    // --- Credenziali ---
    auto* credBox = new QGroupBox("Credenziali", tab.page);
    AlignedForm credForm(credBox);
    tab.usernameEdit = new QLineEdit(credBox);
    tab.usernameEdit->setPlaceholderText("admin");
    credForm.addRow("Utente:", tab.usernameEdit);

    auto* pwRow = new QWidget(credBox);
    auto* pwLayout = new QHBoxLayout(pwRow);
    pwLayout->setContentsMargins(0, 0, 0, 0);
    tab.passwordEdit = new QLineEdit(pwRow);
    tab.passwordEdit->setEchoMode(QLineEdit::Password);
    tab.showPasswordCheck = new QCheckBox("Mostra", pwRow);
    pwLayout->addWidget(tab.passwordEdit, 1);
    pwLayout->addWidget(tab.showPasswordCheck);
    connect(tab.showPasswordCheck, &QCheckBox::toggled, tab.passwordEdit, [&tab](bool on) {
        tab.passwordEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    credForm.addRow("Password:", pwRow);
    credForm.addRow("", makeHint("Lascia vuoto se utente e password sono già nell'URL "
                                  "(rtsp://utente:password@indirizzo...).", credBox));
    layout->addWidget(credBox);

    // --- Prestazioni ---
    auto* perfBox = new QGroupBox("Prestazioni", tab.page);
    AlignedForm perfForm(perfBox);
    tab.fpsSpin = new QSpinBox(perfBox);
    tab.fpsSpin->setRange(1, 60);
    tab.fpsSpin->setSuffix(" fps");
    tab.fpsSpin->setMaximumWidth(140);
    perfForm.addRow("FPS massimi a schermo:", tab.fpsSpin);
    perfForm.addRow("", makeHint("Su PC poco potenti 10-15 fps sono sufficienti e dimezzano il carico.", perfBox));

    tab.lowCpuCheck = new QCheckBox("Decodifica leggera (salta il deblocking, meno CPU)", perfBox);
    perfForm.addRow("", tab.lowCpuCheck);

    tab.enableHwAccelCheck = new QCheckBox("Accelerazione hardware", perfBox);
    tab.hwAccelTypeCombo = new QComboBox(perfBox);
    tab.hwAccelTypeCombo->addItem("VAAPI (Intel / AMD su Linux)", "vaapi");
    tab.hwAccelTypeCombo->addItem("QuickSync (Intel)", "qsv");
    tab.hwAccelTypeCombo->addItem("NVDEC (NVIDIA)", "cuda");
    tab.hwAccelTypeCombo->setEnabled(false);
    connect(tab.enableHwAccelCheck, &QCheckBox::toggled, tab.hwAccelTypeCombo, &QComboBox::setEnabled);
    perfForm.addRow("", tab.enableHwAccelCheck);
    perfForm.addRow("Tipo:", tab.hwAccelTypeCombo);
    perfForm.addRow("", makeHint("Se l'accelerazione non è disponibile si torna automaticamente al software.", perfBox));
    layout->addWidget(perfBox);

    // --- Rete (avanzate) ---
    auto* netBox = new QGroupBox("Rete (avanzate)", tab.page);
    AlignedForm netForm(netBox);
    tab.enableTcpCheck = new QCheckBox("Usa TCP per RTSP (consigliato, più affidabile)", netBox);
    netForm.addRow("", tab.enableTcpCheck);
    tab.enableBufferingCheck = new QCheckBox("Buffer di rete ampio (tollera meglio il Wi-Fi)", netBox);
    netForm.addRow("", tab.enableBufferingCheck);
    tab.reconnectIntervalSpin = makeMsSpin(1000, 60000, 1000, netBox);
    netForm.addRow("Attesa tra riconnessioni:", tab.reconnectIntervalSpin);
    tab.connectionTimeoutSpin = makeMsSpin(2000, 60000, 1000, netBox);
    netForm.addRow("Timeout connessione:", tab.connectionTimeoutSpin);
    tab.receiveTimeoutSpin = makeMsSpin(2000, 60000, 1000, netBox);
    netForm.addRow("Timeout ricezione dati:", tab.receiveTimeoutSpin);
    layout->addWidget(netBox);

    // --- Test ---
    auto* testRow = new QHBoxLayout();
    testRow->setContentsMargins(0, 10, 0, 0);
    tab.testBtn = new QPushButton("Testa connessione", tab.page);
    connect(tab.testBtn, &QPushButton::clicked, this, [this, i]() { onTestConnection(i); });
    tab.testStatusLabel = new QLabel(tab.page);
    tab.testStatusLabel->setWordWrap(true);
    tab.testStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    testRow->addWidget(tab.testBtn);
    testRow->addWidget(tab.testStatusLabel, 1);
    layout->addLayout(testRow);

    tab.validationLabel = new QLabel(tab.page);
    tab.validationLabel->setObjectName("validation");
    tab.validationLabel->setWordWrap(true);
    tab.validationLabel->hide();
    layout->addWidget(tab.validationLabel);

    layout->addStretch();
    return tab.page;
}

void CameraConfigDialog::loadConfig() {
    auto configs = m_configManager->getAllConfigs();
    auto defaults = CameraConfigManager::createDefaults();

    for (int i = 0; i < 3; ++i) {
        CameraTab& tab = m_cameraTabs[i];
        const CameraConfig& c = configs[i].has_value() ? configs[i].value() : defaults[i];

        tab.nameEdit->setText(c.name);
        tab.rtspUrlEdit->setText(c.rtspUrl);
        tab.subStreamUrlEdit->setText(c.subStreamUrl);
        tab.streamTypeCombo->setCurrentIndex(c.useSubStream ? 1 : 0);
        tab.usernameEdit->setText(c.username);
        tab.passwordEdit->setText(c.password);
        tab.fpsSpin->setValue(c.fps);
        tab.lowCpuCheck->setChecked(c.lowCpuDecode);
        tab.enableHwAccelCheck->setChecked(c.enableHwAccel);
        tab.hwAccelTypeCombo->setEnabled(c.enableHwAccel);
        int hwIndex = tab.hwAccelTypeCombo->findData(c.hwAccelType.toLower());
        tab.hwAccelTypeCombo->setCurrentIndex(hwIndex >= 0 ? hwIndex : 0);
        tab.enableTcpCheck->setChecked(c.enableTcp);
        tab.enableBufferingCheck->setChecked(c.enableBuffering);
        tab.reconnectIntervalSpin->setValue(c.reconnectIntervalMs);
        tab.connectionTimeoutSpin->setValue(c.connectionTimeoutMs);
        tab.receiveTimeoutSpin->setValue(c.receiveTimeoutMs);
    }
}

CameraConfig CameraConfigDialog::configFromTab(int i) const {
    const CameraTab& tab = m_cameraTabs[i];
    // Parte dalla config esistente: i campi non presenti nel dialog (es. width/height) restano invariati
    CameraConfig c = m_configManager->getConfig(i).value_or(CameraConfigManager::createDefaults()[i]);
    c.id = i;
    c.name = tab.nameEdit->text().trimmed();
    if (c.name.isEmpty()) c.name = QString("Camera %1").arg(i + 1);
    c.rtspUrl = tab.rtspUrlEdit->text().trimmed();
    c.subStreamUrl = tab.subStreamUrlEdit->text().trimmed();
    c.useSubStream = tab.streamTypeCombo->currentIndex() == 1;
    c.username = tab.usernameEdit->text().trimmed();
    c.password = tab.passwordEdit->text();
    c.fps = tab.fpsSpin->value();
    c.lowCpuDecode = tab.lowCpuCheck->isChecked();
    c.enableHwAccel = tab.enableHwAccelCheck->isChecked();
    c.hwAccelType = tab.hwAccelTypeCombo->currentData().toString();
    c.enableTcp = tab.enableTcpCheck->isChecked();
    c.enableBuffering = tab.enableBufferingCheck->isChecked();
    c.reconnectIntervalMs = tab.reconnectIntervalSpin->value();
    c.connectionTimeoutMs = tab.connectionTimeoutSpin->value();
    c.receiveTimeoutMs = tab.receiveTimeoutSpin->value();
    return c;
}

void CameraConfigDialog::markInvalid(QWidget* w, bool invalid) {
    w->setProperty("invalid", invalid);
    w->style()->unpolish(w);
    w->style()->polish(w);
}

bool CameraConfigDialog::validateTab(int i) {
    CameraTab& tab = m_cameraTabs[i];
    auto isRtsp = [](const QString& u) {
        return u.startsWith("rtsp://", Qt::CaseInsensitive) || u.startsWith("rtsps://", Qt::CaseInsensitive);
    };

    QString error;
    QLineEdit* bad = nullptr;
    const QString mainUrl = tab.rtspUrlEdit->text().trimmed();
    const QString subUrl = tab.subStreamUrlEdit->text().trimmed();

    if (mainUrl.isEmpty()) {
        error = "Inserisci l'URL dello stream principale.";
        bad = tab.rtspUrlEdit;
    } else if (!isRtsp(mainUrl)) {
        error = "L'URL dello stream principale deve iniziare con rtsp://";
        bad = tab.rtspUrlEdit;
    } else if (tab.streamTypeCombo->currentIndex() == 1 && subUrl.isEmpty()) {
        error = "Hai scelto di visualizzare il sub-stream ma il suo URL è vuoto.";
        bad = tab.subStreamUrlEdit;
    } else if (!subUrl.isEmpty() && !isRtsp(subUrl)) {
        error = "L'URL del sub-stream deve iniziare con rtsp://";
        bad = tab.subStreamUrlEdit;
    }

    markInvalid(tab.rtspUrlEdit, bad == tab.rtspUrlEdit);
    markInvalid(tab.subStreamUrlEdit, bad == tab.subStreamUrlEdit);
    tab.validationLabel->setText(error);
    tab.validationLabel->setVisible(!error.isEmpty());

    if (bad) {
        m_tabWidget->setCurrentIndex(i);
        bad->setFocus();
        return false;
    }
    return true;
}

void CameraConfigDialog::onSave() {
    for (int i = 0; i < 3; ++i) {
        if (!validateTab(i)) return;
    }
    for (int i = 0; i < 3; ++i) {
        m_configManager->updateConfig(configFromTab(i));  // emette configChanged solo se cambiata
    }
    if (!m_configManager->saveToFile(m_configManager->configPath())) {
        QMessageBox::critical(this, "Errore",
            "Le modifiche sono state applicate ma non è stato possibile salvare il file:\n"
            + m_configManager->configPath());
        return;
    }
    emit configSaved();
    accept();
}

void CameraConfigDialog::onTestConnection(int cameraId) {
    if (!validateTab(cameraId)) return;

    CameraTab& tab = m_cameraTabs[cameraId];
    tab.testBtn->setEnabled(false);
    tab.testBtn->setText("Test in corso...");
    tab.testStatusLabel->setText("<span style='color:#f0c850'>Connessione in corso...</span>");

    const CameraConfig cfg = configFromTab(cameraId);
    auto cancel = m_testCancel;
    QPointer<CameraConfigDialog> self(this);

    // Il test gira in un thread separato: prima bloccava la GUI (e quindi il video
    // di TUTTE le camere) fino al timeout, che con FFmpeg 6 poteva non scattare mai.
    QThread* thread = QThread::create([cfg, cancel, self, cameraId]() {
        const ProbeResult result = StreamWorker::probe(cfg, cancel.get());
        QMetaObject::invokeMethod(qApp, [self, cameraId, result]() {
            if (self) self->onTestFinished(cameraId, result);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start(QThread::LowPriority);
}

void CameraConfigDialog::onTestFinished(int cameraId, const ProbeResult& result) {
    CameraTab& tab = m_cameraTabs[cameraId];
    tab.testBtn->setEnabled(true);
    tab.testBtn->setText("Testa connessione");
    if (result.ok) {
        tab.testStatusLabel->setText(QString("<span style='color:#4cc38a; font-weight:600'>✓ Connessa</span>"
                                             " <span style='color:#9aa0a6'>— %1</span>").arg(result.message.toHtmlEscaped()));
    } else {
        tab.testStatusLabel->setText(QString("<span style='color:#f0616d; font-weight:600'>✗ Errore:</span> %1")
                                     .arg(result.message.toHtmlEscaped()));
    }
}
