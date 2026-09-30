#include "PetWidget.h"
#include "ChatBubble.h"
#include "LLMService.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMouseEvent>
#include <QMovie>
#include <QMimeData>
#include <QPainter>
#include <QPropertyAnimation>
#include <QRandomGenerator>
#include <QScreen>
#include <QShortcut>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kMaxPetSize = 340;      // 形象显示尺寸上限（px）
constexpr int kInactivityMs = 60000;  // 连续 60 秒无互动 → 进入睡眠
constexpr double kPi = 3.14159265358979323846;

// 是否在拖图管线中调用 VL 模型提取人设（AI 起名）。
// 显存紧张（如 4GB 卡跑 3b 模型要 1~3 分钟）时可改为 false：
// 拖图秒级完成，角色用文件名起名 + 默认人设。
constexpr bool kEnablePersonaExtraction = true;

// 各状态对应的 QMovie 播放速率（百分比）——状态机的“速率切换”维度：
// Idle 正常呼吸眨眼 / Talking 快速动嘴 / Thinking 慢速摇晃 / Happy 欢快 / Sleeping 近乎静止
int speedForState(PetWidget::PetState state)
{
    switch (state) {
    case PetWidget::Talking:  return 240;
    case PetWidget::Thinking: return 55;
    case PetWidget::Happy:    return 200;
    case PetWidget::Sleeping: return 40;
    case PetWidget::Idle:
    default:                  return 100;
    }
}

// 各状态专属片源在 qrc 资源中的文件名（: /pets/<state>.gif）
const char *stateAssetName(PetWidget::PetState state)
{
    switch (state) {
    case PetWidget::Talking:  return "talking";
    case PetWidget::Thinking: return "thinking";
    case PetWidget::Happy:    return "happy";
    case PetWidget::Sleeping: return "sleeping";
    case PetWidget::Idle:
    default:                  return "idle";
    }
}

// 把任意内容尺寸约束到 [48, maxW/maxH] 区间，并保持宽高比
QSize fitSize(const QSize &size, int maxW, int maxH)
{
    QSize s = size;
    if (s.width() > maxW || s.height() > maxH)
        s.scale(maxW, maxH, Qt::KeepAspectRatio);
    return s.expandedTo(QSize(48, 48));
}

// 拖放支持的图像扩展名（含动图；管线另有一份静态图清单）
bool isSupportedImageFile(const QString &path)
{
    static const QStringList kSupported = {
        QStringLiteral("gif"), QStringLiteral("apng"), QStringLiteral("png"),
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("bmp"),
        QStringLiteral("webp"), QStringLiteral("mng")
    };
    return kSupported.contains(QFileInfo(path).suffix().toLower());
}

// 该文件是否为 QMovie 可播放的动图格式（GIF/APNG/MNG/WEBP，或多帧 PNG）。
// 注意：QMovie 无法播放静态 PNG/JPEG——单帧图必须走 QPixmap 渲染路径，
// 否则会静默失败、画面停留在旧形象上。
bool isAnimatedMediaFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("gif") || suffix == QLatin1String("apng")
        || suffix == QLatin1String("mng") || suffix == QLatin1String("webp"))
        return true;
    if (suffix == QLatin1String("png"))
        return QImageReader(path).imageCount() > 1;
    return false;
}

// 双击 / 右键时气泡里的随机台词
const QStringList kIdlePhrases = {
    QStringLiteral("嗨～我是 AniDesk 桌宠！按住我可以把我拖到屏幕任意角落哦～"),
    QStringLiteral("把图片拖到我身上，AI 会帮我变成它的样子哦！"),
    QStringLiteral("在下面的输入框里跟我说说话吧～"),
    QStringLiteral("今天也要元气满满哦！"),
    QStringLiteral("很久不理我的话……我可是会睡着的！"),
};

// 对话触发的动作关键词（MVP：关键词直配；后续可由 LLM 输出动作标签）
QString matchActionKeyword(const QString &text)
{
    if (text.contains(QStringLiteral("剑")))                                      return QStringLiteral("slash");
    if (text.contains(QStringLiteral("跳")) || text.contains(QStringLiteral("蹦"))) return QStringLiteral("jump");
    if (text.contains(QStringLiteral("鞠躬")) || text.contains(QStringLiteral("拜托"))) return QStringLiteral("bow");
    if (text.contains(QStringLiteral("点头")))                                     return QStringLiteral("nod");
    if (text.contains(QStringLiteral("转圈")) || text.contains(QStringLiteral("转身"))) return QStringLiteral("spin");
    if (text.contains(QStringLiteral("冲")) || text.contains(QStringLiteral("跑"))) return QStringLiteral("dash");
    if (text.contains(QStringLiteral("摇头")))                                     return QStringLiteral("shake");
    return QString();
}

// 缓动函数（动作包络用）
double easeOutCubic(double k)
{
    k = qBound(0.0, k, 1.0);
    return 1.0 - std::pow(1.0 - k, 3);
}

double easeInOut(double k)
{
    k = qBound(0.0, k, 1.0);
    return k < 0.5 ? 2.0 * k * k : 1.0 - std::pow(-2.0 * k + 2.0, 2) / 2.0;
}

} // namespace

// ============================================================================

