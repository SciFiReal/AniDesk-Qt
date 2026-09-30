#include "PersonaExtractor.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace {

constexpr int kTransferTimeoutMs = 300000;
constexpr int kMaxImageEdge = 768;    // 发给 VL 模型前把长边压到 768px，控制 base64 体积

// Vision LLM 的角色提取指令：要求只输出 JSON
const char kExtractPrompt[] =
    "请观察这张图片中的角色，发挥合理的想象力，以 JSON 格式输出它的人设，字段如下：\n"
    "{\n"
    "  \"name\": \"给角色起的名字\",\n"
    "  \"personality\": \"性格特点（一句话）\",\n"
    "  \"speech_style\": \"说话口癖/语气（一句话）\",\n"
    "  \"greeting\": \"打招呼的第一句话（口语化，不超过20字）\"\n"
    "}\n"
    "注意：只输出 JSON 本身，不要输出任何解释、前后缀或 markdown 代码块围栏。";

} // namespace

// ----------------------------------------------------------------------------

PersonaExtractor::PersonaExtractor(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void PersonaExtractor::setApiConfig(const QString &baseUrl, const QString &apiKey, const QString &model)
{
    m_baseUrl = baseUrl.trimmed();
    m_apiKey = apiKey;
    m_model = model;
}

void PersonaExtractor::cancel()
{
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }
}

// 读取图片 → 长边压缩到 768px → 编码为 data URL（有透明通道用 PNG，否则 JPEG）
QString PersonaExtractor::imageToDataUrl(const QString &imagePath, QString *mimeType)
{
    QImage image(imagePath);
    if (image.isNull())
        return QString();

    if (image.width() > kMaxImageEdge || image.height() > kMaxImageEdge)
        image = image.scaled(kMaxImageEdge, kMaxImageEdge,
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);

    const bool hasAlpha = image.hasAlphaChannel();
    const QString mime = hasAlpha ? QStringLiteral("image/png") : QStringLiteral("image/jpeg");
    if (mimeType)
        *mimeType = mime;

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, hasAlpha ? "PNG" : "JPEG");

    return QStringLiteral("data:%1;base64,%2")
        .arg(mime, QString::fromLatin1(bytes.toBase64()));
}

// ----------------------------------------------------------------------------

void PersonaExtractor::extractPersona(const QString &imagePath)
{
    cancel();

    // 1) 图片 → data URL（Base64）
    QString mimeType;
    const QString dataUrl = imageToDataUrl(imagePath, &mimeType);
    if (dataUrl.isEmpty()) {
        emit extractFailed(QStringLiteral("无法读取图片：%1").arg(imagePath));
        return;
    }

    // 2) OpenAI Vision 格式请求体：text + image_url 两个 content 分段
    QJsonObject textPart;
    textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
    textPart.insert(QStringLiteral("text"), QString::fromUtf8(kExtractPrompt));

    QJsonObject imageUrlObj;
    imageUrlObj.insert(QStringLiteral("url"), dataUrl);
    QJsonObject imagePart;
    imagePart.insert(QStringLiteral("type"), QStringLiteral("image_url"));
    imagePart.insert(QStringLiteral("image_url"), imageUrlObj);

    QJsonArray contentParts;
    contentParts.append(textPart);
    contentParts.append(imagePart);

    QJsonObject userMsg;
    userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMsg.insert(QStringLiteral("content"), contentParts);

    QJsonObject sysMsg;
    sysMsg.insert(QStringLiteral("role"), QStringLiteral("system"));
    sysMsg.insert(QStringLiteral("content"),
                  QStringLiteral("你是角色设定提取助手，只输出 JSON。"));

    QJsonArray messages;
    messages.append(sysMsg);
    messages.append(userMsg);

    QJsonObject body;
    body.insert(QStringLiteral("model"), m_model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("temperature"), 0.7);
    body.insert(QStringLiteral("stream"), false);
    // Ollama 的思考型 VL 模型同样注入 "think": false，避免人设提取等几分钟
    const bool isOllama = m_baseUrl.contains(QStringLiteral("ollama"), Qt::CaseInsensitive)
                          || m_baseUrl.contains(QStringLiteral(":11434"));
    if (isOllama)
        body.insert(QStringLiteral("think"), false);

    QString url = m_baseUrl;
    if (!url.endsWith(QLatin1Char('/')))
        url += QLatin1Char('/');
    url += QStringLiteral("chat/completions");

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_apiKey.isEmpty())
        request.setRawHeader(QByteArray("Authorization"), QByteArray("Bearer ") + m_apiKey.toUtf8());
    request.setTransferTimeout(kTransferTimeoutMs);

    // 3) 异步发送（非流式，一次取回完整 JSON）
    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (reply != m_reply)
            return;
        m_reply = nullptr;
        handleReply(reply);
    });
}

