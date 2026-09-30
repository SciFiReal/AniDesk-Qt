#include "BackgroundRemover.h"

#include <QCoreApplication>
#include <QDir>
#include <QHttpMultiPart>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QNetworkRequest>

namespace {

constexpr int kTransferTimeoutMs = 120000;

QString cacheDir()
{
    // 与 LivePortraitClient 共用 <exe目录>/cache
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/cache");
    QDir().mkpath(dir);
    return dir;
}

} // namespace

// ----------------------------------------------------------------------------

BackgroundRemover::BackgroundRemover(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void BackgroundRemover::setConfig(const QString &baseUrl, const QString &apiKey, Mode mode)
{
    m_baseUrl = baseUrl.trimmed();
    m_apiKey = apiKey;
    m_mode = mode;
}

void BackgroundRemover::cancel()
{
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }
    if (m_followReply) {
        m_followReply->abort();
        m_followReply = nullptr;
    }
}

QString BackgroundRemover::makeCachePath(const QString &inputPath) const
{
    const QString base = QFileInfo(inputPath).completeBaseName();
    const QString stamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    return cacheDir() + QStringLiteral("/cutout_%1_%2.png").arg(base, stamp);
}

// ----------------------------------------------------------------------------

void BackgroundRemover::removeBackground(const QString &inputImagePath)
{
    cancel();

    QFile file(inputImagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit removeBackgroundFailed(QStringLiteral("无法读取图片：%1").arg(inputImagePath));
        return;
    }
    const QByteArray raw = file.readAll();
    if (raw.isEmpty()) {
        emit removeBackgroundFailed(QStringLiteral("图片内容为空：%1").arg(inputImagePath));
        return;
    }
    m_inputPath = inputImagePath;

    static const QMimeDatabase mimeDb;
    const QString mimeType = mimeDb.mimeTypeForData(raw).name();   // image/png / image/jpeg ...

    QNetworkReply *reply = nullptr;

    if (m_mode == Mode::RemoveBgCloud) {
        // ---- remove.bg 云 API：multipart/form-data + X-Api-Key ----
        auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

        QHttpPart imagePart;
        imagePart.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
        imagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                            QStringLiteral("form-data; name=\"image_file\"; filename=\"input.%1\"")
                                .arg(QFileInfo(inputImagePath).suffix()));
        imagePart.setBody(raw);
        multiPart->append(imagePart);
        multiPart->setParent(reply);   // 占位，下面 post 后再挂到 reply 上

        QNetworkRequest request{QUrl(m_baseUrl)};
        request.setRawHeader(QByteArray("X-Api-Key"), m_apiKey.toUtf8());
        request.setTransferTimeout(kTransferTimeoutMs);

        reply = m_nam->post(request, multiPart);
        multiPart->setParent(reply);   // multiPart 生命周期跟随 reply
    } else {
        // ---- 本地 RMBG 类服务：原始图像字节直投，Accept 声明可接受的响应 ----
        QNetworkRequest request{QUrl(m_baseUrl)};
        request.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
        request.setRawHeader(QByteArray("Accept"), QByteArray("image/png, application/json"));
        request.setTransferTimeout(kTransferTimeoutMs);
        reply = m_nam->post(request, raw);
    }

    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (reply != m_reply)
            return;                     // 已被 cancel / 新请求取代
        m_reply = nullptr;
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            emit removeBackgroundFailed(QStringLiteral("HTTP %1：%2")
                                            .arg(status)
                                            .arg(QString::fromUtf8(reply->readAll().left(200))));
            return;
        }
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        handleCutoutBytes(reply->readAll(), contentType);
    });
}

// 依据 Content-Type 分流：二进制图片直接落盘；JSON 则解 base64 或跟随 url 二次下载
void BackgroundRemover::handleCutoutBytes(const QByteArray &bytes, const QString &contentType)
{
    if (contentType.contains(QStringLiteral("json"), Qt::CaseInsensitive)) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            emit removeBackgroundFailed(QStringLiteral("响应 JSON 解析失败"));
            return;
        }
        const QJsonObject obj = doc.object();
        // 兼容多种字段命名
        const QStringList b64Keys = { QStringLiteral("image_b64"), QStringLiteral("image"),
                                      QStringLiteral("data"), QStringLiteral("result") };
        for (const QString &key : b64Keys) {
            const QString b64 = obj.value(key).toString();
            if (!b64.isEmpty()) {
                const QImage img = QImage::fromData(QByteArray::fromBase64(b64.toLatin1()));
                QString path;
                if (!img.isNull() && saveCutout(img, QByteArray(), &path))
                    emit removeBackgroundFinished(path);
                else
                    emit removeBackgroundFailed(QStringLiteral("base64 图像解码失败"));
                return;
            }
        }
        const QString urlStr = obj.value(QStringLiteral("url")).toString();
        if (!urlStr.isEmpty()) {
            startUrlFollowup(QUrl(urlStr));   // 二次下载（异步）
            return;
        }
        emit removeBackgroundFailed(QStringLiteral("响应 JSON 中缺少图像数据字段"));
        return;
    }

    // 二进制图片（image/png 等）
    const QImage img = QImage::fromData(bytes);
    QString path;
    if (!img.isNull() && saveCutout(img, bytes, &path))
        emit removeBackgroundFinished(path);
    else
        emit removeBackgroundFailed(QStringLiteral("返回内容无法解码为图像"));
}

// JSON 返回 url 时的二次下载
void BackgroundRemover::startUrlFollowup(const QUrl &url)
{
    QNetworkRequest request{url};
    request.setTransferTimeout(kTransferTimeoutMs);
    QNetworkReply *reply = m_nam->get(request);
    m_followReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (reply != m_followReply)
            return;
        m_followReply = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit removeBackgroundFailed(QStringLiteral("抠图结果下载失败：%1").arg(reply->errorString()));
            return;
        }
        const QByteArray bytes = reply->readAll();
        const QImage img = QImage::fromData(bytes);
        QString path;
        if (!img.isNull() && saveCutout(img, bytes, &path))
            emit removeBackgroundFinished(path);
        else
            emit removeBackgroundFailed(QStringLiteral("下载的抠图结果无法解码"));
    });
}

// 校验图像可解码后写盘（优先原样保存服务端字节，避免二次编码损失透明度）
bool BackgroundRemover::saveCutout(const QImage &image, const QByteArray &rawBytes, QString *outPath)
{
    if (image.isNull())
        return false;
    const QString path = makeCachePath(m_inputPath);
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    if (!rawBytes.isEmpty()) {
        out.write(rawBytes);            // 服务端返回的即为图像字节：原样落盘
    } else {
        image.save(&out, "PNG");        // base64 解码结果：统一编码为 PNG
    }
    out.close();
    if (out.error() != QFileDevice::NoError)
        return false;
    *outPath = path;
    return true;
}