PetWidget::PetWidget(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
{
    // 【窗口特性】完全透明背景 + 接受拖放
    setAttribute(Qt::WA_TranslucentBackground);
    setAcceptDrops(true);
    setWindowTitle(tr("AniDesk 桌面宠物"));

    setupUi();
    setupAiServices();
    setupBreathing();

    // 入睡倒计时：60s 无互动 → Sleeping
    m_sleepTimer.setSingleShot(true);
    m_sleepTimer.setInterval(kInactivityMs);
    connect(&m_sleepTimer, &QTimer::timeout, this, &PetWidget::fallAsleep);

    // 程序化绘制 / 静态皮肤的帧驱动（仅在无素材与静态形象模式下运行）
    connect(&m_fallbackTimer, &QTimer::timeout, this, [this] { update(); });

    // 动作播放帧驱动（约 60fps，动作结束后自动停）
    m_actionTimer.setInterval(16);
    connect(&m_actionTimer, &QTimer::timeout, this, [this] {
        double dx = 0, dy = 0, rot = 0, sx = 1, sy = 1;
        if (!actionEnvelope(m_actionClock.elapsed() / 1000.0, &dx, &dy, &rot, &sx, &sy)) {
            m_actionTimer.stop();
            m_actionName.clear();
        }
        update();
    });

    // Ctrl+Q 退出原型（无边框窗口没有自带关闭按钮）
    auto *quitShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Q")), this);
    connect(quitShortcut, &QShortcut::activated, qApp, &QCoreApplication::quit);

    // 初始尺寸（加载素材成功后会被 QMovie::resized / 静态图尺寸覆盖）
    applyWidgetSize(QSize(230, 230));

    // 优先加载内置默认形象（qrc 资源）；没有资源则进入程序化绘制兜底模式
    const QString defaultPet = QStringLiteral(":/pets/idle.gif");
    if (QFile::exists(defaultPet)) {
        loadMedia(defaultPet, /*asCustomSkin=*/false);
    } else {
        m_mediaMode = MediaMode::None;
        m_fallbackTimer.start();
        qInfo() << "[AniDesk] 未找到内置素材，启用程序化绘制形象。";
    }

    m_animClock.start();
}

// ----------------------------------------------------------------------------
//  初始化
// ----------------------------------------------------------------------------

void PetWidget::setupUi()
{
    // 皮肤渲染统一走 paintEvent（静态图 / QMovie 帧都由本窗口绘制，
    // 这样对话触发的动作变换才能作用在任何形象上）
    m_movie = new QMovie(this);
    // QMovie 帧尺寸就绪 → 让窗口尺寸贴合形象
    connect(m_movie, &QMovie::resized, this, &PetWidget::onMovieResized);
    // 每解码一帧 → 取出当前帧供 paintEvent 绘制
    connect(m_movie, &QMovie::frameChanged, this, [this](int) {
        m_skinPixmap = m_movie->currentPixmap();
        update();
    });

    // 头顶气泡：以宠物为父窗口的 Qt::Tool 顶层窗口，天然悬浮于宠物之上
    m_bubble = new ChatBubble(this);
    connect(m_bubble, &ChatBubble::messageSent, this, &PetWidget::onBubbleMessage);
    connect(m_bubble, &ChatBubble::typingFinished, this, &PetWidget::onTypingFinished);
    connect(m_bubble, &ChatBubble::geometryDirtied, this, &PetWidget::updateBubbleAnchor);
    connect(m_bubble, &ChatBubble::bubbleClosed, this, &PetWidget::onBubbleClosed);
}

// 【AI 服务装配】第二、三步的两大引擎 + 信号接线
void PetWidget::setupAiServices()
{
    // ---- 第三步：LLM 流式对话服务 ----
    // 默认对接本地 Ollama（本机已部署 qwen3.5:0.8b；服务未启动时会在气泡里收到提示）
    m_llm = new LLMService(this);
    m_llm->setApiConfig(QStringLiteral("http://localhost:11434/v1"),
                        QString(),
                        QStringLiteral("qwen3.5:0.8b"));

    // token → 打字机追加；情绪 → 状态机；结束/出错 → 复位
    connect(m_llm, &LLMService::tokenReceived, m_bubble, &ChatBubble::appendToken);
    connect(m_llm, &LLMService::emotionDetected, this, &PetWidget::setState);
    connect(m_llm, &LLMService::conversationFinished, this, &PetWidget::onConversationFinished);
    connect(m_llm, &LLMService::errorOccurred, this, &PetWidget::onLlmError);

    // “清空”按钮：清空气泡显示 + 清空模型多轮记忆
    connect(m_bubble, &ChatBubble::clearConversationRequested, this, [this] {
        m_llm->clearHistory();
        m_bubble->appendToken(QStringLiteral("（对话记录与记忆已清空）"));
    });

    // ---- 第二步：AI 资产生成管线 ----
    // 三个子服务的地址都可通过对应 set*Api() 随时替换（本地 / 云端）
    m_pipeline = new AssetPipelineManager(this);
    m_pipeline->setBackgroundRemovalApi(QStringLiteral("http://127.0.0.1:8765/removebg"), QString());
    m_pipeline->setLivePortraitApi(QStringLiteral("http://127.0.0.1:7890"));
    m_pipeline->setVisionApi(QStringLiteral("http://localhost:11434/v1"),
                              QString(),
                              QStringLiteral("qwen2.5vl:3b"));   // 需要多模态模型（4GB 显存选 3b）
    m_pipeline->setPersonaExtractionEnabled(kEnablePersonaExtraction);

    connect(m_pipeline, &AssetPipelineManager::pipelineProgress,
            this, &PetWidget::onPipelineProgress);
    connect(m_pipeline, &AssetPipelineManager::pipelineCompleted,
            this, &PetWidget::onPipelineCompleted);
    connect(m_pipeline, &AssetPipelineManager::pipelineFailed,
            this, &PetWidget::onPipelineFailed);
}

// 【呼吸悬浮微动】用 QPropertyAnimation 对 pos 属性做正弦式缓动循环：
//   锚点 → 上浮 5px → 回到锚点，周期 3.2s，EasingCurve::InOutQuad，无限循环。
void PetWidget::setupBreathing()
{
    m_breathAnim = new QPropertyAnimation(this, "pos", this);
    m_breathAnim->setLoopCount(-1);                        // 无限循环
    m_breathAnim->setDuration(3200);                       // 一个呼吸周期 3.2s
    m_breathAnim->setEasingCurve(QEasingCurve::InOutQuad); // 正弦式缓入缓出
}

// 以“当前所在位置”为新锚点重启呼吸动画（拖拽松手 / 首次显示时调用），
// 关键帧在 0.0 / 0.5 / 1.0 三点上取锚点与锚点-5px，保证循环首尾无缝衔接。
void PetWidget::restartBreathing()
{
    if (!m_breathAnim)
        return;
    m_breathAnim->stop();
    m_anchorPos = pos();
    m_breathAnim->setKeyValueAt(0.0, m_anchorPos);
    m_breathAnim->setKeyValueAt(0.5, m_anchorPos - QPoint(0, m_breathAmplitude));
    m_breathAnim->setKeyValueAt(1.0, m_anchorPos);
    m_breathAnim->start();
}

// ----------------------------------------------------------------------------
//  状态机
// ----------------------------------------------------------------------------

// 【动画状态机】切换状态：QMovie 模式下换片源 + 调播放速率；程序化模式下重绘。
void PetWidget::setState(PetState state)
{
    if (m_state == state)
        return;
    m_state = state;
    applyStateToMovie();   // 电影模式：切换片源 / 播放速率
    update();              // 程序化模式：立即重绘（电影模式下无副作用）
}

// 形象包中对应状态的片源（Thinking / Sleeping 不在包内，始终用内置素材）
QString PetWidget::packagePathForState(PetState state) const
{
    switch (state) {
    case PetWidget::Idle:    return m_pkgMedia[0];
    case PetWidget::Talking: return m_pkgMedia[1];
    case PetWidget::Happy:   return m_pkgMedia[2];
    default:                 return QString();
    }
}

// 依据当前状态解析片源并渲染。优先级：
//   形象包（AI 管线产物）> 用户拖入素材 > 内置 qrc 状态 GIF
// 动态格式（GIF/APNG/...）走 QMovie + 播放速率；
// 静态格式（管线降级产物）走 QPixmap 渲染——QMovie 播不了静态 PNG/JPEG。
void PetWidget::applyStateToMovie()
{
    if (!m_movie)
        return;

    QString file;
    const QString pkgPath = packagePathForState(m_state);
    if (!pkgPath.isEmpty()) {
        file = pkgPath;                        // 形象包优先
    } else if (!m_customMediaPath.isEmpty()) {
        file = m_customMediaPath;              // 用户素材：所有状态共用，仅调速
    } else {
        const QString perState =
            QStringLiteral(":/pets/%1.gif").arg(QString::fromLatin1(stateAssetName(m_state)));
        file = QFile::exists(perState) ? perState : QStringLiteral(":/pets/idle.gif");
    }

    if (file.isEmpty() || !QFile::exists(file)) {
        // 片源失效：整体回退到程序化绘制
        qWarning() << "[AniDesk] 片源失效，回退程序化绘制:" << file;
        m_movie->stop();
        m_mediaMode = MediaMode::None;
        m_fallbackTimer.start();
        update();
        return;
    }

    if (!isAnimatedMediaFile(file)) {
        // ---- 静态形象：QPixmap 渲染（QMovie 不支持静态 PNG/JPEG）----
        // 形象由 paintEvent 直接绘制并叠加“呼吸浮动 + 轻微摇摆”的微动作，
        // 因此窗口比图片略大一点，给动作预留空间。
        const QPixmap pm(file);
        if (pm.isNull()) {
            qWarning() << "[AniDesk] 无法解码形象，回退程序化绘制:" << file;
            m_movie->stop();
            m_mediaMode = MediaMode::None;
            m_fallbackTimer.start();
            update();
            return;
        }
        if (m_currentMovieFile != file) {      // 仅在片源变化时重载
            m_currentMovieFile = file;
            m_movie->stop();
            const QSize core = fitSize(pm.size(), kMaxPetSize, kMaxPetSize);
            const QSize target = core + QSize(24, 20);   // 微动作预留空间
            m_skinPixmap = pm.scaled(core, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
            applyWidgetSize(target);
        }
        m_mediaMode = MediaMode::StaticImage;
        m_fallbackTimer.start();               // 驱动微动作重绘（约 30fps）
        return;
    }

    // ---- 动态形象：QMovie 渲染（帧经 frameChanged 存入 m_skinPixmap）----
    if (m_currentMovieFile != file) {          // 仅在片源变化时重载，避免闪烁
        m_currentMovieFile = file;
        m_movie->setFileName(file);
        m_movie->start();
    }
    m_movie->setSpeed(speedForState(m_state)); // 调整播放速率（百分比）
    if (m_mediaMode != MediaMode::Movie) {     // 从静态/兜底模式切回电影模式
        m_mediaMode = MediaMode::Movie;
        m_skinPixmap = QPixmap();
        m_fallbackTimer.stop();
    }
}

// 载入 AI 管线产出的角色资产包（第二步 → 第一步的交接点）
void PetWidget::applyAssetPackage(const PetAssetPackage &pkg)
{
    m_petName = pkg.name;
    m_greeting = pkg.greeting;
    m_greetingUsed = true;                     // 管线完成时已展示过打招呼

    m_pkgMedia[0] = pkg.idleGifPath;
    m_pkgMedia[1] = pkg.talkingGifPath;
    m_pkgMedia[2] = pkg.happyGifPath;

    // 退出“用户临时素材”模式，进入形象包片源体系
    m_customMediaPath.clear();
    m_currentMovieFile.clear();                // 强制重载片源

    // 人设注入 LLM（LLMService 会自动追加情绪标签指令）
    if (m_llm)
        m_llm->setSystemPrompt(pkg.systemPrompt);

    // 注意：不能用 setState(Idle) 触发重载——管线结束时状态本就是 Idle，
    // 会被 setState 的去重守卫直接返回，导致“变身完成但形象不更新”。
    // 这里直接强制按新片源重载：
    applyStateToMovie();
    update();
    m_pendingIdle = false;
}

// ----------------------------------------------------------------------------
//  媒体加载
// ----------------------------------------------------------------------------

// 直接套用本地媒体（GIF/APNG 动图，或静态图——静态图自带呼吸微动作）
void PetWidget::loadMedia(const QString &path)
{
    loadMedia(path, /*asCustomSkin=*/true);
}

void PetWidget::loadMedia(const QString &path, bool asCustomSkin)
{
    if (!QFile::exists(path)) {
        qWarning() << "[AniDesk] 媒体文件不存在:" << path;
        return;
    }
    QImageReader probe(path);
    if (!probe.canRead()) {
        qWarning() << "[AniDesk] 无法解码媒体文件:" << path;
        return;
    }

    m_skinPixmap = QPixmap();
    m_fallbackTimer.stop();
    m_customMediaPath = asCustomSkin ? path : QString();
    m_currentMovieFile.clear();            // 强制重载片源
    applyStateToMovie();                   // 内部按文件类型自动走 QMovie/QPixmap
    update();
    qInfo() << "[AniDesk] 已加载形象:" << path << (asCustomSkin ? "(自定义)" : "(内置)");
}

// QMovie 解码出首帧后回调：让窗口尺寸贴合动画帧尺寸
void PetWidget::onMovieResized(const QSize &size)
{
    if (m_mediaMode == MediaMode::Movie)
        applyWidgetSize(size);
}

// 统一调整窗口尺寸，并让气泡重新吸附
void PetWidget::applyWidgetSize(const QSize &content)
{
    const QSize s = fitSize(content, kMaxPetSize, kMaxPetSize);
    setFixedSize(s);
    updateBubbleAnchor();
}

// ----------------------------------------------------------------------------
//  无边框拖拽（左键按住平滑拖动）
// ----------------------------------------------------------------------------

void PetWidget::mousePressEvent(QMouseEvent *event)
{
    poke();   // 任何互动都唤醒宠物并重置入睡倒计时

    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        // 记录按下点相对窗口左上角的偏移，保证拖拽不跳变
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        // 暂停呼吸动画，避免它与拖拽争抢 pos 属性
        m_breathAnim->pause();
        event->accept();
    }
    // 右键等其余按键交给 contextMenuEvent 处理
}

void PetWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragOffset);  // 平滑跟随
        event->accept();
    }
}

void PetWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        restartBreathing();    // 以松手位置为新锚点恢复呼吸悬浮
        event->accept();
    }
}

// 【气泡触发】双击：开心一下 + 弹出头顶气泡说一句台词
void PetWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    poke();
    showBubble();              // 先清空并开始打字机播放台词
    setState(Happy);           // 随后进入 Happy（台词播完会自动回到 Idle）
    event->accept();
}

// 【气泡触发】右键：同样弹出头顶气泡（按需求约定）
void PetWidget::contextMenuEvent(QContextMenuEvent *event)
{
    poke();
    showBubble();
    event->accept();
}

// ----------------------------------------------------------------------------
//  拖放：动图直接换肤；静态图进入 AI 形象生成管线
// ----------------------------------------------------------------------------

void PetWidget::dragEnterEvent(QDragEnterEvent *event)
{
    // 仅当拖入的是本地受支持的图片文件时才接受（给出“可放置”的视觉反馈）
    if (event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty()
        && event->mimeData()->urls().first().isLocalFile()
        && isSupportedImageFile(event->mimeData()->urls().first().toLocalFile())) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void PetWidget::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls() || event->mimeData()->urls().isEmpty()) {
        event->ignore();
        return;
    }

    const QString file = event->mimeData()->urls().first().toLocalFile();
    if (!isSupportedImageFile(file)) {
        event->ignore();
        return;
    }
    poke();

    const QString suffix = QFileInfo(file).suffix().toLower();
    if (suffix == QStringLiteral("gif") || suffix == QStringLiteral("apng")
        || suffix == QStringLiteral("mng")) {
        loadMedia(file);       // 动图：直接套用（第一步行为）
    } else {
        // 静态图：进入 AI 形象生成管线（第二步）
        m_bubble->appendToken(
            QStringLiteral("\n——— 收到「%1」，开始生成新形象 ———\n").arg(QFileInfo(file).fileName()));
        updateBubbleAnchor();
        m_bubble->popup();
        m_pipeline->processImage(file);
        updateBubbleHold();    // 管线运行期间保持气泡
    }
    event->acceptProposedAction();
}

