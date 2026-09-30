#include "ChatBubble.h"

#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QScreen>
#include <QShortcut>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

// 气泡 QSS：深色圆角半透明卡片 + 1px 浅色描边。
// 描边让卡片边界在任何背景上都清晰可辨（深色桌面/编辑器上不会误读为“被裁切”）。
const char kStyleSheet[] = R"(
#bubbleCard {
    background: rgba(30, 30, 40, 0.88);
    border: 1px solid rgba(255, 255, 255, 0.22);
    border-radius: 12px;
    color: white;
}
#bubbleText {
    color: white;
    background: transparent;
    border: none;
    font-size: 14px;
}
QLineEdit {
    background: rgba(255, 255, 255, 0.12);
    border: 1px solid rgba(255, 255, 255, 0.18);
    border-radius: 8px;
    padding: 4px 8px;
    color: white;
    font-size: 13px;
    selection-background-color: #3d7ecc;
}
QLineEdit:focus {
    border: 1px solid rgba(110, 180, 255, 0.85);
}
QPushButton {
    background: rgba(90, 168, 255, 0.92);
    color: white;
    border: none;
    border-radius: 8px;
    padding: 5px 16px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton:hover   { background: rgba(120, 190, 255, 0.95); }
QPushButton:pressed { background: rgba(70, 140, 220, 0.95); }
QPushButton#bubbleClear {
    background: rgba(255, 255, 255, 0.14);
    padding: 5px 10px;
    font-weight: 400;
}
QPushButton#bubbleClear:hover { background: rgba(255, 255, 255, 0.22); }
)";

constexpr int kCardWidth = 380;          // 卡片内容宽度（迷你聊天窗）
constexpr int kShadowMargin = 22;        // 窗口上/左/右留白：防止投影被窗口边界裁剪
constexpr int kShadowMarginBottom = 30;  // 底部留白更多（投影向下偏移）
constexpr int kGapAbovePet = 8;          // 气泡与宠物头顶的间距
constexpr int kTypeIntervalMs = 28;      // 打字机每个字符的间隔
constexpr int kAutoHideMs = 20000;       // 无操作 20s 后自动隐藏
constexpr int kMinBubbleHeight = 240;    // 气泡最小高度
constexpr int kMaxBubbleHeight = 520;    // 气泡最大高度（超出部分内部滚动）
constexpr int kBubbleChrome = 140;       // 窗口高度扣除会话区后的装饰开销
                                         // （投影留白 52 + 卡片内边距 28 + 输入行 ~40 + 间距 8 + 富余）
constexpr int kMaxLogChars = 8000;       // 会话记录字符上限（防止无限增长拖慢渲染）

} // namespace

// ----------------------------------------------------------------------------

