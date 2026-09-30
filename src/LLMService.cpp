#include "LLMService.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>

namespace {

// 历史长度上限（防止上下文无限增长拖垮小模型）
constexpr int kMaxHistoryMessages = 24;
// 流式传输总超时（本地小模型 + 云端慢响应都足够宽裕）
constexpr int kTransferTimeoutMs = 600000;

// 追加到 System Prompt 末尾的强制情绪标签指令
const char kEmotionInstruction[] =
    "\n*请在回复的第一句开头务必附带情绪标签，格式如 [EMOTION: happy]、[EMOTION: thinking] 或 [EMOTION: normal]。*";

// 未设置人设时的兜底 System Prompt
const char kFallbackSystemPrompt[] =
    "你是 AniDesk 桌面宠物，一只活泼可爱的桌面小生物。请始终用简短、口语化的中文与用户聊天，"
    "回复一般不超过两句话。";

// 情绪字符串 → 桌宠状态：
//   happy   -> Happy（开心蹦跳）
//   thinking-> Thinking（思考摇晃）
//   sad / angry / normal -> Talking（暂无专属素材，统一用说话动图；后续版本细化）
PetWidget::PetState petStateForEmotion(const QString &emotion)
{
    if (emotion.compare(QStringLiteral("happy"), Qt::CaseInsensitive) == 0)
        return PetWidget::Happy;
    if (emotion.compare(QStringLiteral("thinking"), Qt::CaseInsensitive) == 0)
        return PetWidget::Thinking;
    return PetWidget::Talking;
}

// 从 API 错误响应体中尽量提取人类可读的信息（兼容 OpenAI / Ollama 两种格式）
QString extractApiErrorMessage(const QByteArray &body)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject obj = doc.object();
        const QJsonValue err = obj.value(QStringLiteral("error"));
        if (err.isString())
            return err.toString();
        if (err.isObject())
            return err.toObject().value(QStringLiteral("message")).toString();
        if (obj.contains(QStringLiteral("message")))
            return obj.value(QStringLiteral("message")).toString();
    }
    return QString::fromUtf8(body.left(200));
}

} // namespace

// ----------------------------------------------------------------------------

LLMService::LLMService(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

QString LLMService::normalizeBaseUrl(QString url)
{
    return url.trimmed();   // 去掉首尾空白；末尾 '/' 在拼 URL 时统一处理
}

void LLMService::setApiConfig(const QString &baseUrl, const QString &apiKey, const QString &modelName)
{
    m_baseUrl = normalizeBaseUrl(baseUrl);
    m_apiKey = apiKey;
    m_model = modelName;
}

void LLMService::setSystemPrompt(const QString &systemPrompt)
{
    // 【人设】末尾自动追加强制情绪标签指令
    m_systemPrompt = systemPrompt.trimmed() + QString::fromUtf8(kEmotionInstruction);
}

void LLMService::clearHistory()
{
    m_chatHistory.clear();
}

// 主动取消：置位 m_userAborted 后 abort；finished 回调里按“用户中断”收尾
//（半截回复不入历史）。abort 可能同步触发 finished，onReplyFinished 中的
// m_finalized 守卫保证两条路径都只清理一次。
void LLMService::cancel()
{
    if (m_reply && !m_finalized) {
        m_userAborted = true;
        m_reply->abort();
    }
}

QJsonObject LLMService::makeMessage(const QString &role, const QString &content) const
{
    QJsonObject msg;
    msg.insert(QStringLiteral("role"), role);
    msg.insert(QStringLiteral("content"), content);
    return msg;
}

void LLMService::appendHistory(const QString &role, const QString &content)
{
    m_chatHistory.append(makeMessage(role, content));
    while (m_chatHistory.size() > kMaxHistoryMessages)
        m_chatHistory.removeFirst();
}

// 安全销毁当前 reply：先断开它到本对象的所有信号连接（防止 abort 触发的
// finished/readyRead 重入），abort + deleteLater 挪到下一轮事件循环执行，
// 避免在 readyRead/finished 派发栈内销毁网络层仍在使用的对象（曾导致段错误）。
void LLMService::disposeReply()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    if (!reply)
        return;
    reply->disconnect(this);
    QTimer::singleShot(0, this, [reply] {
        reply->abort();
        reply->deleteLater();
    });
}

// ----------------------------------------------------------------------------

