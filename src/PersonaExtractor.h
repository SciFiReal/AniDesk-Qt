#pragma once
// ============================================================================
//  PersonaExtractor —— 多模态 Vision LLM 人设提取客户端（第二步）
//
//  输入一张本地图片，转为 Base64 data-URL，调用 Vision LLM
//  （OpenAI 兼容 /chat/completions，如本地 Ollama 的 qwen2.5vl、云端的 GLM-4V），
//  要求其返回严格 JSON：
//      {
//        "name": "角色名",
//        "personality": "性格特点",
//        "speech_style": "说话口癖/语气",
//        "greeting": "打招呼第一句话"
//      }
//  解析后组装成桌宠可用的 System Prompt，经 personaExtracted() 异步返回。
// ============================================================================
#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

// 人设提取结果
struct PersonaResult {
    QString name;          // 角色名
    QString personality;   // 性格特点
    QString speechStyle;   // 说话口癖 / 语气
    QString greeting;      // 打招呼第一句话
    QString systemPrompt;  // 组装好的 System Prompt（不含情绪标签指令）
};
Q_DECLARE_METATYPE(PersonaResult)

class PersonaExtractor : public QObject
{
    Q_OBJECT

public:
    explicit PersonaExtractor(QObject *parent = nullptr);

    // Vision LLM 配置（OpenAI 兼容端点；本地 Ollama 可留空 apiKey）
    void setApiConfig(const QString &baseUrl, const QString &apiKey, const QString &model);

    // 入口：异步提取图片角色的人设
    void extractPersona(const QString &imagePath);

    void cancel();

signals:
    void personaExtracted(const PersonaResult &result);
    void extractFailed(const QString &errorMsg);

private:
    void handleReply(QNetworkReply *reply);
    static QString imageToDataUrl(const QString &imagePath, QString *mimeType);
    static QString extractJsonBlock(const QString &text);
    PersonaResult buildFromJson(const QJsonObject &obj) const;

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;

    QString m_baseUrl = QStringLiteral("http://localhost:11434/v1");
    QString m_apiKey;
    QString m_model = QStringLiteral("qwen2.5vl:3b");   // 需为多模态模型（可换 GLM-4V 等）
};