// ----------------------------------------------------------------------------
//  布局联动：气泡跟随
// ----------------------------------------------------------------------------

void PetWidget::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    updateBubbleAnchor();      // 呼吸微动 / 拖拽时气泡实时跟随
}

void PetWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateBubbleAnchor();
}

void PetWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 首次显示时才启动呼吸动画（此时 pos 已被 main 定位到右下角）
    if (!m_lifeStarted) {
        m_lifeStarted = true;
        restartBreathing();
        m_sleepTimer.start();
    }
}

void PetWidget::updateBubbleAnchor()
{
    if (m_bubble)
        m_bubble->followAbove(this);
}

// LLM 思考/推理期间（可能长达数十秒）与管线运行期间，气泡不能被 20s 超时自动关闭，
// 否则 onBubbleClosed 会把还在等待回复的流取消掉。
void PetWidget::updateBubbleHold()
{
    if (m_bubble)
        m_bubble->setBusyHold((m_llm && m_llm->isBusy()) || (m_pipeline && m_pipeline->isRunning()));
}

// ----------------------------------------------------------------------------
//  气泡交互（第三步：LLM 流式对话回路）
// ----------------------------------------------------------------------------

// 会话记录模式：气泡不再逐轮清空，完整上下文滚动显示；
// 仅“清空”按钮（clearConversationRequested）会重置显示与模型记忆。
void PetWidget::showBubble()
{
    QString phrase;
    if (!m_greeting.isEmpty() && !m_greetingUsed) {
        phrase = m_greeting;       // 形象包的人设打招呼（首次使用）
        m_greetingUsed = true;
    } else {
        phrase = kIdlePhrases.at(QRandomGenerator::global()->bounded(kIdlePhrases.size()));
    }
    m_bubble->appendToken(QStringLiteral("\n【桌宠】%1").arg(phrase));

    updateBubbleAnchor();          // 显示前先吸附定位
    m_bubble->popup();
}