void LLMService::sendMessage(const QString &userMessage)
{
    const QString text = userMessage.trimmed();
    if (text.isEmpty())
        return;

    if (m_streamActive)
        cancel();                    // 新消息打断上一轮流式回复

    // 1) 用户消息入历史
    appendHistory(QStringLiteral("user"), text);

    // 2) 组装消息序列：system（人设 + 情绪指令）+ 多轮历史
    QJsonArray messages;
    QJsonObject sys;
    sys.insert(QStringLiteral("role"), QStringLiteral("system"));
    sys.insert(QStringLiteral("content"),
               m_systemPrompt.isEmpty() ? QString::fromUtf8(kFallbackSystemPrompt) : m_systemPrompt);
    messages.append(sys);
    for (const QJsonObject &msg : m_chatHistory)
        messages.append(msg);

    // 3) 构造流式请求体并选择端点风格：
    //    · Ollama + 关思考 → 原生 /api/chat（NDJSON，think 字段才生效）
    //    · 其余（云端 API / Ollama 不关思考）→ OpenAI 兼容 /v1/chat/completions（SSE）
    QString url;
    QJsonObject body;
    body.insert(QStringLiteral("model"), m_model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), true);

    const bool isOllama = m_baseUrl.contains(QStringLiteral("ollama"), Qt::CaseInsensitive)
                          || m_baseUrl.contains(QStringLiteral(":11434"));
    if (m_disableThinking && isOllama) {
        m_flavor = ApiFlavor::OllamaNative;
        QString nativeBase = m_baseUrl;
        if (nativeBase.endsWith(QStringLiteral("/v1/")))
            nativeBase.chop(4);
        else if (nativeBase.endsWith(QStringLiteral("/v1")))
            nativeBase.chop(3);
        url = nativeBase + QStringLiteral("/api/chat");
        body.insert(QStringLiteral("think"), false);          // 跳过思考阶段
        QJsonObject options;                                  // Ollama 原生采样参数
        options.insert(QStringLiteral("temperature"), 0.8);
        body.insert(QStringLiteral("options"), options);
    } else {
        m_flavor = ApiFlavor::OpenAICompat;
        url = m_baseUrl;
        if (!url.endsWith(QLatin1Char('/')))
            url += QLatin1Char('/');
        url += QStringLiteral("chat/completions");
        body.insert(QStringLiteral("temperature"), 0.8);
    }
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_apiKey.isEmpty())
        request.setRawHeader(QByteArray("Authorization"), QByteArray("Bearer ") + m_apiKey.toUtf8());
    request.setTransferTimeout(kTransferTimeoutMs);

    // 4) 重置本轮流式状态并发送（异步，不阻塞 UI）
    m_sseBuffer.clear();
    m_assembledReply.clear();
    m_emotionParser.reset();
    m_userAborted = false;
    m_finalized = false;
    m_streamActive = true;

    m_reply = m_nam->post(request, payload);
    connect(m_reply, &QNetworkReply::readyRead, this, &LLMService::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &LLMService::onReplyFinished);
}

// 流缓冲处理：按 '\n' 切出完整行；flushRemainder=true 时把无换行的尾巴也按一行处理
void LLMService::consumeBuffer(bool flushRemainder)
{
    int idx;
    while ((idx = m_sseBuffer.indexOf('\n')) != -1) {
        QByteArray line = m_sseBuffer.left(idx);
        m_sseBuffer.remove(0, idx + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        processLine(line);
        if (m_finalized)             // [DONE] / done:true / 出错后停止消费
            return;
    }
    if (flushRemainder && !m_sseBuffer.isEmpty()) {
        QByteArray line = m_sseBuffer;
        m_sseBuffer.clear();
        if (line.endsWith('\r'))
            line.chop(1);
        processLine(line);
    }
}

void LLMService::onReadyRead()
{
    if (!m_reply || m_finalized || !m_streamActive)
        return;
    m_sseBuffer += m_reply->readAll();
    consumeBuffer(false);
}

// 处理单行数据（两种协议共用一个行缓冲）：
//   OpenAI SSE:  "data: {...}" / "data: [DONE]" / 空行、event:、注释 → 忽略
//   Ollama NDJSON: 每行一个 JSON 对象 {"message":{"content":...},"done":false}
void LLMService::processLine(const QByteArray &line)
{
    if (line.startsWith("data:")) {
        const QByteArray payload = line.mid(5).trimmed();
        if (payload.isEmpty())
            return;
        if (payload == "[DONE]") {
            finalizeStream();
            return;
        }
        handleDataPayload(payload);
        return;
    }

    if (m_flavor == ApiFlavor::OllamaNative && !line.isEmpty())
        handleNativeLine(line);
}

// Ollama 原生 /api/chat 的 NDJSON 行
void LLMService::handleNativeLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return;                                  // 心跳 / 非标准行：静默忽略

    const QJsonObject root = doc.object();

    // 服务端报错（Ollama 原生格式：{"error": "..."}）
    if (root.contains(QStringLiteral("error"))) {
        const QString msg = root.value(QStringLiteral("error")).toString();
        if (!msg.isEmpty())
            failStream(msg);
        return;
    }

    // 逐 Token 提取 message.content
    const QString token = root.value(QStringLiteral("message")).toObject()
                              .value(QStringLiteral("content")).toString();
    if (!token.isEmpty()) {
        m_assembledReply += token;

        // 实时剥离情绪标记：命中 → 直接驱动桌宠状态机
        QString emotion;
        const QString cleaned = m_emotionParser.parseAndClean(token, emotion);
        if (!emotion.isEmpty())
            emit emotionDetected(petStateForEmotion(emotion));
        if (!cleaned.isEmpty())
            emit tokenReceived(cleaned);         // 打字机追加清洗后的文本
    }

    if (root.value(QStringLiteral("done")).toBool())
        finalizeStream();                        // Ollama 结束标志
}

