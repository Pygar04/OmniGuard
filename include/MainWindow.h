#ifndef OMNIGUARD_MAIN_WINDOW_H
#define OMNIGUARD_MAIN_WINDOW_H

#include "Common.h"
#include <QMainWindow>
#include <QLabel>
#include <QGridLayout>
#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QStatusBar>
#include <QMenuBar>
#include <QAction>
#include <QCloseEvent>
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QTabWidget>
#include <QPushButton>
#include <array>
#include <atomic>
#include <memory>

class StreamWorker;
class CameraConfigManager;
struct ProbeResult;

// ============================================================================
// VideoWidget - riquadro di una telecamera
// Riceve frame già ridimensionati dal thread della camera: il paint è un semplice blit.
// ============================================================================
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(int cameraId, const QString& name, QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void setState(CameraState state);
    void setError(const QString& error);
    void setName(const QString& name);
    void setStale(bool stale);

    int cameraId() const { return m_cameraId; }
    qint64 msSinceLastFrame() const;

signals:
    // Dimensione in pixel fisici in cui il video verrà disegnato
    void targetSizeChanged(const QSize& size);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    int m_cameraId;
    QString m_name;
    QImage m_currentFrame;
    CameraState m_state = CameraState::Disconnected;
    QString m_errorText;
    bool m_stale = false;
    QElapsedTimer m_lastFrameTimer;
    QString m_lastFrameClock;
    QFont m_titleFont;
    QFont m_smallFont;
};

// ============================================================================
// CameraConfigDialog - configurazione telecamere
// ============================================================================
class CameraConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit CameraConfigDialog(CameraConfigManager* configManager, QWidget* parent = nullptr);
    ~CameraConfigDialog() override;

    CameraConfigDialog(const CameraConfigDialog&) = delete;
    CameraConfigDialog& operator=(const CameraConfigDialog&) = delete;

signals:
    void configSaved();

private slots:
    void onSave();
    void onTestConnection(int cameraId);

private:
    struct CameraTab {
        QWidget* page = nullptr;
        QLineEdit* nameEdit = nullptr;
        QLineEdit* rtspUrlEdit = nullptr;
        QLineEdit* subStreamUrlEdit = nullptr;
        QComboBox* streamTypeCombo = nullptr;
        QLineEdit* usernameEdit = nullptr;
        QLineEdit* passwordEdit = nullptr;
        QCheckBox* showPasswordCheck = nullptr;
        QSpinBox* fpsSpin = nullptr;
        QCheckBox* lowCpuCheck = nullptr;
        QCheckBox* enableHwAccelCheck = nullptr;
        QComboBox* hwAccelTypeCombo = nullptr;
        QCheckBox* enableTcpCheck = nullptr;
        QCheckBox* enableBufferingCheck = nullptr;
        QSpinBox* reconnectIntervalSpin = nullptr;
        QSpinBox* connectionTimeoutSpin = nullptr;
        QSpinBox* receiveTimeoutSpin = nullptr;
        QPushButton* testBtn = nullptr;
        QLabel* testStatusLabel = nullptr;
        QLabel* validationLabel = nullptr;
    };

    void setupUI();
    QWidget* buildCameraPage(int i);
    void loadConfig();
    CameraConfig configFromTab(int i) const;
    bool validateTab(int i);
    void markInvalid(QWidget* w, bool invalid);
    void onTestFinished(int cameraId, const ProbeResult& result);

    CameraConfigManager* m_configManager = nullptr;
    std::array<CameraTab, 3> m_cameraTabs;
    QTabWidget* m_tabWidget = nullptr;
    QDialogButtonBox* m_buttonBox = nullptr;

    // Impostato alla chiusura del dialog: interrompe subito eventuali test in corso
    std::shared_ptr<std::atomic<bool>> m_testCancel;
};

// ============================================================================
// MainWindow
// ============================================================================
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(CameraConfigManager* configManager, QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onFrameAvailable(int cameraId);
    void onStateChanged(int cameraId, CameraState state);
    void onErrorOccurred(int cameraId, const QString& error);
    void updateStatusBar();
    void onConfigChanged(int cameraId, const CameraConfig& config);
    void openCameraConfig();

private:
    void setupUI();
    void setupMenuBar();
    void setupStatusBar();
    void createCameraWidgets();
    void startWorkers();
    void stopWorkers();
    void applyConfig(const CameraConfig& config);

    CameraConfigManager* m_configManager = nullptr;

    QWidget* m_centralWidget = nullptr;
    QGridLayout* m_gridLayout = nullptr;

    std::array<std::unique_ptr<VideoWidget>, 3> m_videoWidgets;
    std::array<std::unique_ptr<StreamWorker>, 3> m_workers;

    QStatusBar* m_statusBar = nullptr;
    QLabel* m_statusLabels[3] = {nullptr, nullptr, nullptr};
    QLabel* m_uptimeLabel = nullptr;
    QTimer* m_statusTimer = nullptr;
    QElapsedTimer m_uptime;
    QElapsedTimer m_fpsTimer;

    std::array<uint64_t, 3> m_lastDisplayedCounts{0, 0, 0};
    bool m_workersStopped = false;
};

#endif // OMNIGUARD_MAIN_WINDOW_H