// 用户发送消息：命中动作关键词先播动作 → 会话区追加 → Thinking → 交给 LLMService
void PetWidget::onBubbleMessage(const QString &text)
{
    poke();

    // 对话触发动作：消息命中动作关键词（如“拔剑”“跳一下”）立即播放
    const QString action = matchActionKeyword(text);
    if (!action.isEmpty())
        startAction(action);

    m_bubble->appendToken(QStringLiteral("\n【你】%1\n").arg(text));
    setState(Thinking);            // 等待模型首个 token / 情绪信号
    m_llm->sendMessage(text);      // 全异步，token/emotion 经信号驱动
    updateBubbleHold();            // 思考/推理期间保持气泡
}

// LLM 流结束：等打字机把剩余文本播完后回到 Idle
void PetWidget::onConversationFinished()
{
    updateBubbleHold();            // 流已结束 → 解除忙碌保持
    if (m_state == Sleeping)
        return;
    m_pendingIdle = true;          // onTypingFinished 中复位
}

void PetWidget::onTypingFinished()
{
    if (m_pendingIdle) {
        m_pendingIdle = false;
        setState(Idle);
        return;
    }
    // Thinking 不在此复位：它由情绪信号 / conversationFinished 推进
    if (m_state == Talking || m_state == Happy)
        setState(Idle);
}

// 网络 / API 错误：气泡提示 + 复位状态
void PetWidget::onLlmError(const QString &errorMsg)
{
    updateBubbleHold();
    m_bubble->appendToken(QStringLiteral("\n⚠ %1").arg(errorMsg));
    setState(Idle);
}

// 气泡被关闭（Esc / 超时）：中断流式回复并复位
void PetWidget::onBubbleClosed()
{
    if (m_llm && m_llm->isBusy())
        m_llm->cancel();           // 流中止（半截回复不入历史）
    m_bubble->setBusyHold(false);
    m_pendingIdle = false;
    if (m_state != Sleeping)
        setState(Idle);
}

// ----------------------------------------------------------------------------
//  AI 资产生成管线（第二步）进度与产物
// ----------------------------------------------------------------------------