void LLMService::handleDataPayload(const QByteArray &payload)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return;                                  // 心跳 / 非标准行：静默忽略

    const QJsonObject root = doc.object();

    // 服务端中途报错（部分实现在 200 流里包 error 对象）
    if (root.contains(QStringLiteral("error"))) {
        const QJsonValue err = root.value(QStringLiteral("error"));
        const QString msg = err.isString() ? err.toString()
                                           : err.toObject().value(QStringLiteral("message")).toString();
        if (!msg.isEmpty())
            failStream(msg);
        return;
    }

    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return;
    const QJsonObject choice = choices.first().toObject();

    // 逐 Token 提取 delta.content（兼容旧版 choices[0].text）
    QString token = choice.value(QStringLiteral("delta")).toObject()
                        .value(QStringLiteral("content")).toString();
    if (token.isEmpty())
        token = choice.value(QStringLiteral("text")).toString();
    if (token.isEmpty())
        return;   // role 增量 / reasoning_content 等非正文增量：跳过

    m_assembledReply += token;

    // 实时剥离情绪标记：命中 → 直接驱动桌宠状态机
    QString emotion;
    const QString cleaned = m_emotionParser.parseAndClean(token, emotion);
    if (!emotion.isEmpty())
        emit emotionDetected(petStateForEmotion(emotion));
    if (!cleaned.isEmpty())
        emit tokenReceived(cleaned);             // 打字机追加清洗后的文本
}

// 正常收尾（收到 [DONE] / done:true）：回复入历史 + 通知外部。
// m_finalized 保证只执行一次；reply 的销毁经 disposeReply 延后到事件循环。
void LLMService::finalizeStream()
{
    if (m_finalized)
        return;
    m_finalized = true;
    m_streamActive = false;

    // 冲刷情绪解析器中滞留的尾部文本
    const QString tail = m_emotionParser.flush();
    if (!tail.isEmpty()) {
        m_assembledReply += tail;
        emit tokenReceived(tail);
    }

    if (!m_assembledReply.isEmpty())
        appendHistory(QStringLiteral("assistant"), m_assembledReply);

    disposeReply();                              // 断开信号并延后销毁 reply
    emit conversationFinished();
}

void LLMService::failStream(const QString &msg)
{
    if (m_finalized)
        return;
    m_finalized = true;
    m_streamActive = false;
    disposeReply();
    emit errorOccurred(msg);
    emit conversationFinished();
}

void LLMService::onReplyFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    if (!reply)
        return;                                  // 已被 finalize/fail/dispose 清理

    // 先消费缓冲区中残留的最后一行（无换行结尾的情况），可能触发 finalize/fail
    if (!m_finalized)
        consumeBuffer(true);

    // 冲刷情绪解析器滞留文本（用户主动取消时丢弃半截内容）
    const QString tail = m_emotionParser.flush();
    if (!tail.isEmpty() && !m_userAborted && !m_finalized) {
        m_assembledReply += tail;
        emit tokenReceived(tail);
    }

    if (m_finalized)
        return;                                  // consumeBuffer 里已收尾（reply 已 dispose）

    m_finalized = true;
    m_streamActive = false;
    reply->deleteLater();                        // finished 派发栈内 deleteLater 是 Qt 官方模式

    // 用户主动取消：丢弃半截回复，不入历史
    if (m_userAborted) {
        emit conversationFinished();
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        if (!m_assembledReply.isEmpty()) {
            // 已收到部分回复：按“提前结束”处理，内容照常入库
            appendHistory(QStringLiteral("assistant"), m_assembledReply);
        } else {
            emit errorOccurred(QStringLiteral("网络错误：%1（请确认 Ollama 已启动：ollama serve）")
                                   .arg(reply->errorString()));
        }
        emit conversationFinished();
        return;
    }

    if (httpStatus >= 400) {
        const QByteArray body = reply->readAll();
        emit errorOccurred(QStringLiteral("API 错误(HTTP %1)：%2")
                               .arg(httpStatus)
                               .arg(extractApiErrorMessage(body)));
        emit conversationFinished();
        return;
    }

    // 正常结束：完整回复入多轮历史
    if (!m_assembledReply.isEmpty())
        appendHistory(QStringLiteral("assistant"), m_assembledReply);
    emit conversationFinished();
}
