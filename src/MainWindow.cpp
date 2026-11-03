#include "MainWindow.h"
#include "StreamWorker.h"
#include "CameraConfig.h"
#include <QPainter>
#include <QMouseEvent>
#include <QApplication>
#include <QScreen>
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>
#include <QDebug>
#include <QProcess>
#include <QUrl>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

VideoWidget::VideoWidget(int cameraId, const QString& name, QWidget* parent)
    : QWidget(parent)
    , m_cameraId(cameraId)
    , m_name(name) {
    
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
}

void VideoWidget::setImage(const QImage& image) {
    if (image.isNull()) return;
    
    m_currentFrame = image;
    m_needsRescale = true;
    update();
}

void VideoWidget::setState(CameraState state) {
    m_state = state;
    m_needsRescale = true;
    update();
}

void VideoWidget::setError(const QString& error) {
    m_errorText = error;
    update();
}

void VideoWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    
    painter.fillRect(rect(), Qt::black);
    
    if (!m_currentFrame.isNull()) {
        if (m_needsRescale || m_scaledPixmap.isNull() || m_scaledPixmap.size() != size()) {
            m_scaledPixmap = QPixmap::fromImage(m_currentFrame).scaled(
                size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_needsRescale = false;
        }
        
        if (!m_scaledPixmap.isNull()) {
            QPoint center = rect().center() - m_scaledPixmap.rect().center();
            painter.drawPixmap(center, m_scaledPixmap);
        }
    }
    
    QColor borderColor;
    QString statusText;
    
    switch (m_state) {
        case CameraState::Disconnected:
            borderColor = Qt::red;
            statusText = "Disconnessa";
            break;
        case CameraState::Connecting:
            borderColor = Qt::yellow;
            statusText = "Connessione...";
            break;
        case CameraState::Connected:
            borderColor = Qt::green;
            statusText = "Connessa";
            break;
        case CameraState::Reconnecting:
            borderColor = QColor(255, 165, 0);  // orange
            statusText = "Riconnessione...";
            break;
        case CameraState::Error:
            borderColor = Qt::red;
            statusText = "Errore";
            break;
    }
    
    QPen pen(borderColor, 3);
    painter.setPen(pen);
    painter.drawRect(rect().adjusted(1, 1, -2, -2));
    
    painter.setPen(Qt::white);
    painter.setFont(QFont("Arial", 12, QFont::Bold));
    painter.drawText(10, 25, m_name);
    
    painter.setFont(QFont("Arial", 10));
    QColor statusColor = borderColor;
    if (m_state == CameraState::Connected) statusColor = Qt::lightGray;
    painter.setPen(statusColor);
    painter.drawText(10, 45, statusText);
    
    if (!m_errorText.isEmpty() && m_state != CameraState::Connected) {
        painter.setPen(Qt::red);
        painter.setFont(QFont("Arial", 9));
        QRect errorRect = rect().adjusted(10, 65, -10, -10);
        painter.drawText(errorRect, Qt::AlignLeft | Qt::TextWordWrap, m_errorText);
    }
    
    if (m_state == CameraState::Connected) {
        painter.setPen(Qt::green);
        painter.setFont(QFont("Arial", 9));
        painter.drawText(rect().adjusted(10, 10, -10, -10), Qt::AlignRight | Qt::AlignTop, "LIVE");
    }
}

void VideoWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_needsRescale = true;
}

MainWindow::MainWindow(CameraConfigManager* configManager, QWidget* parent)
    : QMainWindow(parent)
    , m_configManager(configManager) {
    
    setupUI();
    setupMenuBar();
    setupStatusBar();
    createCameraWidgets();
    
    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    restoreState(settings.value("window/state").toByteArray());
    
    QTimer::singleShot(100, this, &MainWindow::startWorkers);
}

MainWindow::~MainWindow() {
    stopWorkers();
    
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    stopWorkers();
    event->accept();
}

