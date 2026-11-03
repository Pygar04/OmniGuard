#ifndef OMNIGUARD_MAIN_WINDOW_H
#define OMNIGUARD_MAIN_WINDOW_H

#include "Common.h"
#include <QMainWindow>
#include <QLabel>
#include <QGridLayout>
#include <QWidget>
#include <QTimer>
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
#include <memory>

class StreamWorker;
class CameraConfigManager;

class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(int cameraId, const QString& name, QWidget* parent = nullptr);
    
    void setImage(const QImage& image);
    void setState(CameraState state);
    void setError(const QString& error);
    
    int cameraId() const { return m_cameraId; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    int m_cameraId;
    QString m_name;
    QImage m_currentFrame;
    CameraState m_state = CameraState::Disconnected;
    QString m_errorText;
    QPixmap m_scaledPixmap;
    bool m_needsRescale = true;
};

// ============================================================================
// CameraConfigDialog - Dialog per configurazione camere
// ============================================================================
class CameraConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit CameraConfigDialog(CameraConfigManager* configManager, QWidget* parent = nullptr);
    ~CameraConfigDialog() override = default;
    
    // Non-copyable
    CameraConfigDialog(const CameraConfigDialog&) = delete;
    CameraConfigDialog& operator=(const CameraConfigDialog&) = delete;

signals:
    void configSaved();

private slots:
    void onSave();
    void onCancel();
    void onTestConnection(int cameraId);
    void onUseSubStreamChanged(int cameraId, bool enabled);

private:
    void setupUI();
    void loadConfig();
    void saveConfig();
    void updateSubStreamFields(int cameraId, bool enabled);
    
    CameraConfigManager* m_configManager = nullptr;
    
    // Per ogni camera: tab con campi
    struct CameraTab {
        QWidget* widget = nullptr;
        QLineEdit* nameEdit = nullptr;
        QLineEdit* rtspUrlEdit = nullptr;
        QLineEdit* usernameEdit = nullptr;
        QLineEdit* passwordEdit = nullptr;
        QLineEdit* subStreamUrlEdit = nullptr;
        QCheckBox* useSubStreamCheck = nullptr;
        QSpinBox* reconnectIntervalSpin = nullptr;
        QSpinBox* connectionTimeoutSpin = nullptr;
        QSpinBox* receiveTimeoutSpin = nullptr;
        QSpinBox* widthSpin = nullptr;
        QSpinBox* heightSpin = nullptr;
        QSpinBox* fpsSpin = nullptr;
        QCheckBox* enableTcpCheck = nullptr;
        QCheckBox* enableBufferingCheck = nullptr;
        QCheckBox* enableHwAccelCheck = nullptr;
        QComboBox* hwAccelTypeCombo = nullptr;
        QPushButton* testBtn = nullptr;
        QLabel* testStatusLabel = nullptr;
    };
    std::array<CameraTab, 3> m_cameraTabs;
    
    QTabWidget* m_tabWidget = nullptr;
    QDialogButtonBox* m_buttonBox = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(CameraConfigManager* configManager, QWidget* parent = nullptr);
    ~MainWindow() override;
    
    // Non-copyable
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onFrameReady(FrameData frame);
    void onStateChanged(int cameraId, CameraState state);
    void onErrorOccurred(int cameraId, const QString& error);
    void onConnected(int cameraId);
    void onDisconnected(int cameraId, const QString& reason);
    void updateStatusBar();
    void onConfigChanged(int cameraId, const CameraConfig& config);
    void openCameraConfig();
    void onConfigDialogSaved();

private:
    void setupUI();
    void setupMenuBar();
    void setupStatusBar();
    void createCameraWidgets();
    void startWorkers();
    void stopWorkers();
    void applyConfig(const CameraConfig& config);
    
    CameraConfigManager* m_configManager = nullptr;
    
    // Central widget and layout
    QWidget* m_centralWidget = nullptr;
    QGridLayout* m_gridLayout = nullptr;
    
    // Video widgets (one per camera)
    std::array<std::unique_ptr<VideoWidget>, 3> m_videoWidgets;
    
    // Workers (one per camera)
    std::array<std::unique_ptr<StreamWorker>, 3> m_workers;
    
    // Status bar
    QStatusBar* m_statusBar = nullptr;
    QLabel* m_statusLabels[3] = {nullptr, nullptr, nullptr};
    QLabel* m_fpsLabel = nullptr;
    QTimer* m_statusTimer = nullptr;
    
    // Performance tracking
    std::array<uint64_t, 3> m_lastFrameCounts{0, 0, 0};
    std::array<double, 3> m_currentFps{0.0, 0.0, 0.0};
};

#endif // OMNIGUARD_MAIN_WINDOW_H
