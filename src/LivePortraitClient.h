#pragma once
// ============================================================================
//  LivePortraitClient —— LivePortrait 动图驱动客户端（第二步）
//
//  输入一张透明 PNG，分别使用 3 个预设驱动模板异步生成透明 GIF：
//      idle    ← Blink  模板（眨眼 / 待机）
//      talking ← Speak  模板（说话口型）
//      happy   ← Smile  模板（微笑）
//
//  服务端契约（本地部署一个轻量 LivePortrait 包装服务即可，如 ComfyUI 工作流网关）：
//      POST {baseUrl}/animate
//      {
//          "image":      "<透明 PNG 的 base64>",
//          "template":   "blink" | "speak" | "smile",
//          "transparent": true,
//          "format":     "gif"
//      }
//      响应：image/gif 二进制字节，或 JSON { "gif_b64": "...", "url": "..." }
//
//  产物写入 <exe目录>/cache/{Idle.gif, Talking.gif, Happy.gif}，
//  每个模板完成时发射 animationReady(kind, path)，全部结束后发射
//  generateAnimationsFinished(success, error)。
// ============================================================================
#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QVector>

class LivePortraitClient : public QObject
{
    Q_OBJECT

public:
    explicit LivePortraitClient(QObject *parent = nullptr);

    // 服务地址，如 "http://127.0.0.1:7890"（实际请求 {baseUrl}/animate）
    void setEndpoint(const QString &baseUrl);

    // 入口：对透明 PNG 并行发起 3 个模板的动图生成请求
    void generateAnimations(const QString &transparentPngPath);

    void cancel();

signals:
    void animationReady(const QString &kind, const QString &gifPath);   // kind: idle/talking/happy
    void generateAnimationsFinished(bool success, const QString &error);

private:
    struct TemplateJob {
        QString kind;            // idle / talking / happy
        QString templateName;    // blink / speak / smile
        QString savePath;        // cache/Idle.gif 等
        QNetworkReply *reply = nullptr;
        bool done = false;
        bool followedUrl = false;   // 已跟随 JSON 中的 url 二次下载（只跟一跳）
    };

    void startJob(TemplateJob &job);
    void handleJobReply(TemplateJob &job);
    void checkAllDone();
    QString makeSavePath(const QString &fileName) const;

    QNetworkAccessManager *m_nam = nullptr;
    QString m_baseUrl = QStringLiteral("http://127.0.0.1:7890");
    QString m_sourcePngPath;
    QString m_imageB64;

    QVector<TemplateJob> m_jobs;
    int m_pendingCount = 0;
    int m_successCount = 0;
    QString m_lastError;
};