void MainWindow::setupUI() {
    setWindowTitle("OmniGuard - Videosorveglianza Zosi (3 Camere)");
    resize(1280, 720);
    setMinimumSize(960, 540);
    
    // Icona finestra (per barra titolo Linux)
    setWindowIcon(QIcon(":/omniguard.svg"));
    
    // Sfondo nero per tutta la finestra
    setStyleSheet("QMainWindow { background-color: black; }"
                  "QWidget { background-color: black; }"
                  "QStatusBar { background-color: #1a1a1a; color: white; }"
                  "QLabel { color: white; }"
                  "QMenuBar { background-color: #1a1a1a; color: white; }"
                  "QMenuBar::item:selected { background-color: #333; }"
                  "QMenu { background-color: #1a1a1a; color: white; }"
                  "QMenu::item:selected { background-color: #333; }");
    
    m_centralWidget = new QWidget(this);
    m_centralWidget->setStyleSheet("background-color: black;");
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
        if (isFullScreen()) {
            showNormal();
        } else {
            showFullScreen();
        }
    });
    fullscreenAction->setShortcut(Qt::Key_F11);
    
    QMenu* helpMenu = menuBar->addMenu("&Aiuto");
    helpMenu->addAction("&Informazioni", this, []() {
        QMessageBox::about(nullptr, "OmniGuard", 
            "OmniGuard v1.0\n"
            "Sistema di videosorveglianza per 3 telecamere IP Zosi\n"
            "Basato su Qt 6 e FFmpeg\n\n"
            "Architettura multithread per stabilita assoluta");
    });
}

void MainWindow::openCameraConfig() {
    CameraConfigDialog dialog(m_configManager, this);
    if (dialog.exec() == QDialog::Accepted) {
        // Config saved, apply changes
        auto configs = m_configManager->getAllConfigs();
        for (int i = 0; i < 3; ++i) {
            if (configs[i].has_value() && m_workers[i]) {
                m_workers[i]->requestReconnect();
            }
        }
    }
}

void MainWindow::setupStatusBar() {
    m_statusBar = statusBar();
    
    for (int i = 0; i < 3; ++i) {
        m_statusLabels[i] = new QLabel(QString("Cam %1: Inizializzazione...").arg(i + 1));
        m_statusLabels[i]->setMinimumWidth(180);
        m_statusBar->addWidget(m_statusLabels[i]);
    }
    
    m_statusBar->addPermanentWidget(new QLabel(" | "));
    
    m_fpsLabel = new QLabel("FPS: --");
    m_fpsLabel->setMinimumWidth(120);
    m_statusBar->addPermanentWidget(m_fpsLabel);
    
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(1000);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    m_statusTimer->start();
}

void MainWindow::createCameraWidgets() {
    auto configs = m_configManager->getAllConfigs();
    
    for (int i = 0; i < 3; ++i) {
        QString name = QString("Camera %1").arg(i + 1);
        if (configs[i].has_value()) {
            name = configs[i]->name;
        }
        
        m_videoWidgets[i] = std::make_unique<VideoWidget>(i, name);
        
        int row = i / 2;
        int col = i % 2;
        m_gridLayout->addWidget(m_videoWidgets[i].get(), row, col);
    }
    
    m_gridLayout->setRowStretch(0, 1);
    m_gridLayout->setRowStretch(1, 1);
    m_gridLayout->setColumnStretch(0, 1);
    m_gridLayout->setColumnStretch(1, 1);
}

void MainWindow::startWorkers() {
    auto configs = m_configManager->getAllConfigs();
    
    for (int i = 0; i < 3; ++i) {
        CameraConfig config;
        if (configs[i].has_value()) {
            config = configs[i].value();
        } else {
            config = CameraConfig(i, QString("Camera %1").arg(i + 1), 
                QString("rtsp://192.168.1.%1:554/stream1").arg(100 + i));
        }
        
        m_workers[i] = std::make_unique<StreamWorker>(config);
        
        connect(m_workers[i].get(), &StreamWorker::frameReady,
                this, &MainWindow::onFrameReady, Qt::QueuedConnection);
        
        connect(m_workers[i].get(), &StreamWorker::stateChanged,
                this, [this, i](CameraState state) {
                    onStateChanged(i, state);
                }, Qt::QueuedConnection);
        
        connect(m_workers[i].get(), &StreamWorker::errorOccurred,
                this, [this, i](const QString& error) {
                    onErrorOccurred(i, error);
                }, Qt::QueuedConnection);
        
        connect(m_workers[i].get(), &StreamWorker::connected,
                this, [this, i]() {
                    onConnected(i);
                }, Qt::QueuedConnection);
        
        connect(m_workers[i].get(), &StreamWorker::disconnected,
                this, [this, i](const QString& reason) {
                    onDisconnected(i, reason);
                }, Qt::QueuedConnection);
        
        m_workers[i]->start();
    }
    
    connect(m_configManager, &CameraConfigManager::configChanged,
            this, &MainWindow::onConfigChanged);
}