// 进度提示：气泡逐行追加“正在抠图... / 正在生成动画...”等加载文案
void PetWidget::onPipelineProgress(const QString &stepName, int percent)
{
    if (!m_bubble->isVisible()) {  // 气泡可能已被超时关闭 → 重新打开（不清空会话记录）
        updateBubbleAnchor();
        m_bubble->popup();
    }
    m_bubble->appendToken(QStringLiteral("\n· %1（%2%）").arg(stepName).arg(percent));
}

// 管线完成：换装 + 展示角色名与打招呼台词（会话记录模式：不清屏，加分隔线）
void PetWidget::onPipelineCompleted(const PetAssetPackage &package)
{
    updateBubbleHold();            // 管线结束 → 解除忙碌保持
    applyAssetPackage(package);

    m_bubble->appendToken(QStringLiteral("\n——— 变身！———\n我是「%1」～").arg(package.name));
    if (!package.greeting.isEmpty())
        m_bubble->appendToken(QStringLiteral("\n%1").arg(package.greeting));
    setState(Happy);               // 庆祝一下（打字机播完后回 Idle）
}

void PetWidget::onPipelineFailed(const QString &error)
{
    updateBubbleHold();
    m_bubble->appendToken(QStringLiteral("\n⚠ 形象生成失败：%1").arg(error));
}

// ----------------------------------------------------------------------------
//  互动 / 睡眠
// ----------------------------------------------------------------------------

// 任意交互入口：唤醒 + 重置入睡倒计时 + 保证气泡不被宠物压住
void PetWidget::poke()
{
    if (m_state == Sleeping)
        setState(Idle);            // 被唤醒
    m_sleepTimer.start();          // 重置 60s 倒计时
    if (m_bubble && m_bubble->isVisible())
        m_bubble->raise();
}

void PetWidget::fallAsleep()
{
    setState(Sleeping);
    if (m_bubble && m_bubble->isVisible()) {
        m_bubble->appendToken(QStringLiteral("\n【桌宠】Zzz……\n（摸摸我唤醒我）"));
    }
}

// ----------------------------------------------------------------------------
//  程序化绘制兜底（无任何素材时也能看到一只会动的“小团子”）
// ----------------------------------------------------------------------------

void PetWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    // 兜底程序化形象（无任何素材时）
    if (m_mediaMode == MediaMode::None) {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        drawFallbackPet(p);
        return;
    }
    if (m_skinPixmap.isNull())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const double t = m_animClock.elapsed() / 1000.0;

    // ---- 1) 呼吸微动作（静态形象明显；GIF 自带动作所以轻微）----
    const bool animatedSkin = (m_mediaMode == MediaMode::Movie);
    const double bob = (animatedSkin ? 2.0 : 5.0) * std::sin(t * 2.0);   // 上下浮动
    const double sway = (animatedSkin ? 0.0 : 3.0) * std::sin(t * 1.3);  // 左右轻摆
    double angle = (animatedSkin ? 0.0 : 2.0) * std::sin(t * 1.1);       // 轻微倾斜
    double dx = sway, dy = bob, sx = 1.0, sy = 1.0;

    // ---- 2) 对话触发的动作包络（叠加在呼吸之上）----
    if (m_actionTimer.isActive()) {
        double adx = 0, ady = 0, arot = 0, asx = 1, asy = 1;
        if (!actionEnvelope(m_actionClock.elapsed() / 1000.0,
                            &adx, &ady, &arot, &asx, &asy)) {
            m_actionTimer.stop();          // 动作播完
            m_actionName.clear();
        } else {
            dx += adx; dy += ady; angle += arot; sx *= asx; sy *= asy;
        }
    }

    // ---- 3) 以“脚底中心”为轴绘制（旋转/鞠躬绕脚底更自然）----
    const QPointF pivot(rect().width() / 2.0 + dx,
                        rect().height() - 8.0 + dy);
    p.translate(pivot);
    p.rotate(angle);
    p.scale(sx, sy);
    p.drawPixmap(QPointF(-m_skinPixmap.width() / 2.0,
                         -m_skinPixmap.height()), m_skinPixmap);
}

// ----------------------------------------------------------------------------
//  对话触发动作系统
// ----------------------------------------------------------------------------

// 播放一段动作（slash/jump/bow/nod/spin/dash/shake）
void PetWidget::startAction(const QString &name)
{
    if (name.isEmpty())
        return;
    m_actionName = name;
    m_actionClock.restart();
    m_actionTimer.start();
    qDebug() << "[AniDesk] 播放动作:" << name;
}

