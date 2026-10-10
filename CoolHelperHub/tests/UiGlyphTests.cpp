#include "UiGlyphSeed.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <cstdlib>
#include <iostream>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void CheckText(ImFont* font, const char* text) {
    while (*text) {
        unsigned int codepoint = 0;
        const int length = ImTextCharFromUtf8(&codepoint, text, nullptr);
        Check(length > 0 && codepoint != 0xFFFD, "Invalid UTF-8 in UI glyph seed");
        Check(font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) != nullptr,
            "UI character fell back to a missing glyph");
        text += length;
    }
}

void CheckAtlas(float scale) {
    ImFontAtlas atlas;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(atlas.GetGlyphRangesChineseSimplifiedCommon());
    builder.AddText(coolhelper::kStaticUiGlyphSeed);
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);
    ImFontConfig config;
    config.OversampleH = config.OversampleV = 2;
    const auto mergeSymbols = [&atlas](float size) {
        static constexpr ImWchar symbols[] = {
            0x00B1, 0x00B1, 0x00D7, 0x00D7, 0x00F7, 0x00F7,
            0x0391, 0x03C9, 0x2000, 0x2BFF, 0
        };
        ImFontConfig symbolConfig;
        symbolConfig.MergeMode = true;
        symbolConfig.OversampleH = symbolConfig.OversampleV = 2;
        Check(atlas.AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf",
            size, &symbolConfig, symbols) != nullptr, "Symbol font merge failed");
    };
    ImFont* regular = atlas.AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc",
        18.0f * scale, &config, ranges.Data);
    Check(regular != nullptr, "Regular Windows font unavailable");
    mergeSymbols(18.0f * scale);
    ImFont* title = atlas.AddFontFromFileTTF("C:\\Windows\\Fonts\\msyhbd.ttc",
        19.0f * scale, &config, ranges.Data);
    Check(title != nullptr, "Bold Windows font unavailable");
    mergeSymbols(19.0f * scale);
    ImFont* code = atlas.AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf",
        16.0f * scale, &config, atlas.GetGlyphRangesDefault());
    Check(regular && title && code, "Required Windows fonts unavailable");
    config.MergeMode = true;
    Check(atlas.AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc",
        16.0f * scale, &config, ranges.Data) != nullptr, "Code CJK font merge failed");
    mergeSymbols(16.0f * scale);
    Check(atlas.Build(), "UI font atlas build failed");
    Check(atlas.TexWidth <= 16384 && atlas.TexHeight <= 16384,
        "UI font atlas exceeds D3D11 texture limits");
    for (ImFont* font : {regular, title, code}) {
        // Covers every statically collected character, not just this regression.
        CheckText(font, coolhelper::kStaticUiGlyphSeed);
        CheckText(font, "兼容默认仅主屏所有屏幕显示目标已同步到覆盖层");
    }
    std::cout << "UI glyph coverage OK at scale " << scale << ": "
        << atlas.TexWidth << 'x' << atlas.TexHeight << '\n';
}
}

int main() {
    CheckAtlas(1.0f);
    CheckAtlas(1.5f);
    CheckAtlas(2.0f);
    return 0;
}