void MainWindow::stopWorkers() {
    for (int i = 0; i < 3; ++i) {
        if (m_workers[i]) {
            m_workers[i]->stop();
        }
    }
    
    for (int i = 0; i < 3; ++i) {
        if (m_workers[i] && m_workers[i]->isRunning()) {
            QThread* thread = m_workers[i]->thread();
            if (thread) {
                thread->wait(3000);
            }
        }
    }
    
    for (int i = 0; i < 3; ++i) {
        m_workers[i].reset();
    }
}

void MainWindow::applyConfig(const CameraConfig& config) {
    if (config.id >= 0 && config.id < 3 && m_workers[config.id]) {
        m_workers[config.id]->requestReconnect();
    }
}

void MainWindow::onFrameReady(FrameData frame) {
    if (!frame.isValid()) return;
    
    int id = frame.cameraId;
    if (id >= 0 && id < 3 && m_videoWidgets[id]) {
        m_videoWidgets[id]->setImage(frame.image);
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
    qWarning() << "Camera" << cameraId << "error:" << error;
}

void MainWindow::onConnected(int cameraId) {
    if (cameraId >= 0 && cameraId < 3 && m_statusLabels[cameraId]) {
        m_statusLabels[cameraId]->setText(QString("Cam %1: Connessa").arg(cameraId + 1));
        m_statusLabels[cameraId]->setStyleSheet("color: green;");
    }
}

void MainWindow::onDisconnected(int cameraId, const QString& reason) {
    if (cameraId >= 0 && cameraId < 3 && m_statusLabels[cameraId]) {
        m_statusLabels[cameraId]->setText(QString("Cam %1: %2").arg(cameraId + 1).arg(reason));
        m_statusLabels[cameraId]->setStyleSheet("color: orange;");
    }
}

void MainWindow::updateStatusBar() {
    QString fpsText = "FPS: ";
    for (int i = 0; i < 3; ++i) {
        if (m_workers[i]) {
            CameraStatus status = m_workers[i]->getStatus();
            uint64_t currentFrames = status.totalFrames;
            double fps = (currentFrames - m_lastFrameCounts[i]) / 1.0;
            m_currentFps[i] = fps;
            m_lastFrameCounts[i] = currentFrames;
            
            if (i > 0) fpsText += " | ";
            fpsText += QString("C%1: %2").arg(i + 1).arg(fps, 0, 'f', 1);
        }
    }
    m_fpsLabel->setText(fpsText);
}

void MainWindow::onConfigChanged(int cameraId, const CameraConfig& config) {
    applyConfig(config);
}

void MainWindow::onConfigDialogSaved() {
    // Config saved signal from dialog - workers will be restarted via configChanged signal
    qInfo() << "Configuration saved, cameras will reconnect with new settings";
}

// ============================================================================
// CameraConfigDialog Implementation
// ============================================================================

CameraConfigDialog::CameraConfigDialog(CameraConfigManager* configManager, QWidget* parent)
    : QDialog(parent)
    , m_configManager(configManager)
{
    setWindowTitle("Configurazione Telecamere");
    setMinimumSize(700, 900);
    setModal(true);
    
    setupUI();
    loadConfig();
}

void CameraConfigDialog::setupUI() {
    // Stile CSS pulito e curato nei colori per il menu di configurazione
    setStyleSheet(
        "QDialog { background-color: #f8f9fa; }"
        "QTabWidget::pane { border: 1px solid #dee2e6; background-color: white; border-radius: 4px; }"
        "QTabBar::tab { background-color: #e9ecef; color: #495057; padding: 8px 16px; "
        "margin-right: 2px; border-top-left-radius: 4px; border-top-right-radius: 4px; border: 1px solid #dee2e6; border-bottom: none; }"
        "QTabBar::tab:selected { background-color: white; color: #212529; font-weight: bold; border-bottom: 1px solid white; }"
        "QTabBar::tab:hover { background-color: #dee2e6; }"
        "QGroupBox { font-weight: bold; color: #495057; border: 1px solid #dee2e6; "
        "border-radius: 4px; margin-top: 12px; padding-top: 10px; background-color: white; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; color: #495057; background-color: white; }"
        "QLabel { color: #495057; }"
        "QLineEdit, QSpinBox, QComboBox { background-color: white; border: 1px solid #ced4da; "
        "border-radius: 4px; padding: 4px 8px; color: #212529; selection-background-color: #0d6efd; selection-color: white; }"
        "QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #86b7fe; outline: none; }"
        "QPushButton { background-color: #e9ecef; color: #212529; border: 1px solid #ced4da; "
        "border-radius: 4px; padding: 6px 12px; font-weight: 500; }"
        "QPushButton:hover { background-color: #dee2e6; border-color: #adb5bd; }"
        "QPushButton:pressed { background-color: #ced4da; }"
        "QDialogButtonBox QPushButton { min-width: 100px; padding: 8px 16px; }"
    );

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(15, 15, 15, 15);

    // Title label
    QLabel* titleLabel = new QLabel("Configurazione Telecamere IP", this);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("font-size: 18px; font-weight: 600; color: #212529; margin-bottom: 10px; padding: 10px; background-color: white; border-radius: 6px; border: 1px solid #dee2e6;");
    mainLayout->addWidget(titleLabel);

    // Tab widget for cameras
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabPosition(QTabWidget::North);
    
    for (int i = 0; i < 3; ++i) {
        CameraTab& tab = m_cameraTabs[i];
        tab.widget = new QWidget();
        QFormLayout* formLayout = new QFormLayout(tab.widget);
        formLayout->setSpacing(10);
        formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        formLayout->setFormAlignment(Qt::AlignHCenter | Qt::AlignTop);
        formLayout->setContentsMargins(20, 20, 20, 20);

        // Camera Name
        tab.nameEdit = new QLineEdit(tab.widget);
        tab.nameEdit->setPlaceholderText(QString("Camera %1").arg(i + 1));
        tab.nameEdit->setMinimumWidth(400);
        formLayout->addRow("Nome:", tab.nameEdit);

        // RTSP URL (Main Stream)
        tab.rtspUrlEdit = new QLineEdit(tab.widget);
        tab.rtspUrlEdit->setPlaceholderText("rtsp://192.168.1.xxx:554/stream1");
        tab.rtspUrlEdit->setMinimumWidth(400);
        formLayout->addRow("URL Stream Principale:", tab.rtspUrlEdit);

        // Sub-stream URL
        tab.subStreamUrlEdit = new QLineEdit(tab.widget);
        tab.subStreamUrlEdit->setPlaceholderText("rtsp://192.168.1.xxx:554/stream2 (opzionale)");
        tab.subStreamUrlEdit->setMinimumWidth(400);
        formLayout->addRow("URL Sub-Stream:", tab.subStreamUrlEdit);

        // Username
        tab.usernameEdit = new QLineEdit(tab.widget);
        tab.usernameEdit->setPlaceholderText("admin");
        tab.usernameEdit->setMinimumWidth(400);
        formLayout->addRow("Username:", tab.usernameEdit);

        // Password
        tab.passwordEdit = new QLineEdit(tab.widget);
        tab.passwordEdit->setEchoMode(QLineEdit::Password);
        tab.passwordEdit->setPlaceholderText("password");
        tab.passwordEdit->setMinimumWidth(400);
        formLayout->addRow("Password:", tab.passwordEdit);

        // Use Sub-stream checkbox
        tab.useSubStreamCheck = new QCheckBox("Usa sub-stream (bassa risoluzione) per monitoraggio", tab.widget);
        formLayout->addRow("", tab.useSubStreamCheck);
        connect(tab.useSubStreamCheck, &QCheckBox::checkStateChanged, 
                this, [this, i](Qt::CheckState state) { onUseSubStreamChanged(i, state == Qt::Checked); });

        // Sub-stream URL row (initially hidden)
        tab.subStreamUrlEdit->setVisible(false);
        QWidget* subStreamLabel = formLayout->labelForField(tab.subStreamUrlEdit);
        if (subStreamLabel) subStreamLabel->setVisible(false);

        // Group: Connection Settings
        QGroupBox* connGroup = new QGroupBox("Impostazioni Connessione", tab.widget);
        QFormLayout* connLayout = new QFormLayout(connGroup);
        connLayout->setSpacing(8);
        
        tab.reconnectIntervalSpin = new QSpinBox(tab.widget);
        tab.reconnectIntervalSpin->setRange(1000, 60000);
        tab.reconnectIntervalSpin->setSingleStep(1000);
        tab.reconnectIntervalSpin->setSuffix(" ms");
        tab.reconnectIntervalSpin->setValue(3000);
        connLayout->addRow("Intervallo Riconnessione:", tab.reconnectIntervalSpin);

        tab.connectionTimeoutSpin = new QSpinBox(tab.widget);
        tab.connectionTimeoutSpin->setRange(1000, 60000);
        tab.connectionTimeoutSpin->setSingleStep(1000);
        tab.connectionTimeoutSpin->setSuffix(" ms");
        tab.connectionTimeoutSpin->setValue(10000);
        connLayout->addRow("Timeout Connessione:", tab.connectionTimeoutSpin);

        tab.receiveTimeoutSpin = new QSpinBox(tab.widget);
        tab.receiveTimeoutSpin->setRange(1000, 60000);
        tab.receiveTimeoutSpin->setSingleStep(1000);
        tab.receiveTimeoutSpin->setSuffix(" ms");
        tab.receiveTimeoutSpin->setValue(5000);
        connLayout->addRow("Timeout Ricezione:", tab.receiveTimeoutSpin);

        tab.enableTcpCheck = new QCheckBox("Usa TCP per RTSP (più affidabile)", tab.widget);
        tab.enableTcpCheck->setChecked(true);
        connLayout->addRow("", tab.enableTcpCheck);

        tab.enableBufferingCheck = new QCheckBox("Abilita buffering di rete", tab.widget);
        tab.enableBufferingCheck->setChecked(true);
        connLayout->addRow("", tab.enableBufferingCheck);

        formLayout->addRow(connGroup);

        // Group: Video Settings
        QGroupBox* videoGroup = new QGroupBox("Impostazioni Video", tab.widget);
        QFormLayout* videoLayout = new QFormLayout(videoGroup);
        videoLayout->setSpacing(8);

        tab.widthSpin = new QSpinBox(tab.widget);
        tab.widthSpin->setRange(160, 3840);
        tab.widthSpin->setSingleStep(16);
        tab.widthSpin->setValue(1920);
        videoLayout->addRow("Larghezza:", tab.widthSpin);

        tab.heightSpin = new QSpinBox(tab.widget);
        tab.heightSpin->setRange(120, 2160);
        tab.heightSpin->setSingleStep(16);
        tab.heightSpin->setValue(1080);
        videoLayout->addRow("Altezza:", tab.heightSpin);

        tab.fpsSpin = new QSpinBox(tab.widget);
        tab.fpsSpin->setRange(1, 60);
        tab.fpsSpin->setValue(25);
        videoLayout->addRow("FPS:", tab.fpsSpin);

        formLayout->addRow(videoGroup);

        // Group: Hardware Acceleration
        QGroupBox* hwGroup = new QGroupBox("Accelerazione Hardware", tab.widget);
        QFormLayout* hwLayout = new QFormLayout(hwGroup);
        hwLayout->setSpacing(8);

        tab.enableHwAccelCheck = new QCheckBox("Abilita accelerazione hardware", tab.widget);
        hwLayout->addRow("", tab.enableHwAccelCheck);

        tab.hwAccelTypeCombo = new QComboBox(tab.widget);
        tab.hwAccelTypeCombo->addItems({"qsv (Intel QuickSync)", "cuda (NVIDIA)", "vaapi (AMD/Intel)", "auto"});
        tab.hwAccelTypeCombo->setEnabled(false);
        hwLayout->addRow("Tipo:", tab.hwAccelTypeCombo);
        
        connect(tab.enableHwAccelCheck, &QCheckBox::toggled, 
                tab.hwAccelTypeCombo, &QComboBox::setEnabled);

        formLayout->addRow(hwGroup);

        // Test Connection Button
        QHBoxLayout* testLayout = new QHBoxLayout();
        tab.testBtn = new QPushButton("Testa Connessione", tab.widget);
        tab.testBtn->setMinimumHeight(35);
        tab.testBtn->setStyleSheet(
            "QPushButton { background-color: #0d6efd; color: white; font-weight: 600; border-radius: 4px; padding: 8px 16px; }"
            "QPushButton:hover { background-color: #0b5ed7; }"
            "QPushButton:pressed { background-color: #0a58ca; }"
            "QPushButton:disabled { background-color: #6c757d; color: #adb5bd; }"
        );
        connect(tab.testBtn, &QPushButton::clicked, this, [this, i]() { onTestConnection(i); });
        testLayout->addStretch();
        testLayout->addWidget(tab.testBtn);
        testLayout->addStretch();
        formLayout->addRow(testLayout);

        // Test Status Label
        tab.testStatusLabel = new QLabel("", tab.widget);
        tab.testStatusLabel->setAlignment(Qt::AlignCenter);
        tab.testStatusLabel->setStyleSheet("color: #6c757d; font-style: italic;");
        tab.testStatusLabel->setWordWrap(true);
        formLayout->addRow("", tab.testStatusLabel);

        m_tabWidget->addTab(tab.widget, QString("Camera %1").arg(i + 1));
    }

    mainLayout->addWidget(m_tabWidget);

    // Button box
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    m_buttonBox->button(QDialogButtonBox::Save)->setText("Salva");
    m_buttonBox->button(QDialogButtonBox::Cancel)->setText("Annulla");
    m_buttonBox->button(QDialogButtonBox::Save)->setMinimumWidth(100);
    m_buttonBox->button(QDialogButtonBox::Cancel)->setMinimumWidth(100);
    
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &CameraConfigDialog::onSave);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &CameraConfigDialog::onCancel);
    
    mainLayout->addWidget(m_buttonBox);
}

