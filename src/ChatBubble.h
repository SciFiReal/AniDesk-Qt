#pragma once
// ============================================================================
//  ChatBubble —— 挂载在宠物头顶的聊天气泡（独立顶层窗口）
//
//  特性：
//    · Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
//      以 PetWidget 为父窗口，因此永远悬浮在宠物之上且不进任务栏；
//    · 圆角半透明深色卡片（QSS）+ 投影（QGraphicsDropShadowEffect）；
//    · followAbove() 将气泡贴附到宠物头顶，并随宠物移动 / 避开屏幕边缘；
//    · appendToken() 流式“打字机”逐字追加文本（自动 HTML 转义，支持换行）；
//    · 底部 QLineEdit + 发送按钮：回车即发送；Esc 关闭气泡；20s 无操作自动隐藏。
// ============================================================================

#include <QWidget>
#include <QTimer>

class QLineEdit;
class QPushButton;
class QTextBrowser;

class ChatBubble : public QWidget
{
    Q_OBJECT

public:
    explicit ChatBubble(QWidget *parent = nullptr);

    QString text() const;                     // 当前已打出的文本

public slots:
    void appendToken(const QString &text);    // 流式打字机追加（可多次调用拼接）
    void clearText();                         // 清空文本并停止打字机
    void followAbove(const QWidget *pet);     // 吸附到 pet 头顶
    void popup();                             // 显示气泡并置顶、聚焦输入框
    void setBusyHold(bool on);                // 忙碌保持：LLM/管线工作期间禁止自动隐藏

signals:
    void messageSent(const QString &text);    // 用户发送了一条消息
    void typingFinished();                    // 打字机播放完毕
    void geometryDirtied();                   // 气泡尺寸变化，宿主需重新吸附
    void bubbleClosed();                      // 气泡被关闭（Esc / 超时自动隐藏）
    void clearConversationRequested();        // 用户点击“清空”：清空显示与模型记忆

protected:
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private slots:
    void revealNextChar();                    // 打字机节拍：每 tick 吐出若干字符
    void onSendClicked();                     // 发送按钮 / 回车

private:
    void rebuildDisplayText();                // 依据已显示文本重建富文本并自适应高度
    void updateSize();                        // 手动布局（卡片 + 投影留白）
    void scheduleAutoHide();                  // 重置“无操作自动隐藏”倒计时

    QWidget    *m_card = nullptr;             // 圆角半透明卡片（投影作用于它）
    QTextBrowser *m_textDisplay = nullptr;      // 会话记录区（滚动显示完整上下文）
    QLineEdit  *m_inputEdit = nullptr;        // 下方：输入框
    QPushButton *m_sendButton = nullptr;      // 下方：发送按钮
    QPushButton *m_clearButton = nullptr;     // 下方：清空按钮（清显示 + 清记忆）

    QString m_shown;                          // 已打出的文本
    QString m_pending;                        // 待打出的字符队列
    QTimer  m_typeTimer;                      // 打字机节拍定时器
    QTimer  m_autoHideTimer;                  // 无操作自动隐藏定时器
    bool    m_busyHold = false;               // LLM/管线忙碌期间暂停自动隐藏
};
