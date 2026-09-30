#pragma once
// ============================================================================
//  BackgroundRemover —— 自动抠图去背景模块（第二步）
//
//  通过 QNetworkAccessManager 调用本地 / 云端抠图 API，异步产出透明 PNG：
//
//  · Mode::LocalHttp   —— 自建 RMBG 类服务：
//        POST {baseUrl}（完整端点，如 http://127.0.0.1:8765/removebg）
//        请求体：原始图像字节（Content-Type: image/png|jpeg）
//        响应：  二进制图片（image/png），或 JSON { "image_b64": "...", "url": "..." }
//  · Mode::RemoveBgCloud —— remove.bg 云服务：
//        POST https://api.remove.bg/v1.0/remove-bg（multipart/form-data）
//        Header：X-Api-Key: <apiKey>；响应：PNG 二进制
//
//  结果统一落盘到 <exe目录>/cache/cutout_*.png，并通过
//  removeBackgroundFinished(transparentPngPath) 异步返回路径。
// ============================================================================
#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class BackgroundRemover : public QObject
{
    Q_OBJECT

public:
    enum class Mode { LocalHttp, RemoveBgCloud };
    Q_ENUM(Mode)

    explicit BackgroundRemover(QObject *parent = nullptr);

    // API 配置：LocalHttp 模式下 baseUrl 为完整端点；云模式使用固定端点 + apiKey
    void setConfig(const QString &baseUrl, const QString &apiKey, Mode mode);

    // 入口：异步抠图，结果经 removeBackgroundFinished / removeBackgroundFailed 返回
    void removeBackground(const QString &inputImagePath);

    void cancel();

signals:
    void removeBackgroundFinished(const QString &transparentPngPath);
    void removeBackgroundFailed(const QString &errorMsg);

private:
    void handleCutoutBytes(const QByteArray &bytes, const QString &contentType);
    bool saveCutout(const QImage &image, const QByteArray &rawBytes, QString *outPath);
    void startUrlFollowup(const QUrl &url);
    QString makeCachePath(const QString &inputPath) const;

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;        // 主请求
    QNetworkReply *m_followReply = nullptr;  // JSON 返回 url 时的二次下载

    QString m_baseUrl = QStringLiteral("http://127.0.0.1:8765/removebg");
    QString m_apiKey;
    Mode m_mode = Mode::LocalHttp;
    QString m_inputPath;
};