void CameraConfigDialog::loadConfig() {
    auto configs = m_configManager->getAllConfigs();
    
    for (int i = 0; i < 3; ++i) {
        CameraTab& tab = m_cameraTabs[i];
        
        if (configs[i].has_value()) {
            const CameraConfig& config = configs[i].value();
            
            tab.nameEdit->setText(config.name);
            tab.rtspUrlEdit->setText(config.rtspUrl);
            tab.subStreamUrlEdit->setText(config.subStreamUrl);
            tab.usernameEdit->setText(config.username);
            tab.passwordEdit->setText(config.password);
            tab.useSubStreamCheck->setChecked(config.useSubStream);
            tab.reconnectIntervalSpin->setValue(config.reconnectIntervalMs);
            tab.connectionTimeoutSpin->setValue(config.connectionTimeoutMs);
            tab.receiveTimeoutSpin->setValue(config.receiveTimeoutMs);
            tab.enableTcpCheck->setChecked(config.enableTcp);
            tab.enableBufferingCheck->setChecked(config.enableBuffering);
            tab.widthSpin->setValue(config.width);
            tab.heightSpin->setValue(config.height);
            tab.fpsSpin->setValue(config.fps);
            tab.enableHwAccelCheck->setChecked(config.enableHwAccel);
            
            int hwIndex = tab.hwAccelTypeCombo->findText(config.hwAccelType);
            if (hwIndex >= 0) {
                tab.hwAccelTypeCombo->setCurrentIndex(hwIndex);
            }
            
            updateSubStreamFields(i, config.useSubStream);
        }
    }
}