ChatBubble::ChatBubble(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
{
    setAttribute(Qt::WA_TranslucentBackground);      // 顶层窗口背景全透明
    setObjectName(QStringLiteral("bubbleRoot"));
    setStyleSheet(QString::fromUtf8(kStyleSheet));

    // 圆角卡片：真正可见的主体；投影效果挂在其上。
    // 窗口布局留出四周留白，保证投影完整渲染。
    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("bubbleCard"));
    m_card->setAttribute(Qt::WA_StyledBackground, true); // 普通 QWidget 绘制 QSS 背景所必需

    // 上方：会话记录区（QTextBrowser 可滚动、可选中复制）
    m_textDisplay = new QTextBrowser(m_card);
    m_textDisplay->setObjectName(QStringLiteral("bubbleText"));
    m_textDisplay->setReadOnly(true);
    m_textDisplay->setOpenExternalLinks(false);
    m_textDisplay->setFrameShape(QFrame::NoFrame);
    m_textDisplay->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_textDisplay->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_textDisplay->setMinimumHeight(120);

    // 下方：输入行（回车即发送）+ 清空 + 发送
    m_inputEdit = new QLineEdit(m_card);
    m_inputEdit->setPlaceholderText(tr("跟它说点什么…（回车发送，Esc 关闭）"));
    connect(m_inputEdit, &QLineEdit::returnPressed, this, &ChatBubble::onSendClicked);
    connect(m_inputEdit, &QLineEdit::textEdited, this, &ChatBubble::scheduleAutoHide);

    m_sendButton = new QPushButton(m_card);
    m_sendButton->setText(tr("发送"));
    m_sendButton->setCursor(Qt::PointingHandCursor);
    connect(m_sendButton, &QPushButton::clicked, this, &ChatBubble::onSendClicked);

    m_clearButton = new QPushButton(m_card);
    m_clearButton->setObjectName(QStringLiteral("bubbleClear"));
    m_clearButton->setText(tr("清空"));
    m_clearButton->setToolTip(tr("清空对话记录与模型记忆"));
    m_clearButton->setCursor(Qt::PointingHandCursor);
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        clearText();
        emit clearConversationRequested();
    });

    // 卡片布局：会话记录（可伸缩）+ 输入行。
    // 用布局管理器精确排布（含 QSS padding 的实际渲染高度），
    // 替代早期的手动算术——那会导致输入行溢出卡片底边。
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_inputEdit, 1);
    row->addWidget(m_sendButton, 0);
    row->addWidget(m_clearButton, 0);

    auto *cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(14, 14, 14, 14);
    cardLayout->setSpacing(8);
    cardLayout->addWidget(m_textDisplay, 1);     // 会话区占据全部剩余空间
    cardLayout->addLayout(row);

    // 窗口布局：卡片 + 四周投影留白（底部留更多，投影向下偏移）
    auto *windowLayout = new QVBoxLayout(this);
    windowLayout->setContentsMargins(kShadowMargin, kShadowMargin,
                                     kShadowMargin, kShadowMarginBottom);
    windowLayout->addWidget(m_card);

    // 卡片投影（柔和下坠阴影）
    auto *shadow = new QGraphicsDropShadowEffect;
    shadow->setBlurRadius(26);
    shadow->setOffset(0, 5);
    shadow->setColor(QColor(0, 0, 0, 150));
    m_card->setGraphicsEffect(shadow);   // setGraphicsEffect 接管效果的所有权

    // 打字机节拍
    m_typeTimer.setInterval(kTypeIntervalMs);
    connect(&m_typeTimer, &QTimer::timeout, this, &ChatBubble::revealNextChar);

    // 无操作自动隐藏
    m_autoHideTimer.setSingleShot(true);
    m_autoHideTimer.setInterval(kAutoHideMs);
    connect(&m_autoHideTimer, &QTimer::timeout, this, &QWidget::hide);

    // Esc 关闭气泡（WindowShortcut 上下文，输入框获得焦点时同样生效）
    auto *escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escShortcut, &QShortcut::activated, this, &QWidget::hide);

    updateSize();   // 初始宽度/高度
}

QString ChatBubble::text() const
{
    return m_shown;
}

// 追加一段“流式 token”到待打出队列，打字机随后逐字显示。
// 可被连续调用（例如先 append("你说：xx\n") 再 append(回复)），队列会自动拼接。
void ChatBubble::appendToken(const QString &text)
{
    if (text.isEmpty())
        return;
    m_pending += text;                              // 入队待打出的字符
    if (!m_typeTimer.isActive())
        m_typeTimer.start();                        // 打字机未运行则启动
    scheduleAutoHide();
}

// 清空会话记录（“清空”按钮触发：显示与模型记忆一并清空）
void ChatBubble::clearText()
{
    const bool wasTyping = !m_pending.isEmpty();
    m_typeTimer.stop();
    m_pending.clear();
    m_shown.clear();
    rebuildDisplayText();
    if (wasTyping)
        emit typingFinished();
}

// 吸附到宠物头顶：水平居中，垂直位于宠物上方 kGapAbovePet 处，
// 并夹取到宠物所在屏幕的可用区域内（顶部放不下时改为贴到宠物下方），
// 左右各留 8px 边距，避免按钮贴着屏幕边缘显得被裁切。
void ChatBubble::followAbove(const QWidget *pet)
{
    if (!pet)
        return;
    const QRect petFrame = pet->frameGeometry();    // 顶层窗口的全局几何
    int x = petFrame.center().x() - width() / 2;
    int y = petFrame.top() - height() - kGapAbovePet;

    QScreen *screen = QGuiApplication::screenAt(petFrame.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        constexpr int kScreenEdgeMargin = 8;
        if (y < avail.top())                        // 头顶空间不足 → 贴到宠物下方
            y = petFrame.bottom() + kGapAbovePet;
        x = qBound(avail.left() + kScreenEdgeMargin, x,
                   qMax(avail.left() + kScreenEdgeMargin,
                        avail.right() - width() + 1 - kScreenEdgeMargin));
        y = qBound(avail.top(), y,
                   qMax(avail.top(), avail.bottom() - height() + 1));
    }
    move(x, y);
}

