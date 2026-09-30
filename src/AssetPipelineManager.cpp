#include "AssetPipelineManager.h"
#include "LivePortraitClient.h"

#include <QFileInfo>

namespace {

// 管线接受的静态图扩展名（GIF 由 PetWidget 直接套用，不进管线）
const QStringList kSupportedExts = {
    QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
    QStringLiteral("bmp"), QStringLiteral("webp")
};

} // namespace

// ----------------------------------------------------------------------------

AssetPipelineManager::AssetPipelineManager(QObject *parent)
    : QObject(parent)
{
    m_bgRemover = new BackgroundRemover(this);
    m_livePortrait = new LivePortraitClient(this);
    m_persona = new PersonaExtractor(this);

    // 说明：三个子模块在 cancel() 后都会静默丢弃被中止请求的回调
    //（BackgroundRemover/PersonaExtractor 校验 reply 指针，LivePortraitClient 清空任务表），
    // 因此这里用 m_running 守卫即可，无需额外的运行代际号。

    // ---- Step 1：抠图 ----
    connect(m_bgRemover, &BackgroundRemover::removeBackgroundFinished, this,
            [this](const QString &pngPath) {
                if (!m_running)
                    return;
                m_transparentPngPath = pngPath;
                report(QStringLiteral("抠图完成！"), 30);
                startParallelStage();          // Step 2：并行启动动图与人设
            });
    connect(m_bgRemover, &BackgroundRemover::removeBackgroundFailed, this,
            [this](const QString &error) {
                if (!m_running)
                    return;
                if (m_graceful) {
                    // 优雅降级：抠图服务不可用 → 直接用原图继续
                    m_transparentPngPath = m_sourceImagePath;
                    report(QStringLiteral("抠图服务不可用，已跳过（使用原图）"), 30);
                    startParallelStage();
                } else {
                    m_running = false;
                    emit pipelineFailed(error);
                }
            });

    // ---- Step 2a：动图生成（逐个模板就绪 + 总体完成）----
    connect(m_livePortrait, &LivePortraitClient::animationReady, this,
            [this](const QString &kind, const QString &gifPath) {
                if (!m_running)
                    return;
                if (kind == QStringLiteral("idle"))
                    m_idleGifPath = gifPath;
                else if (kind == QStringLiteral("talking"))
                    m_talkingGifPath = gifPath;
                else if (kind == QStringLiteral("happy"))
                    m_happyGifPath = gifPath;
                report(QStringLiteral("动画模板 %1 生成完成").arg(kind), qMin(m_lastPercent + 8, 60));
            });
    connect(m_livePortrait, &LivePortraitClient::generateAnimationsFinished, this,
            [this](bool success, const QString &error) {
                if (!m_running)
                    return;
                m_animDone = true;
                if (!success)
                    report(QStringLiteral("动图生成失败，将使用静态形象"), m_lastPercent);
                tryFinalize();
            });

    // ---- Step 2b：人设提取 ----
    connect(m_persona, &PersonaExtractor::personaExtracted, this,
            [this](const PersonaResult &result) {
                if (!m_running)
                    return;
                m_personaResult = result;
                m_personaDone = true;
                report(QStringLiteral("人设提取完成：%1").arg(result.name), 70);
                tryFinalize();
            });
    connect(m_persona, &PersonaExtractor::extractFailed, this,
            [this](const QString &error) {
                if (!m_running)
                    return;
                m_personaDone = true;
                if (m_graceful) {
                    // 透出真实失败原因（如 HTTP 404 模型未拉取、显存不足等），方便排查
                    report(QStringLiteral("人设提取失败（%1），使用默认人设").arg(error),
                           m_lastPercent);
                    tryFinalize();
                } else {
                    m_running = false;
                    emit pipelineFailed(error);
                }
            });
}

void AssetPipelineManager::setBackgroundRemovalApi(const QString &baseUrl, const QString &apiKey)
{
    // 端点含 remove.bg 时自动切到云端模式（multipart + X-Api-Key）
    const bool isCloud = baseUrl.contains(QStringLiteral("remove.bg"), Qt::CaseInsensitive);
    m_bgRemover->setConfig(baseUrl, apiKey,
                           isCloud ? BackgroundRemover::Mode::RemoveBgCloud
                                   : BackgroundRemover::Mode::LocalHttp);
}