void CameraConfigDialog::saveConfig() {
    for (int i = 0; i < 3; ++i) {
        CameraTab& tab = m_cameraTabs[i];
        
        CameraConfig config;
        config.id = i;
        config.name = tab.nameEdit->text().trimmed();
        config.rtspUrl = tab.rtspUrlEdit->text().trimmed();
        config.subStreamUrl = tab.subStreamUrlEdit->text().trimmed();
        config.username = tab.usernameEdit->text().trimmed();
        config.password = tab.passwordEdit->text();
        config.useSubStream = tab.useSubStreamCheck->isChecked();
        config.reconnectIntervalMs = tab.reconnectIntervalSpin->value();
        config.connectionTimeoutMs = tab.connectionTimeoutSpin->value();
        config.receiveTimeoutMs = tab.receiveTimeoutSpin->value();
        config.enableTcp = tab.enableTcpCheck->isChecked();
        config.enableBuffering = tab.enableBufferingCheck->isChecked();
        config.width = tab.widthSpin->value();
        config.height = tab.heightSpin->value();
        config.fps = tab.fpsSpin->value();
        config.enableHwAccel = tab.enableHwAccelCheck->isChecked();
        config.hwAccelType = tab.hwAccelTypeCombo->currentText().split(" ").first();
        
        if (config.name.isEmpty()) {
            config.name = QString("Camera %1").arg(i + 1);
        }
        if (config.rtspUrl.isEmpty()) {
            tab.testStatusLabel->setText("<span style='color: red;'>URL stream principale richiesto</span>");
            m_tabWidget->setCurrentIndex(i);
            return;
        }
        
        m_configManager->updateConfig(config);
    }
    
    if (m_configManager->saveToFile(m_configManager->defaultConfigPath())) {
        emit configSaved();
        accept();
    } else {
        QMessageBox::critical(this, "Errore", "Impossibile salvare il file di configurazione.");
    }
}

