#include "MainWindow.h"
#include "CameraConfig.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QDir>
#include <QDebug>
#include <QTimer>
#include <csignal>
#include <cstdlib>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

// Global flag for graceful shutdown.
// L'handler di segnale fa SOLO una scrittura atomica (async-signal-safe):
// qInfo()/invokeMethod dentro un handler possono andare in deadlock.
static volatile std::sig_atomic_t g_shutdownRequested = 0;

extern "C" void signalHandler(int) {
    g_shutdownRequested = 1;
}

int main(int argc, char* argv[]) {
    // Install signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    // Initialize FFmpeg
    avformat_network_init();
    av_log_set_level(AV_LOG_WARNING);
    
    QApplication app(argc, argv);
    
    app.setApplicationName("OmniGuard");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("OmniGuard");
    app.setOrganizationDomain("omniguard.local");
    
    // Set application icon
    app.setWindowIcon(QIcon(":/omniguard.svg"));
    
    // Command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription("OmniGuard - Sistema di videosorveglianza per 3 telecamere IP Zosi");
    parser.addHelpOption();
    parser.addVersionOption();
    
    QCommandLineOption configOption("c", "Percorso file configurazione", "file");
    parser.addOption(configOption);
    
    QCommandLineOption fullscreenOption("f", "Avvia in modalità schermo intero");
    parser.addOption(fullscreenOption);
    
    parser.process(app);
    
    // Create config manager
    auto configManager = std::make_unique<CameraConfigManager>();
    
    // Load configuration
    QString configPath = parser.value(configOption);
    if (configPath.isEmpty()) {
        configPath = CameraConfigManager::defaultConfigPath();
    }
    
    if (!configManager->loadFromFile(configPath)) {
        qInfo() << "Config file not found or invalid, creating defaults at:" << configPath;
        configManager->saveToFile(configPath);
    }
    
    // Create and show main window
    MainWindow mainWindow(configManager.get());
    
    if (parser.isSet(fullscreenOption)) {
        mainWindow.showFullScreen();
    } else {
        mainWindow.show();
    }
    
    // Controlla il flag di shutdown dal thread GUI
    QTimer shutdownPoll;
    QObject::connect(&shutdownPoll, &QTimer::timeout, &app, [&mainWindow]() {
        if (g_shutdownRequested) {
            qInfo() << "Signal received, initiating graceful shutdown...";
            mainWindow.close();
            QCoreApplication::quit();
        }
    });
    shutdownPoll.start(250);
    
    // Run application
    int result = app.exec();
    
    // Cleanup
    configManager.reset();
    avformat_network_deinit();
    
    return result;
}