// 动作包络：输入动作开始以来的秒数，输出全身位移/旋转/缩放增量。
// 返回 false 表示动作已播完。所有动作都定义在“脚底中心”为轴的局部坐标系里。
bool PetWidget::actionEnvelope(double t, double *dx, double *dy,
                               double *rot, double *sx, double *sy) const
{
    if (m_actionName.isEmpty())
        return false;

    if (m_actionName == QLatin1String("slash")) {          // 拔剑：蓄力后拉 → 突刺 → 收势
        constexpr double D = 1.0;
        if (t >= D)
            return false;
        if (t < 0.25) {
            const double k = t / 0.25;
            *dx = -10 * k; *rot = -6 * k;
        } else if (t < 0.55) {
            const double k = easeOutCubic((t - 0.25) / 0.30);
            *dx = -10 + 30 * k; *rot = -6 + 14 * k;
            *sx = 1 + 0.06 * k; *sy = 1 - 0.02 * k;        // 突刺时轻微前倾拉伸
        } else {
            const double k = easeInOut((t - 0.55) / 0.45);
            *dx = 20 * (1 - k); *rot = 8 * (1 - k);
            *sx = 1.06 - 0.06 * k; *sy = 0.98 + 0.02 * k;
        }
        return true;
    }

    if (m_actionName == QLatin1String("jump")) {           // 连跳两下（抛物线）
        constexpr double D = 1.0;
        if (t >= D)
            return false;
        const double ph = std::fmod(t, 0.5) / 0.5;
        *dy = -26 * 4 * ph * (1 - ph);
        return true;
    }

    if (m_actionName == QLatin1String("bow")) {            // 鞠躬：绕脚底前倾→保持→还原
        constexpr double D = 1.4;
        if (t >= D)
            return false;
        if (t < 0.35)
            *rot = 24 * easeInOut(t / 0.35);
        else if (t < 0.85)
            *rot = 24;
        else
            *rot = 24 * (1 - easeInOut((t - 0.85) / 0.55));
        *dy = 3;
        return true;
    }

    if (m_actionName == QLatin1String("nod")) {            // 点头两下
        constexpr double D = 0.8;
        if (t >= D)
            return false;
        const double ph = std::fmod(t, 0.4) / 0.4;
        *dy = 8 * 4 * ph * (1 - ph);
        return true;
    }

    if (m_actionName == QLatin1String("spin")) {           // 转身（水平压缩翻转一圈）
        constexpr double D = 0.9;
        if (t >= D)
            return false;
        double c = std::cos(t / D * 2 * kPi);
        if (std::abs(c) < 0.12)                // 侧身瞬间保留最小厚度
            c = c < 0 ? -0.12 : 0.12;
        *sx = c;
        return true;
    }

    if (m_actionName == QLatin1String("dash")) {           // 冲刺出去再折返
        constexpr double D = 0.8;
        if (t >= D)
            return false;
        if (t < 0.4) {
            *dx = 24 * easeOutCubic(t / 0.4);
            *sx = 1 + 0.05 * (t / 0.4);
        } else {
            *dx = 24 * (1 - easeInOut((t - 0.4) / 0.4));
            *sx = 1 + 0.05 * (1 - (t - 0.4) / 0.4);
        }
        return true;
    }

    if (m_actionName == QLatin1String("shake")) {          // 摇头三下（幅度渐收）
        constexpr double D = 0.8;
        if (t >= D)
            return false;
        const double fade = 1 - t / D;
        *dx = 8 * std::sin(t / D * kPi * 3) * fade;
        *rot = 3 * std::sin(t / D * kPi * 3) * fade;
        return true;
    }

    return false;                              // 未知动作名
}

