#pragma once
// ============================================================================
//  PetWidget —— 桌面宠物主窗口（一~三步整合核心）
//
//  窗口特性：
//    · Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool + 完全透明背景
//    · 鼠标左键按住即可平滑拖拽到屏幕任意位置
//
//  渲染与状态机（PetState：Idle/Talking/Thinking/Happy/Sleeping）：
//    · QMovie 渲染带 Alpha 通道的 GIF / APNG；片源解析优先级：
//        形象包路径（第二步管线产物）> 用户拖入素材 > 内置状态专属资源
//        （Thinking / Sleeping 始终使用内置素材）
//    · 完全没有素材时回退到 paintEvent 程序化绘制的“小团子”
//
//  AI 集成（第二、三步）：
//    · 拖入静态图片 → AssetPipelineManager 四步管线 → PetAssetPackage → 换装
//    · 气泡发言    → LLMService 流式对话（SSE）：
//        messageSent → sendMessage + Thinking
//        emotionDetected → setState（情绪驱动动图）
//        tokenReceived → ChatBubble::appendToken 打字机
//        conversationFinished / bubbleClosed → Idle
// ============================================================================
#pragma once

#include <QWidget>
#include <QPoint>
#include <QPixmap>
#include <QSize>
#include <QTimer>
#include <QElapsedTimer>

#include "AssetPipelineManager.h"   // PetAssetPackage / 管线调度器

class QMovie;
class QPainter;
class QPropertyAnimation;
class ChatBubble;
class LLMService;

class PetWidget : public QWidget
{
    Q_OBJECT

public:
    // 宠物动画状态机
    enum PetState { Idle, Talking, Thinking, Happy, Sleeping };
    Q_ENUM(PetState)

    explicit PetWidget(QWidget *parent = nullptr);

public slots:
    void setState(PetState state);            // 切换动画状态
    void showBubble();                        // 弹出头顶气泡（双击 / 右键触发）
    void loadMedia(const QString &path);      // 直接加载 GIF/APNG 动态皮肤

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    // ---- 气泡 / LLM 对话回路（第三步）----
    void onBubbleMessage(const QString &text);   // 用户在气泡里发言
    void onConversationFinished();               // LLM 流结束
    void onTypingFinished();                     // 打字机播放完毕
    void onLlmError(const QString &errorMsg);    // 网络 / API 错误
    void onBubbleClosed();                       // 气泡被关闭（中断流并复位）

    // ---- AI 资产生成管线（第二步）----
    void onPipelineProgress(const QString &stepName, int percent);
    void onPipelineCompleted(const PetAssetPackage &package);
    void onPipelineFailed(const QString &error);

    void onMovieResized(const QSize &size);      // QMovie 帧尺寸就绪 → 适配窗口
    void fallAsleep();                           // 长时间无互动 → 睡觉
private:
    enum class MediaMode { None, Movie, StaticImage };

    void setupUi();
    void setupAiServices();
    void setupBreathing();
    void restartBreathing();                  // 以当前位置为锚点重启呼吸动画
    void applyStateToMovie();                 // 按状态解析片源并渲染
    QString packagePathForState(PetState state) const;   // 形象包中的状态片源
    void applyAssetPackage(const PetAssetPackage &pkg);  // 载入管线产物
    void loadMedia(const QString &path, bool asCustomSkin);
    void applyWidgetSize(const QSize &content);
    void updateBubbleAnchor();                // 让气泡重新吸附到头顶
    void updateBubbleHold();                  // LLM/管线忙碌期间保持气泡不被自动隐藏
    void poke();                              // 任意交互：唤醒宠物并重置入睡计时
    void drawFallbackPet(QPainter &painter);  // 无素材时的程序化绘制

    // ---- 对话触发动作系统 ----
    void startAction(const QString &name);    // 播放一段动作（拔剑/跳跃/鞠躬...）
    bool actionEnvelope(double t, double *dx, double *dy, double *rot,
                        double *sx, double *sy) const;   // 动作包络：时间→位移/旋转/缩放

    // ---- UI 与渲染 ----
    QPixmap m_skinPixmap;                     // 当前皮肤帧（静态图或 QMovie 当前帧）
    QMovie *m_movie = nullptr;
    ChatBubble *m_bubble = nullptr;           // 头顶聊天气泡（以本窗口为父的顶层窗口）

    MediaMode m_mediaMode = MediaMode::None;  // 当前渲染模式
    QString m_customMediaPath;                // 用户拖入的自定义素材（所有状态共用、仅调速）
    QString m_currentMovieFile;               // 当前 QMovie 文件，避免重复 setFileName

    // ---- 状态机 ----
    PetState m_state = Idle;

    // ---- AI 服务（第二、三步）----
    LLMService *m_llm = nullptr;              // 流式对话服务
    AssetPipelineManager *m_pipeline = nullptr;  // AI 资产生成管线
    QString m_pkgMedia[3];                    // 形象包片源：[Idle, Talking, Happy]
    QString m_petName;                        // 当前角色名（管线产物）
    QString m_greeting;                       // 打招呼台词（首次双击使用）
    bool m_greetingUsed = false;
    bool m_pendingIdle = false;               // 流结束后待打字机播完回 Idle

    // ---- 拖拽 ----
    bool m_dragging = false;
    QPoint m_dragOffset;                      // 按下点相对窗口左上角的偏移

    // ---- 呼吸悬浮 ----
    QPropertyAnimation *m_breathAnim = nullptr;
    QPoint m_anchorPos;                       // 呼吸基准位置
    int m_breathAmplitude = 5;                // 上下浮动幅度（4~6px）
    bool m_lifeStarted = false;               // 首次 show 之后才启动呼吸动画

    // ---- 计时器 ----
    QTimer m_sleepTimer;                      // 无互动倒计时（60s 入睡）
    QTimer m_fallbackTimer;                   // 程序化绘制 / 静态皮肤的帧驱动（约 30fps）
    QTimer m_actionTimer;                     // 动作播放帧驱动（约 60fps）
    QElapsedTimer m_animClock;                // 呼吸微动作的时间相位
    QElapsedTimer m_actionClock;              // 动作播放计时
    QString m_actionName;                     // 正在播放的动作名（空 = 无动作）
};
