#include "LivePortraitClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>

namespace {

constexpr int kTransferTimeoutMs = 600000;   // 视频生成耗时较长，放宽超时

QString cacheDir()
{
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/cache");
    QDir().mkpath(dir);
    return dir;
}

// 三个预设驱动模板：kind（对外名称）→ template（服务端模板名）→ 缓存文件名
struct TemplateSpec {
    const char *kind;
    const char *templateName;
    const char *fileName;
};
const TemplateSpec kSpecs[] = {
    { "idle",    "blink", "Idle.gif"    },
    { "talking", "speak", "Talking.gif" },
    { "happy",   "smile", "Happy.gif"   },
};

} // namespace

// ----------------------------------------------------------------------------

LivePortraitClient::LivePortraitClient(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void LivePortraitClient::setEndpoint(const QString &baseUrl)
{
    m_baseUrl = baseUrl.trimmed();
}

QString LivePortraitClient::makeSavePath(const QString &fileName) const
{
    return cacheDir() + QLatin1Char('/') + fileName;
}

void LivePortraitClient::cancel()
{
    for (TemplateJob &job : m_jobs) {
        if (job.reply && !job.done) {
            job.done = true;
            job.reply->abort();   // finished 回调因 m_jobs 已清空而静默忽略
        }
    }
    m_jobs.clear();               // 关键：清空任务表，抑制被取消请求的迟到回调
    m_pendingCount = 0;
}

// ----------------------------------------------------------------------------

void LivePortraitClient::generateAnimations(const QString &transparentPngPath)
{
    cancel();
    m_jobs.clear();
    m_successCount = 0;
    m_lastError.clear();

    // 读取透明 PNG 并转 base64（三个模板请求共用同一份）
    QFile file(transparentPngPath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit generateAnimationsFinished(false, QStringLiteral("无法读取透明 PNG：%1").arg(transparentPngPath));
        return;
    }
    m_sourcePngPath = transparentPngPath;
    m_imageB64 = QString::fromLatin1(file.readAll().toBase64());

    m_jobs.reserve(3);
    for (const TemplateSpec &spec : kSpecs) {
        TemplateJob job;
        job.kind = QString::fromLatin1(spec.kind);
        job.templateName = QString::fromLatin1(spec.templateName);
        job.savePath = makeSavePath(QString::fromLatin1(spec.fileName));
        m_jobs.append(job);
    }

    m_pendingCount = m_jobs.size();
    for (int i = 0; i < m_jobs.size(); ++i)
        startJob(m_jobs[i]);            // 三个模板并行请求
}

void LivePortraitClient::startJob(TemplateJob &job)
{
    // 请求体：透明 PNG base64 + 驱动模板
    QJsonObject body;
    body.insert(QStringLiteral("image"), m_imageB64);
    body.insert(QStringLiteral("template"), job.templateName);
    body.insert(QStringLiteral("transparent"), true);
    body.insert(QStringLiteral("format"), QStringLiteral("gif"));
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QString url = m_baseUrl;
    if (!url.endsWith(QLatin1Char('/')))
        url += QLatin1Char('/');
    url += QStringLiteral("animate");

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(kTransferTimeoutMs);

    QNetworkReply *reply = m_nam->post(request, payload);
    job.reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        // 找到对应的 job（并发期间 m_jobs 不增删，指针稳定）
        for (TemplateJob &job : m_jobs) {
            if (job.reply == reply) {
                handleJobReply(job);
                return;
            }
        }
    });
}

void LivePortraitClient::handleJobReply(TemplateJob &job)
{
    QNetworkReply *reply = job.reply;
    job.reply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        job.done = true;
        m_lastError = QStringLiteral("%1 模板请求失败：%2").arg(job.kind, reply->errorString());
        --m_pendingCount;
        checkAllDone();
        return;
    }

    const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    const QByteArray body = reply->readAll();
    QByteArray gifBytes;
    QString urlStr;

    if (contentType.contains(QStringLiteral("json"), Qt::CaseInsensitive)) {
        // JSON 响应：优先取 gif_b64，其次跟随 url 二次下载（只跟一跳）
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        const QString b64 = obj.value(QStringLiteral("gif_b64")).toString();
        if (!b64.isEmpty()) {
            gifBytes = QByteArray::fromBase64(b64.toLatin1());
        } else {
            urlStr = obj.value(QStringLiteral("url")).toString();
        }
    } else {
        gifBytes = body;                        // image/gif 二进制
    }

    if (!urlStr.isEmpty() && !job.followedUrl) {
        job.followedUrl = true;                 // 任务保持 pending，等待下载完成
        QNetworkRequest request{QUrl(urlStr)};
        request.setTransferTimeout(kTransferTimeoutMs);
        job.reply = m_nam->get(request);
        connect(job.reply, &QNetworkReply::finished, this, [this, reply] {
            for (TemplateJob &job : m_jobs) {
                if (job.reply == reply) {
                    handleJobReply(job);
                    return;
                }
            }
        });
        return;
    }

    // 校验 GIF 魔数（GIF87a / GIF89a）后写入缓存目录
    if (gifBytes.size() > 6 && gifBytes.startsWith("GIF8")) {
        QFile out(job.savePath);
        if (out.open(QIODevice::WriteOnly) && out.write(gifBytes) == gifBytes.size()) {
            out.close();
            ++m_successCount;
            emit animationReady(job.kind, job.savePath);
        } else {
            m_lastError = QStringLiteral("GIF 写盘失败：%1").arg(job.savePath);
        }
    } else {
        m_lastError = QStringLiteral("%1 模板响应不是有效 GIF").arg(job.kind);
    }
    job.done = true;
    --m_pendingCount;
    checkAllDone();
}

void LivePortraitClient::checkAllDone()
{
    if (m_pendingCount > 0)
        return;                          // 还有模板未完成
    const bool success = (m_successCount == m_jobs.size());
    emit generateAnimationsFinished(success, m_lastError);
    m_jobs.clear();
}
