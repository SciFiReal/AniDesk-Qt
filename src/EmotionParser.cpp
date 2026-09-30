#include "EmotionParser.h"

#include <QRegularExpression>
#include <QStringList>
#include <QStringView>

namespace {

// 情绪标记正则：[EMOTION: xxx]，容忍大小写与多余空白
const QRegularExpression kEmotionRe(
    QStringLiteral("\\[\\s*EMOTION\\s*:\\s*(normal|happy|sad|angry|thinking)\\s*\\]"),
    QRegularExpression::CaseInsensitiveOption);

// 五种标记的“规范化形态”（小写、去空白），用于判断尾部是否为半截标签
const QStringList kCanonicalTags = {
    QStringLiteral("[emotion:normal]"),
    QStringLiteral("[emotion:happy]"),
    QStringLiteral("[emotion:sad]"),
    QStringLiteral("[emotion:angry]"),
    QStringLiteral("[emotion:thinking]"),
};

// 滞留长度上限：超过该长度仍匹配不上标签前缀的尾部，按普通文本放行
constexpr qsizetype kMaxTagLen = 32;

QString normalizeTagTail(const QString &tail)
{
    QString s = tail.toLower();
    s.remove(QLatin1Char(' '));
    s.remove(QLatin1Char('\t'));
    s.remove(QLatin1Char('\r'));
    s.remove(QLatin1Char('\n'));
    return s;
}

// 若 text 结尾是某个合法标签的“前缀”（可能尚未写完），返回应扣留的字符数
qsizetype partialTagSuffixLength(const QString &text)
{
    const qsizetype pos = text.lastIndexOf(QLatin1Char('['));
    if (pos < 0)
        return 0;
    const QString tail = normalizeTagTail(text.mid(pos));
    if (tail.isEmpty() || tail.size() > kMaxTagLen)
        return 0;
    for (const QString &tag : kCanonicalTags) {
        if (tag.startsWith(tail))
            return text.size() - pos;   // 扣留原始文本中的这半截
    }
    return 0;
}

} // namespace

// ----------------------------------------------------------------------------

QString EmotionParser::parseAndClean(const QString &rawChunk, QString &extractedEmotion)
{
    extractedEmotion.clear();
    if (rawChunk.isEmpty())
        return QString();

    m_pending += rawChunk;

    // 1) 找出本段缓冲中所有完整标记，逐个擦除
    QString out;
    qsizetype cursor = 0;
    QRegularExpressionMatchIterator it = kEmotionRe.globalMatch(m_pending);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += m_pending.mid(cursor, match.capturedStart() - cursor);
        if (extractedEmotion.isEmpty())
            extractedEmotion = match.captured(1).toLower();
        cursor = match.capturedEnd();
    }
    out += m_pending.mid(cursor);
    m_pending.clear();

    // 2) 尾部疑似半截标签 → 扣留等待下一个 token
    const qsizetype keep = partialTagSuffixLength(out);
    if (keep > 0) {
        m_pending = out.right(keep);
        out.chop(keep);
    }
    return out;
}

void EmotionParser::reset()
{
    m_pending.clear();
}

QString EmotionParser::flush()
{
    const QString out = m_pending;
    m_pending.clear();
    return out;
}
