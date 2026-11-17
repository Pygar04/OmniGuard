#ifndef OMNIGUARD_CAMERA_CONFIG_H
#define OMNIGUARD_CAMERA_CONFIG_H

#include "Common.h"
#include <QObject>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <array>
#include <optional>

class CameraConfigManager : public QObject {
    Q_OBJECT
public:
    explicit CameraConfigManager(QObject* parent = nullptr);
    ~CameraConfigManager() override = default;
    
    // Load configuration from JSON file
    bool loadFromFile(const QString& filePath);
    
    // Save configuration to JSON file
    bool saveToFile(const QString& filePath);
    
    // Get configuration for a specific camera
    std::optional<CameraConfig> getConfig(int cameraId) const;
    
    // Get all configurations
    std::array<std::optional<CameraConfig>, 3> getAllConfigs() const;
    
    // Update a camera configuration
    bool updateConfig(const CameraConfig& config);
    
    // Create default configuration
    static std::array<CameraConfig, 3> createDefaults();
    
    // Configuration file path
    static QString defaultConfigPath();
    
    // Percorso effettivo in uso (quello passato con -c, altrimenti il default)
    QString configPath() const;
    
signals:
    void configChanged(int cameraId, const CameraConfig& config);
    void configLoaded();
    void configSaved();
    void errorOccurred(const QString& message);

private:
    std::array<std::optional<CameraConfig>, 3> m_configs;
    QString m_configPath;
    
    void parseConfig(const QJsonObject& obj, CameraConfig& config) const;
    QJsonObject toJson(const CameraConfig& config) const;
};

#endif // OMNIGUARD_CAMERA_CONFIG_H