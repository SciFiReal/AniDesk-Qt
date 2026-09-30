#pragma once
// ============================================================================
//  LLMService —— 大模型流式对话服务与历史记录管理（第三步）
//
//  对接 OpenAI 兼容的 /chat/completions 接口（stream: true）：
//    · 默认指向本地 Ollama：http://localhost:11434/v1（本机已部署 qwen3.5:0.8b）；
//    · setApiConfig() 一行切换到任意云端 API（GLM / OpenAI / DeepSeek 等）；
//  System Prompt 末尾会自动追加强制情绪标签指令，模型回复形如：
//      [EMOTION: happy] 你好呀！见到你真开心～
//  解析管线：
//      QNetworkReply::readyRead → SSE 行解析 (data: {...})
//        → choices[0].delta.content 逐 token
//        → EmotionParser 剥离情绪标签 → tokenReceived(清洗后的文本)
//        → 命中标签时 emotionDetected(PetState) 直接驱动桌宠状态机
//  全程异步（信号/槽），绝不阻塞 UI 线程。
// ============================================================================
#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

#include "EmotionParser.h"
#include "PetWidget.h"      // 复用 PetState 枚举（情绪 → 桌宠状态）

class LLMService : public QObject
{
    Q_OBJECT

public:
    explicit LLMService(QObject *parent = nullptr);

    // API 配置：baseUrl 形如 "http://localhost:11434/v1" 或 "https://api.xx.com/v1"；
    // 本地 Ollama 可留空 apiKey；云端 API 填对应 Key（自动加 Bearer 头）。
    void setApiConfig(const QString &baseUrl, const QString &apiKey, const QString &modelName);

    // 思考型模型（如 qwen3.5）的思考阶段可能长达数分钟；开启后对 Ollama 端点
    // 注入 "think": false 跳过思考（云端 API 不受影响）。默认开。
    void setDisableThinking(bool on) { m_disableThinking = on; }

    // 设置人设 System Prompt；末尾自动追加强制情绪标签指令。
    void setSystemPrompt(const QString &systemPrompt);

    void clearHistory();                 // 清空多轮对话记忆
    bool isBusy() const { return m_streamActive; }
    void cancel();                       // 主动中断当前流（如气泡被关闭）

public slots:
    void sendMessage(const QString &userMessage);   // 发送用户消息（流式回复）

signals:
    void tokenReceived(const QString &token);       // 逐 token（已清洗情绪标签）
    void emotionDetected(PetWidget::PetState state);// 识别到情绪标记 → 驱动状态机
    void conversationFinished();                    // 本轮流式回复结束
    void errorOccurred(const QString &errorMsg);    // 网络 / API 错误

private slots:
    void onReadyRead();
    void onReplyFinished();

private:
    // 端点风格：OpenAI 兼容 /chat/completions(SSE) 或 Ollama 原生 /api/chat(NDJSON)。
    // 思考型模型必须走原生端点才能注入 "think": false（/v1 兼容层会忽略该字段，
    // 导致 qwen3.5 等模型思考阶段长达数十秒甚至数分钟）。
    enum class ApiFlavor { OpenAICompat, OllamaNative };

    QJsonObject makeMessage(const QString &role, const QString &content) const;
    void appendHistory(const QString &role, const QString &content);
    void consumeBuffer(bool flushRemainder);        // 从流缓冲中切出完整行
    void processLine(const QByteArray &line);
    void handleDataPayload(const QByteArray &payload);   // OpenAI: choices[0].delta.content
    void handleNativeLine(const QByteArray &line);       // Ollama: message.content / done
    void finalizeStream();                          // [DONE] / done:true：正常收尾
    void failStream(const QString &msg);            // 流中途失败
    void disposeReply();                            // 断开信号并延后销毁 reply（防重入崩溃）
    static QString normalizeBaseUrl(QString url);

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;

    QString m_baseUrl = QStringLiteral("http://localhost:11434/v1");
    QString m_apiKey;
    QString m_model = QStringLiteral("qwen3.5:0.8b"); // 本机 Ollama 已部署的模型
    QString m_systemPrompt;

    ApiFlavor m_flavor = ApiFlavor::OpenAICompat;

    QList<QJsonObject> m_chatHistory;    // 多轮上下文（role/content，不含 system）
    QByteArray m_sseBuffer;              // SSE 半行缓冲
    QString m_assembledReply;            // 本轮已收到的完整回复（用于入库）
    EmotionParser m_emotionParser;       // 流式情绪标签剥离器

    bool m_streamActive = false;
    bool m_userAborted = false;
    bool m_finalized = false;            // 本轮已收尾（防重入 / 防二次清理）
    bool m_disableThinking = true;       // 对 Ollama 注入 "think": false
};
