#include "CameraConfig.h"
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QSaveFile>

CameraConfigManager::CameraConfigManager(QObject* parent)
    : QObject(parent) {
    // Initialize with defaults
    auto defaults = createDefaults();
    for (int i = 0; i < 3; ++i) {
        m_configs[i] = defaults[i];
    }
}

bool CameraConfigManager::loadFromFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.exists()) {
        qWarning() << "Config file does not exist:" << filePath;
        return false;
    }
    
    if (!file.open(QIODevice::ReadOnly)) {
        QString err = QString("Cannot open config file: %1").arg(file.errorString());
        qWarning() << err;
        emit errorOccurred(err);
        return false;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        QString err = QString("JSON parse error: %1").arg(parseError.errorString());
        qWarning() << err;
        emit errorOccurred(err);
        return false;
    }
    
    if (!doc.isObject()) {
        QString err = "Config root is not an object";
        qWarning() << err;
        emit errorOccurred(err);
        return false;
    }
    
    QJsonObject root = doc.object();
    QJsonArray cameras = root["cameras"].toArray();
    
    for (const auto& val : cameras) {
        if (!val.isObject()) continue;
        QJsonObject camObj = val.toObject();
        int id = camObj["id"].toInt(-1);
        if (id >= 0 && id < 3) {
            CameraConfig config;
            parseConfig(camObj, config);
            m_configs[id] = config;
        }
    }
    
    m_configPath = filePath;
    emit configLoaded();
    return true;
}

bool CameraConfigManager::saveToFile(const QString& filePath) {
    QJsonObject root;
    QJsonArray cameras;
    
    for (int i = 0; i < 3; ++i) {
        if (m_configs[i].has_value()) {
            cameras.append(toJson(m_configs[i].value()));
        }
    }
    
    root["cameras"] = cameras;
    root["version"] = 1;
    
    QJsonDocument doc(root);
    // QSaveFile: scrittura atomica (file temporaneo + rename). Se manca la corrente
    // durante il salvataggio il vecchio cameras.json resta integro.
    QSaveFile file(filePath);
    
    if (!file.open(QIODevice::WriteOnly)) {
        QString err = QString("Cannot write config file: %1").arg(file.errorString());
        qWarning() << err;
        emit errorOccurred(err);
        return false;
    }
    
    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        QString err = QString("Cannot write config file: %1").arg(file.errorString());
        qWarning() << err;
        emit errorOccurred(err);
        return false;
    }
    m_configPath = filePath;
    
    emit configSaved();
    return true;
}

std::optional<CameraConfig> CameraConfigManager::getConfig(int cameraId) const {
    if (cameraId >= 0 && cameraId < 3) {
        return m_configs[cameraId];
    }
    return std::nullopt;
}

std::array<std::optional<CameraConfig>, 3> CameraConfigManager::getAllConfigs() const {
    return m_configs;
}

bool CameraConfigManager::updateConfig(const CameraConfig& config) {
    if (config.id >= 0 && config.id < 3) {
        if (m_configs[config.id].has_value() && m_configs[config.id].value() == config) {
            return true;  // nessuna modifica: non far riconnettere la camera
        }
        m_configs[config.id] = config;
        emit configChanged(config.id, config);
        return true;
    }
    return false;
}

std::array<CameraConfig, 3> CameraConfigManager::createDefaults() {
    std::array<CameraConfig, 3> defaults;
    for (int i = 0; i < 3; ++i) {
        defaults[i] = CameraConfig(i, 
            QString("Camera %1").arg(i + 1),
            QString("rtsp://192.168.1.%1:554/stream1").arg(100 + i));
        defaults[i].subStreamUrl = QString("rtsp://192.168.1.%1:554/stream2").arg(100 + i);
        defaults[i].username = "admin";
        defaults[i].password = "admin";
        defaults[i].reconnectIntervalMs = 3000;
        defaults[i].connectionTimeoutMs = 10000;
        defaults[i].receiveTimeoutMs = 5000;
        defaults[i].enableTcp = true;
        defaults[i].enableBuffering = true;
        defaults[i].useSubStream = false;
        defaults[i].enableHwAccel = false;
        defaults[i].hwAccelType = "qsv";  // Default to Intel QuickSync
        defaults[i].width = 1920;
        defaults[i].height = 1080;
        defaults[i].fps = 25;
    }
    return defaults;
}

QString CameraConfigManager::configPath() const {
    return m_configPath.isEmpty() ? defaultConfigPath() : m_configPath;
}

QString CameraConfigManager::defaultConfigPath() {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir dir(configDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return configDir + "/cameras.json";
}

void CameraConfigManager::parseConfig(const QJsonObject& obj, CameraConfig& config) const {
    config.id = obj["id"].toInt(config.id);
    config.name = obj["name"].toString(config.name);
    config.rtspUrl = obj["rtspUrl"].toString(config.rtspUrl);
    config.subStreamUrl = obj["subStreamUrl"].toString(config.subStreamUrl);
    config.username = obj["username"].toString(config.username);
    config.password = obj["password"].toString(config.password);
    config.reconnectIntervalMs = obj["reconnectIntervalMs"].toInt(config.reconnectIntervalMs);
    config.connectionTimeoutMs = obj["connectionTimeoutMs"].toInt(config.connectionTimeoutMs);
    config.receiveTimeoutMs = obj["receiveTimeoutMs"].toInt(config.receiveTimeoutMs);
    config.enableTcp = obj["enableTcp"].toBool(config.enableTcp);
    config.enableBuffering = obj["enableBuffering"].toBool(config.enableBuffering);
    config.useSubStream = obj["useSubStream"].toBool(config.useSubStream);
    config.enableHwAccel = obj["enableHwAccel"].toBool(config.enableHwAccel);
    config.hwAccelType = obj["hwAccelType"].toString(config.hwAccelType);
    config.lowCpuDecode = obj["lowCpuDecode"].toBool(config.lowCpuDecode);
    config.width = obj["width"].toInt(config.width);
    config.height = obj["height"].toInt(config.height);
    config.fps = obj["fps"].toInt(config.fps);
}

QJsonObject CameraConfigManager::toJson(const CameraConfig& config) const {
    QJsonObject obj;
    obj["id"] = config.id;
    obj["name"] = config.name;
    obj["rtspUrl"] = config.rtspUrl;
    obj["subStreamUrl"] = config.subStreamUrl;
    obj["username"] = config.username;
    obj["password"] = config.password;
    obj["reconnectIntervalMs"] = config.reconnectIntervalMs;
    obj["connectionTimeoutMs"] = config.connectionTimeoutMs;
    obj["receiveTimeoutMs"] = config.receiveTimeoutMs;
    obj["enableTcp"] = config.enableTcp;
    obj["enableBuffering"] = config.enableBuffering;
    obj["useSubStream"] = config.useSubStream;
    obj["enableHwAccel"] = config.enableHwAccel;
    obj["hwAccelType"] = config.hwAccelType;
    obj["lowCpuDecode"] = config.lowCpuDecode;
    obj["width"] = config.width;
    obj["height"] = config.height;
    obj["fps"] = config.fps;
    return obj;
}