// 显示气泡：置顶、激活窗口并聚焦输入框，用户可以立刻打字。
void ChatBubble::popup()
{
    scheduleAutoHide();
    show();
    raise();
    activateWindow();
    m_inputEdit->setFocus();
}

// 宽度固定；高度在 [min, max] 区间内随会话内容伸缩：
// 内容高度由 QTextDocument 实测（含换行与富文本），超出上限时
// 会话区内部滚动。尺寸变化经 resizeEvent → geometryDirtied 通知宿主重新吸附。
void ChatBubble::updateSize()
{
    const int w = kShadowMargin * 2 + kCardWidth;
    setFixedWidth(w);

    const double docH = m_textDisplay->document()->size().height();
    const int h = qBound(kMinBubbleHeight, int(docH) + kBubbleChrome, kMaxBubbleHeight);
    setFixedHeight(h);
}

void ChatBubble::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 文本增多 → 气泡长高 → 通知宿主重新吸附（底边贴住宠物头顶，向上生长）
    emit geometryDirtied();
}

void ChatBubble::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    emit bubbleClosed();            // 通知宿主（PetWidget 据此中断流式回复并复位状态）
    if (!m_pending.isEmpty()) {     // 隐藏时丢弃未播完的文本，并通知宿主复位状态
        m_pending.clear();
        m_typeTimer.stop();
        emit typingFinished();
    }
    m_autoHideTimer.stop();
}

// 打字机节拍：每 tick 从待打队列取出字符并重建富文本。
void ChatBubble::revealNextChar()
{
    if (m_pending.isEmpty()) {
        m_typeTimer.stop();
        emit typingFinished();
        return;
    }
    // 积压超过 120 字符时一次吐出 2 个，避免长回复拖沓
    const int steps = m_pending.size() > 120 ? 2 : 1;
    for (int i = 0; i < steps && !m_pending.isEmpty(); ++i) {
        m_shown += m_pending.at(0);
        m_pending.remove(0, 1);
    }
    rebuildDisplayText();
    scheduleAutoHide();
}

void ChatBubble::onSendClicked()
{
    const QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty())
        return;
    m_inputEdit->clear();
    scheduleAutoHide();
    emit messageSent(text);
}

// 将会话记录转义为 HTML（换行 → <br/>），交给 QTextBrowser 渲染，
// 并滚动到最新消息。记录过长时从头裁剪，保证渲染性能。
void ChatBubble::rebuildDisplayText()
{
    if (m_shown.size() > kMaxLogChars)
        m_shown.remove(0, m_shown.size() - kMaxLogChars);

    QString escaped = m_shown.toHtmlEscaped();
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    m_textDisplay->setHtml(QStringLiteral(
        "<div style=\"line-height:140%\">%1</div>").arg(escaped));

    // 滚动到底部（setHtml 后滚动条范围下一轮事件循环才就绪）
    QTimer::singleShot(0, this, [this] {
        QScrollBar *bar = m_textDisplay->verticalScrollBar();
        bar->setValue(bar->maximum());
    });
    updateSize();                    // 文本变化 → 重算气泡高度
}

void ChatBubble::scheduleAutoHide()
{
    if (m_busyHold)
        return;                                // 忙碌期间不启动隐藏倒计时
    m_autoHideTimer.start();
}

// 忙碌保持：等待 LLM 首 token / 管线进度时，气泡不能被自动隐藏
//（否则 PetWidget::onBubbleClosed 会把还没开始回复的流取消掉）。
void ChatBubble::setBusyHold(bool on)
{
    if (m_busyHold == on)
        return;
    m_busyHold = on;
    if (m_busyHold) {
        m_autoHideTimer.stop();
    } else if (isVisible()) {
        m_autoHideTimer.start();
    }
}
