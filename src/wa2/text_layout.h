// text_layout.h — 对话框像素换行、自适应字号与逐字显示映射（无 SDL 依赖）
#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace wa2 {

// 一行只保存可见字符。rawCharIndices 记录每个可见字形在原始 UTF-8 码点流
// 中的位置；软换行被重排进同一行时，逐字显示仍能正确跨过控制符。
struct DialogueTextLine {
    std::string text;
    int firstChar = 0;
    int charCount = 0;
    int pixelWidth = 0;
    std::vector<int> rawCharIndices;
};

struct DialogueTextLayout {
    int fontSize = 28;
    int columns = 28;
    int lineAdvance = 41;
    int rawCharCount = 0;
    int maxLineWidth = 0;
    bool fits = true;
    std::vector<DialogueTextLine> lines;

    int PixelHeight() const {
        if (lines.empty()) return 0;
        return ((int)lines.size() - 1) * lineAdvance + fontSize;
    }
};

enum class DialogueNewlinePolicy {
    Preserve,
    // 普通对白脚本里的单个换行是按旧 28 字格预排的软提示。重新按当前 UI
    // 宽度排版时忽略它；连续两个以上换行仍作为显式段落边界。
    ReflowSingle,
};

inline size_t DialogueUtf8Next(const std::string& text, size_t i) {
    const unsigned char c = (unsigned char)text[i];
    const size_t bytes = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return std::min(i + bytes, text.size());
}

template <typename MeasureGlyph>
inline DialogueTextLayout WrapDialogueTextMeasured(
        const std::string& text,
        int fontSize,
        int bodyWidth,
        DialogueNewlinePolicy newlinePolicy,
        MeasureGlyph measureGlyph,
        int referenceFontSize = 28,
        int referenceParagraphSpacing = 13) {
    DialogueTextLayout result;
    result.fontSize = std::max(1, fontSize);
    result.columns = std::max(1, bodyWidth / result.fontSize);
    const int scaledSpacing = std::max(
        0, (referenceParagraphSpacing * result.fontSize + referenceFontSize / 2) /
               referenceFontSize);
    result.lineAdvance = result.fontSize + scaledSpacing;

    DialogueTextLine line;
    auto finishLine = [&]() {
        if (line.charCount <= 0) return;
        result.maxLineWidth = std::max(result.maxLineWidth, line.pixelWidth);
        result.lines.push_back(std::move(line));
        line = DialogueTextLine();
    };

    int rawIndex = 0;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\n') {
            size_t next = i;
            int newlineCount = 0;
            while (next < text.size() && text[next] == '\n') {
                ++next;
                ++newlineCount;
                ++rawIndex;
            }

            const bool hardBreak = newlinePolicy == DialogueNewlinePolicy::Preserve ||
                                   newlineCount >= 2;
            // Preserve 与参考 Wa2Label 一致：行首/连续换行不制造空白行。
            // ReflowSingle 只取消旧字格产生的单换行，保留真正的段落分隔。
            if (hardBreak) finishLine();
            i = next;
            continue;
        }

        const size_t next = DialogueUtf8Next(text, i);
        const std::string glyph = text.substr(i, next - i);
        const int glyphWidth = std::max(0, measureGlyph(glyph, result.fontSize));
        if (line.charCount > 0 && line.pixelWidth + glyphWidth > bodyWidth)
            finishLine();

        if (line.charCount == 0) line.firstChar = rawIndex;
        line.text += glyph;
        line.pixelWidth += glyphWidth;
        line.rawCharIndices.push_back(rawIndex);
        ++line.charCount;
        ++rawIndex;
        i = next;
    }
    finishLine();
    result.rawCharCount = rawIndex;
    return result;
}

// F2 的参考固定字格入口保持不变，确保旧诊断版本仍可复现。
inline DialogueTextLayout WrapDialogueText(const std::string& text,
                                           int fontSize,
                                           int bodyWidth,
                                           int referenceFontSize = 28,
                                           int referenceParagraphSpacing = 13) {
    auto fixedCell = [](const std::string&, int size) { return size; };
    return WrapDialogueTextMeasured(text, fontSize, bodyWidth,
                                    DialogueNewlinePolicy::Preserve, fixedCell,
                                    referenceFontSize, referenceParagraphSpacing);
}

// 正常对白优先保持参考字号 28。只有完整段落连同字体阴影会触及窗口边框时，
// 才以 2px 为一级缩小；16px 是异常超长文本的可读性下限。
inline DialogueTextLayout FitDialogueText(const std::string& text,
                                          int bodyWidth,
                                          int bodyHeight,
                                          int defaultFontSize = 28,
                                          int minimumFontSize = 16) {
    DialogueTextLayout smallest;
    for (int size = defaultFontSize; size >= minimumFontSize; size -= 2) {
        DialogueTextLayout candidate = WrapDialogueText(text, size, bodyWidth);
        candidate.fits = candidate.PixelHeight() <= bodyHeight;
        if (candidate.fits) return candidate;
        smallest = std::move(candidate);
    }
    smallest.fits = false;
    return smallest;
}

template <typename MeasureGlyph>
inline DialogueTextLayout FitDialogueTextMeasured(
        const std::string& text,
        int bodyWidth,
        int bodyHeight,
        DialogueNewlinePolicy newlinePolicy,
        MeasureGlyph measureGlyph,
        int defaultFontSize = 28,
        int minimumFontSize = 16) {
    DialogueTextLayout smallest;
    for (int size = defaultFontSize; size >= minimumFontSize; size -= 2) {
        DialogueTextLayout candidate = WrapDialogueTextMeasured(
            text, size, bodyWidth, newlinePolicy, measureGlyph);
        candidate.fits = candidate.PixelHeight() <= bodyHeight &&
                         candidate.maxLineWidth <= bodyWidth;
        if (candidate.fits) return candidate;
        smallest = std::move(candidate);
    }
    smallest.fits = false;
    return smallest;
}

inline int DialogueLineVisibleChars(const DialogueTextLine& line, int shown) {
    if (shown < 0) return line.charCount;
    if (!line.rawCharIndices.empty()) {
        return (int)(std::lower_bound(line.rawCharIndices.begin(),
                                     line.rawCharIndices.end(), shown) -
                     line.rawCharIndices.begin());
    }
    return std::clamp(shown - line.firstChar, 0, line.charCount);
}

// SetMessage(v3=0) 在参考实现中追加的是“\\k + text”，也就是新的一页，
// 不是把 text 拼进当前页。返回第一个新增页的索引，供引擎立即显示它。
inline int AppendDialoguePages(const std::string& text,
                               std::string& combined,
                               std::vector<std::string>& pages) {
    const int firstNewPage = (int)pages.size();
    if (pages.empty()) combined = text;
    else combined += "\\k" + text;

    size_t start = 0;
    while (true) {
        const size_t split = text.find("\\k", start);
        if (split == std::string::npos) {
            pages.push_back(text.substr(start));
            break;
        }
        pages.push_back(text.substr(start, split - start));
        start = split + 2;
    }
    return firstNewPage;
}

} // namespace wa2