void CameraConfigDialog::onSave() {
    saveConfig();
}

void CameraConfigDialog::onCancel() {
    reject();
}

void CameraConfigDialog::onTestConnection(int cameraId) {
    CameraTab& tab = m_cameraTabs[cameraId];
    tab.testBtn->setEnabled(false);
    tab.testBtn->setText("Test in corso...");
    tab.testStatusLabel->setText("<span style='color: #f39c12;'>Test connessione in corso...</span>");
    QApplication::processEvents();
    
    CameraConfig config;
    config.id = cameraId;
    config.rtspUrl = tab.rtspUrlEdit->text().trimmed();
    config.subStreamUrl = tab.subStreamUrlEdit->text().trimmed();
    config.username = tab.usernameEdit->text().trimmed();
    config.password = tab.passwordEdit->text();
    config.useSubStream = tab.useSubStreamCheck->isChecked();
    config.enableTcp = tab.enableTcpCheck->isChecked();
    config.enableBuffering = tab.enableBufferingCheck->isChecked();
    config.connectionTimeoutMs = tab.connectionTimeoutSpin->value();
    config.receiveTimeoutMs = tab.receiveTimeoutSpin->value();
    
    QString testUrl = config.getActiveUrl();
    
    if (testUrl.isEmpty()) {
        tab.testStatusLabel->setText("<span style='color: red;'>URL non valido</span>");
        tab.testBtn->setEnabled(true);
        tab.testBtn->setText("Testa Connessione");
        return;
    }
    
    AVFormatContext* fmtCtx = nullptr;
    AVDictionary* options = nullptr;
    
    if (config.enableTcp) {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
    }
    if (config.enableBuffering) {
        av_dict_set(&options, "buffer_size", "1024000", 0);
    }
    av_dict_set(&options, "stimeout", QString::number(config.receiveTimeoutMs * 1000).toUtf8().constData(), 0);
    
    if (!config.username.isEmpty()) {
        QUrl url(testUrl);
        url.setUserName(config.username);
        url.setPassword(config.password);
        testUrl = url.toString();
    }
    
    int ret = avformat_open_input(&fmtCtx, testUrl.toUtf8().constData(), nullptr, &options);
    av_dict_free(&options);
    
    if (ret >= 0) {
        avformat_close_input(&fmtCtx);
        tab.testStatusLabel->setText("<span style='color: #27ae60; font-weight: bold;'>✓ Connessione riuscita!</span>");
    } else {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        tab.testStatusLabel->setText(QString("<span style='color: #e74c3c;'>✗ Errore: %1</span>").arg(QString::fromUtf8(errbuf)));
    }
    
    tab.testBtn->setEnabled(true);
    tab.testBtn->setText("Testa Connessione");
}

void CameraConfigDialog::onUseSubStreamChanged(int cameraId, bool enabled) {
    updateSubStreamFields(cameraId, enabled);
}

void CameraConfigDialog::updateSubStreamFields(int cameraId, bool enabled) {
    CameraTab& tab = m_cameraTabs[cameraId];
    tab.subStreamUrlEdit->setVisible(enabled);
    QFormLayout* formLayout = qobject_cast<QFormLayout*>(tab.widget->layout());
    if (formLayout) {
        QWidget* lbl = formLayout->labelForField(tab.subStreamUrlEdit);
        if (lbl) lbl->setVisible(enabled);
    }
}