void PetWidget::drawFallbackPet(QPainter &p)
{
    const QRect area = rect();
    const double t = m_animClock.elapsed() / 1000.0;   // 时间相位（秒）
    QPointF center = QRectF(area).center();
    double rx = area.width() * 0.34;
    double ry = area.height() * 0.30;

    // ---- 状态驱动的身体运动 ----
    double bob = 0.0, sway = 0.0;
    switch (m_state) {
    case Talking:  bob = 2.5 * std::sin(t * 7.0);                  break; // 快速轻颤
    case Thinking: sway = 6.0 * std::sin(t * 1.6);                 break; // 缓慢摇晃
    case Happy:    bob = -14.0 * std::abs(std::sin(t * 5.0));      break; // 开心蹦跳
    case Sleeping:                                                      // 深睡呼吸
        ry *= 1.0 + 0.05 * std::sin(t * 1.3);
        rx *= 1.0 - 0.03 * std::sin(t * 1.3);
        break;
    case Idle:
    default:       bob = 3.0 * std::sin(t * 2.2);                  break; // 平静呼吸
    }
    center += QPointF(sway, bob);

    // ---- 状态主题色 ----
    QColor body(111, 199, 255);
    switch (m_state) {
    case Talking:  body = QColor(255, 183, 77);  break;
    case Thinking: body = QColor(179, 157, 219); break;
    case Happy:    body = QColor(255, 213, 79);  break;
    case Sleeping: body = QColor(144, 164, 174); break;
    case Idle:
    default:                                     break;
    }

    // ---- 身体（径向渐变 + 描边）----
    QRadialGradient grad(center + QPointF(-rx * 0.35, -ry * 0.45), rx * 1.7);
    grad.setColorAt(0.0, body.lighter(135));
    grad.setColorAt(1.0, body.darker(122));
    p.setPen(QPen(body.darker(165), 2.5));
    p.setBrush(grad);
    p.drawEllipse(center, rx, ry);

    // ---- 眼睛（周期性眨眼；睡觉时闭眼弧线）----
    const double eyeY = center.y() - ry * 0.30;
    const double eyeDx = rx * 0.44;
    const double blinkCycle = std::fmod(t, 3.4);
    const bool blinking = (m_state != Sleeping) && (blinkCycle > 3.22);

    if (m_state == Sleeping) {
        p.setPen(QPen(QColor(45, 55, 66), 3.0, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        for (int sx = -1; sx <= 1; sx += 2) {
            const double ex = center.x() + sx * eyeDx;
            // 下弯弧线 = 闭眼（Qt 角度：正方向逆时针，底部为 270°）
            p.drawArc(QRectF(ex - 10, eyeY - 8, 20, 16), 180 * 16, 180 * 16);
        }
    } else if (blinking) {
        p.setPen(QPen(QColor(45, 55, 66), 3.0, Qt::SolidLine, Qt::RoundCap));
        for (int sx = -1; sx <= 1; sx += 2) {
            const double ex = center.x() + sx * eyeDx;
            p.drawLine(QPointF(ex - 9, eyeY), QPointF(ex + 9, eyeY));
        }
    } else {
        QPointF pupilLook(0.0, 1.0);
        if (m_state == Thinking)
            pupilLook = QPointF(3.0, -3.0);    // 思考时望向右上方
        for (int sx = -1; sx <= 1; sx += 2) {
            const double ex = center.x() + sx * eyeDx;
            p.setPen(QPen(QColor(45, 55, 66), 2.0));
            p.setBrush(Qt::white);
            p.drawEllipse(QPointF(ex, eyeY), 10.5, ry * 0.24);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(40, 50, 60));
            p.drawEllipse(QPointF(ex + pupilLook.x(), eyeY + pupilLook.y()), 4.6, 4.6);
        }
    }

    // ---- 嘴（按状态变化）----
    const QPointF mouthPos(center.x(), center.y() + ry * 0.28);
    p.setPen(QPen(QColor(45, 55, 66), 3.0, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    switch (m_state) {
    case Talking: {
        const double open = 2.0 + 6.0 * std::abs(std::sin(t * 10.0));  // 快速张合
        p.setBrush(QColor(96, 44, 50));
        p.drawEllipse(mouthPos, 8.0, open);
        break;
    }
    case Happy:    // 大笑弧线
        p.drawArc(QRectF(mouthPos.x() - 14, mouthPos.y() - 10, 28, 18), 210 * 16, 120 * 16);
        break;
    case Sleeping:
        p.setBrush(QColor(96, 44, 50));
        p.drawEllipse(mouthPos, 3.0, 2.0);
        break;
    case Thinking: // 抿嘴
        p.drawLine(mouthPos + QPointF(-8, 0), mouthPos + QPointF(6, 0));
        break;
    case Idle:
    default:       // 微笑弧线
        p.drawArc(QRectF(mouthPos.x() - 11, mouthPos.y() - 7, 22, 14), 210 * 16, 120 * 16);
        break;
    }

    // ---- 腮红 ----
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 130, 145, 90));
    for (int sx = -1; sx <= 1; sx += 2)
        p.drawEllipse(QPointF(center.x() + sx * rx * 0.62, center.y() + ry * 0.10),
                      8.5, 5.5);

    // ---- Thinking：头顶冒出渐隐的思考泡泡 ----
    if (m_state == Thinking) {
        for (int i = 0; i < 3; ++i) {
            const double ph = std::fmod(t * 1.4 + i * 0.35, 1.0);
            const double alpha = 235.0 * std::max(0.0, 1.0 - ph);
            const double r = 3.0 + i * 1.7;
            const QPointF pos(center.x() + rx * 0.55 + i * 9.0,
                              center.y() - ry - 14.0 - i * 9.0 - ph * 5.0);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(235, 232, 255, int(alpha)));
            p.drawEllipse(pos, r, r);
        }
    }

    // ---- Sleeping：飘起的 “Z z” ----
    if (m_state == Sleeping) {
        for (int i = 0; i < 2; ++i) {
            const double ph = std::fmod(t * 0.45 + i * 0.5, 1.0);
            const double fade = ph < 0.2 ? ph / 0.2 : (1.0 - ph) / 0.8;
            const double s = 8.0 + i * 3.0 + 4.0 * ph;
            const QPointF topLeft(center.x() + rx * 0.45 + i * 16.0,
                                  center.y() - ry - 10.0 - ph * 30.0 - i * 8.0);
            p.setPen(QPen(QColor(240, 246, 255, int(220 * fade)), 2.6,
                          Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawLine(topLeft, topLeft + QPointF(s, 0));
            p.drawLine(topLeft + QPointF(s, 0), topLeft + QPointF(0, s));
            p.drawLine(topLeft + QPointF(0, s), topLeft + QPointF(s, s));
        }
    }

    // ---- Happy：环绕的金色星光 ----
    if (m_state == Happy) {
        for (int i = 0; i < 4; ++i) {
            const double ph = std::fmod(t * 1.3 + i * 0.25, 1.0);
            const double ang = ph * 2.0 * kPi + i * 1.57;
            const QPointF pos(center.x() + (rx + 16.0) * std::cos(ang),
                              center.y() - (ry + 10.0) * std::abs(std::sin(ang)) - 6.0);
            const double a = 210.0 * (1.0 - ph);
            p.setPen(QPen(QColor(255, 236, 140, int(a)), 2.4, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(pos + QPointF(-4.5, 0), pos + QPointF(4.5, 0));
            p.drawLine(pos + QPointF(0, -4.5), pos + QPointF(0, 4.5));
        }
    }
}
