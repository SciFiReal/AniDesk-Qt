// ============================================================================
//  EmotionParser —— 情绪标签提取与流式文本清洗器（第三步）
//
//  LLM 按约定会在回复中输出情绪标记，例如：
//      [EMOTION: happy] 今天也要元气满满哦～
//  本模块负责从【流式 token】中实时识别并剥离该标记：
//    · 标记可能被拆分到多个 token 中（如 "[EMO" + "TION: happy]"），
//      因此内部维护一个滞留缓冲区，凡疑似"未写完的标签尾巴"都会被扣留，
//      待后续 token 到达后再判定；
//    · 命中完整标记后，把标记值写入 extractedEmotion 并从文本中擦除；
//    · 返回值是清洗后的纯对话文字（可直接送入打字机）。
// ============================================================================
#pragma once

#include <QString>

class EmotionParser
{
public:
    explicit EmotionParser() = default;

    // 流式解析：处理一个原始 token（chunk），返回清洗后的文本。
    // 若本段中出现完整情绪标记，extractedEmotion 被赋值为标记值（"normal"/"happy"/"sad"/"angry"/"thinking"），
    // 否则清空。同一段中出现多个标记时只上报第一个（其余仍会被擦除）。
    QString parseAndClean(const QString &rawChunk, QString &extractedEmotion);

    void reset();        // 丢弃滞留缓冲（每轮新对话开始时调用）
    QString flush();     // 流结束时取出滞留缓冲中剩余的文本

private:
    QString m_pending;   // 滞留缓冲：可能是"半截情绪标签"的尾部文本
};