void PersonaExtractor::handleReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        emit extractFailed(QStringLiteral("HTTP %1：%2")
                               .arg(status)
                               .arg(QString::fromUtf8(reply->readAll().left(200))));
        return;
    }

    // 4) 解析 choices[0].message.content
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        emit extractFailed(QStringLiteral("响应不是有效 JSON"));
        return;
    }
    const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        emit extractFailed(QStringLiteral("响应缺少 choices 字段"));
        return;
    }
    const QString content = choices.first().toObject()
                                .value(QStringLiteral("message")).toObject()
                                .value(QStringLiteral("content")).toString();

    // 5) 从回复文本中截取 JSON 并解析（容忍 markdown 围栏 / 前后缀）
    const QString jsonText = extractJsonBlock(content);
    if (jsonText.isEmpty()) {
        emit extractFailed(QStringLiteral("回复中未找到 JSON 人设块"));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        emit extractFailed(QStringLiteral("人设 JSON 解析失败：%1").arg(parseError.errorString()));
        return;
    }

    emit personaExtracted(buildFromJson(json.object()));
}

// 从可能带 markdown 围栏 / 前后缀的文本中截取第一个平衡的 {...} 块
QString PersonaExtractor::extractJsonBlock(const QString &text)
{
    QString t = text.trimmed();

    // 剥离 ```json ... ``` 围栏
    static const QRegularExpression fence(
        QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)```"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch fenceMatch = fence.match(t);
    if (fenceMatch.hasMatch())
        t = fenceMatch.captured(1).trimmed();

    // 大括号配平扫描（跳过字符串字面量与转义）
    const int start = t.indexOf(QLatin1Char('{'));
    if (start < 0)
        return QString();
    int depth = 0;
    bool inString = false;
    for (int i = start; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (inString) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == QLatin1Char('"')) inString = false;
            continue;
        }
        if (c == QLatin1Char('"')) {
            inString = true;
        } else if (c == QLatin1Char('{')) {
            ++depth;
        } else if (c == QLatin1Char('}')) {
            --depth;
            if (depth == 0)
                return t.mid(start, i - start + 1);
        }
    }
    return QString();
}

// 解析 JSON 字段并组装 System Prompt（缺失字段给兜底值，保证管线永不中断）
PersonaResult PersonaExtractor::buildFromJson(const QJsonObject &obj) const
{
    PersonaResult r;
    r.name = obj.value(QStringLiteral("name")).toString().trimmed();
    r.personality = obj.value(QStringLiteral("personality")).toString().trimmed();
    r.speechStyle = obj.value(QStringLiteral("speech_style")).toString().trimmed();
    r.greeting = obj.value(QStringLiteral("greeting")).toString().trimmed();

    if (r.name.isEmpty())
        r.name = QStringLiteral("小可爱");
    if (r.personality.isEmpty())
        r.personality = QStringLiteral("元气满满、好奇心旺盛");
    if (r.speechStyle.isEmpty())
        r.speechStyle = QStringLiteral("活泼口语化，偶尔撒娇");
    if (r.greeting.isEmpty())
        r.greeting = QStringLiteral("嗨～我从图片里跳出来陪你啦！");

    // 组装桌宠 System Prompt（情绪标签指令由 LLMService 统一追加）
    r.systemPrompt = QStringLiteral(
                         "你现在是桌面宠物「%1」，永远以第一人称与用户对话。\n"
                         "角色性格：%2\n"
                         "说话风格（口癖/语气）：%3\n"
                         "回答务必简短口语化（一般不超过两句话），像桌宠一样陪伴用户。")
                         .arg(r.name, r.personality, r.speechStyle);
    return r;
}