void AssetPipelineManager::setLivePortraitApi(const QString &baseUrl)
{
    m_livePortrait->setEndpoint(baseUrl);
}

void AssetPipelineManager::setVisionApi(const QString &baseUrl, const QString &apiKey, const QString &model)
{
    m_persona->setApiConfig(baseUrl, apiKey, model);
}

// ----------------------------------------------------------------------------

void AssetPipelineManager::processImage(const QString &imagePath)
{
    if (m_running) {
        // 允许用新图片打断上一次管线
        m_bgRemover->cancel();
        m_livePortrait->cancel();
        m_persona->cancel();
    }

    // ---- 入参校验 ----
    const QFileInfo info(imagePath);
    if (!info.exists() || !info.isFile()) {
        emit pipelineFailed(QStringLiteral("图片不存在：%1").arg(imagePath));
        return;
    }
    if (!kSupportedExts.contains(info.suffix().toLower())) {
        emit pipelineFailed(QStringLiteral("不支持的图片格式：%1").arg(info.suffix()));
        return;
    }

    // ---- 重置本轮运行状态 ----
    m_transparentPngPath.clear();
    m_idleGifPath.clear();
    m_talkingGifPath.clear();
    m_happyGifPath.clear();
    m_personaResult = PersonaResult{};
    m_personaDone = false;
    m_animDone = false;
    m_lastPercent = 0;
    m_sourceImagePath = imagePath;
    m_running = true;

    // ---- Step 1：抠图去背景 ----
    report(QStringLiteral("正在抠图去背景..."), 10);
    m_bgRemover->removeBackground(imagePath);
}

// Step 2：抠图完成后，动图生成与人设提取并行启动
void AssetPipelineManager::startParallelStage()
{
    report(QStringLiteral("正在并行生成动画与人设..."), 40);
    m_livePortrait->generateAnimations(m_transparentPngPath);
    if (m_personaEnabled) {
        m_persona->extractPersona(m_sourceImagePath);   // 人设用原图（背景信息有助于理解角色）
    } else {
        m_personaDone = true;                           // 人设提取已关闭：跳过 VL 调用
        report(QStringLiteral("人设提取已关闭，使用默认人设"), m_lastPercent);
    }
}

// Step 3：等待两个并行分支均结束后打包
void AssetPipelineManager::tryFinalize()
{
    if (!m_running || !m_animDone || !m_personaDone)
        return;

    // 人设缺失（非优雅模式下的失败）时给兜底值
    if (m_personaResult.name.isEmpty()) {
        m_personaResult.name = QFileInfo(m_sourceImagePath).completeBaseName();
        m_personaResult.personality = QStringLiteral("元气满满、好奇心旺盛");
        m_personaResult.speechStyle = QStringLiteral("活泼口语化");
        m_personaResult.greeting = QStringLiteral("嗨～我从图片里跳出来陪你啦！");
        m_personaResult.systemPrompt = QStringLiteral(
                                     "你现在是桌面宠物「%1」，永远以第一人称与用户对话。\n"
                                     "回答务必简短口语化，像桌宠一样陪伴用户。")
                                     .arg(m_personaResult.name);
    }

    // ---- 打包：动图缺失的状态回落为透明 PNG（PetWidget 支持静态形象）----
    PetAssetPackage pkg;
    pkg.name = m_personaResult.name;
    pkg.systemPrompt = m_personaResult.systemPrompt;
    pkg.greeting = m_personaResult.greeting;
    pkg.idleGifPath = m_idleGifPath.isEmpty() ? m_transparentPngPath : m_idleGifPath;
    pkg.talkingGifPath = m_talkingGifPath.isEmpty() ? m_transparentPngPath : m_talkingGifPath;
    pkg.happyGifPath = m_happyGifPath.isEmpty() ? m_transparentPngPath : m_happyGifPath;

    m_running = false;
    report(QStringLiteral("全部完成！"), 100);
    emit pipelineCompleted(pkg);
}

void AssetPipelineManager::report(const QString &stepName, int percent)
{
    m_lastPercent = qMax(m_lastPercent, percent);   // 进度单调不减
    emit pipelineProgress(stepName, m_lastPercent);
}
