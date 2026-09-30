#pragma once
// ============================================================================
//  AssetPipelineManager —— AI 资产生成管线总调度器（第二步）
//
//  输入一张本地静态图片，异步执行 4 步流程：
//    Step 1  BackgroundRemover::removeBackground      —— 抠图去背景 → 透明 PNG
//    Step 2  （并行）
//            LivePortraitClient::generateAnimations   —— 生成 Idle/Talking/Happy.gif
//            PersonaExtractor::extractPersona         —— Vision LLM 提取人设
//    Step 3  全部完成后打包 PetAssetPackage，
//            发射 pipelineCompleted(package) 交给 PetWidget 载入。
//
//  进度经 pipelineProgress(stepName, percent) 上报（用于气泡/界面加载提示）。
//
//  优雅降级（setGracefulDegradation，默认开）：
//    · 抠图失败      → 用原图继续（视为已透明）
//    · 动图生成失败  → 对应状态回落为透明 PNG（静态形象）
//    · 人设提取失败  → 使用默认人设（名字取自文件名）
//  因此即便本地只有 Ollama 文本模型、没有抠图/动图服务，管线也能产出可用的形象包。
// ============================================================================
#pragma once

#include <QObject>
#include <QString>

#include "BackgroundRemover.h"
#include "PersonaExtractor.h"

class LivePortraitClient;

// 管线最终产物：交给 PetWidget 载入的角色资产包
struct PetAssetPackage {
    QString name;               // 角色名字
    QString systemPrompt;       // 组装好的 System Prompt
    QString greeting;           // 首次打招呼台词
    QString idleGifPath;        // 待机动图路径（失败时为透明 PNG 静态兜底）
    QString talkingGifPath;     // 说话动图路径
    QString happyGifPath;       // 开心动图路径
};
Q_DECLARE_METATYPE(PetAssetPackage)

class AssetPipelineManager : public QObject
{
    Q_OBJECT

public:
    explicit AssetPipelineManager(QObject *parent = nullptr);

    // ---- 各子服务的 API 配置（随时切换本地 / 云端）----
    void setBackgroundRemovalApi(const QString &baseUrl, const QString &apiKey);
    void setLivePortraitApi(const QString &baseUrl);
    void setVisionApi(const QString &baseUrl, const QString &apiKey, const QString &model);
    void setGracefulDegradation(bool on) { m_graceful = on; }

    // 人设提取开关：关闭后跳过 VL 模型调用（拖图秒级完成，使用文件名 + 默认人设）。
    // 适合低显存机器，或不需要 AI 起名的场景。
    void setPersonaExtractionEnabled(bool on) { m_personaEnabled = on; }

    bool isRunning() const { return m_running; }

public slots:
    // 统一入口：拖入的静态图片路径
    void processImage(const QString &imagePath);

signals:
    void pipelineProgress(const QString &stepName, int percent);   // 步骤名 + 总进度百分比
    void pipelineCompleted(const PetAssetPackage &package);
    void pipelineFailed(const QString &error);

private:
    void startParallelStage();
    void tryFinalize();
    void report(const QString &stepName, int percent);
    void resetRun();

    BackgroundRemover *m_bgRemover = nullptr;
    LivePortraitClient *m_livePortrait = nullptr;
    PersonaExtractor *m_persona = nullptr;

    // ---- 本次运行的中间状态 ----
    QString m_sourceImagePath;
    QString m_transparentPngPath;
    QString m_idleGifPath;               // 空表示该状态没有产物（用透明 PNG 兜底）
    QString m_talkingGifPath;
    QString m_happyGifPath;
    PersonaResult m_personaResult;   // 人设提取结果（第二步 Step 2b 的产物）
    bool m_personaDone = false;
    bool m_animDone = false;
    bool m_running = false;
    int m_lastPercent = 0;
    bool m_graceful = true;
    bool m_personaEnabled = true;        // 是否调用 VL 模型提取人设
};
