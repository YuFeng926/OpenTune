// BlobGradientPreview.cpp — 离屏渲染单音符 blob，验证 OpenDyne 纵向渐变效果
// 用法: BlobGradientPreview [outputDir]
// 输出: blob_preview.bmp

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// ── 简易 BMP 写入（32bpp BGRA） ──────────────────────────────────
static bool writeBmp(const std::vector<uint8_t>& rgba, int w, int h, const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    const int stride = w * 4;
    const int imageSize = h * stride;
    const int fileSize = 54 + imageSize;

    // BMP header
    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    memcpy(header + 2, &fileSize, 4);
    uint32_t pixOff = 54; memcpy(header + 10, &pixOff, 4);
    uint32_t infoSize = 40; memcpy(header + 14, &infoSize, 4);
    int32_t negH = -h; memcpy(header + 18, &w, 4); memcpy(header + 22, &negH, 4);
    uint16_t planes = 1; memcpy(header + 26, &planes, 2);
    uint16_t bpp = 32; memcpy(header + 28, &bpp, 2);
    memcpy(header + 34, &imageSize, 4);

    fwrite(header, 1, 54, f);

    // 转换 ARGB → BGRA 并合成到黑色背景
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const int i = (y * w + x) * 4;
            const float a = rgba[i + 3] / 255.0f; // alpha
            const uint8_t r = static_cast<uint8_t>(rgba[i + 0] * a);
            const uint8_t g = static_cast<uint8_t>(rgba[i + 1] * a);
            const uint8_t b = static_cast<uint8_t>(rgba[i + 2] * a);
            uint8_t bgra[4] = { b, g, r, 255 };
            fwrite(bgra, 1, 4, f);
        }
    }
    fclose(f);
    return true;
}

// ── 颜色混合 ─────────────────────────────────────────────────────
struct Color { float r, g, b, a; };

static Color lerpColor(const Color& a, const Color& b, float t)
{
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
             a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

static Color brighter(const Color& c, float amount)
{
    return { c.r + (1.0f - c.r) * amount, c.g + (1.0f - c.g) * amount,
             c.b + (1.0f - c.b) * amount, c.a };
}

static Color darker(const Color& c, float amount)
{
    return { c.r * (1.0f - amount), c.g * (1.0f - amount),
             c.b * (1.0f - amount), c.a };
}

// ── 渐变采样：与 PianoRollRenderer.cpp 1019-1036 一致的纵向渐变 ──
static Color sampleBlobGradient(float y, float centerY, float halfH, const Color& displayColour)
{
    // 顶部纯色 → 中心极亮 → 底部纯色
    const float fillTopA = 0.60f;
    const float fillBotA = 0.56f;
    const float glowCoreA = 0.95f;
    const float glowMidA = 0.70f;

    Color fillTop    = brighter(displayColour, 0.35f);
    fillTop.a = fillTopA;
    Color fillBottom = darker(displayColour, 0.40f);
    fillBottom.a = fillBotA;
    Color glowCore   = brighter(displayColour, 0.75f);
    glowCore.a = glowCoreA;
    Color glowMid    = brighter(displayColour, 0.35f);
    glowMid.a = glowMidA;

    // 归一化 y 到 [0, 1]
    const float t = (y - (centerY - halfH)) / (2.0f * halfH);
    const float tc = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

    // 五段渐变：fillTop(0.0) → glowMid(0.25) → glowCore(0.50) → glowMid(0.75) → fillBottom(1.0)
    if (tc < 0.25f)
        return lerpColor(fillTop, glowMid, tc / 0.25f);
    if (tc < 0.50f)
        return lerpColor(glowMid, glowCore, (tc - 0.25f) / 0.25f);
    if (tc < 0.75f)
        return lerpColor(glowCore, glowMid, (tc - 0.50f) / 0.25f);
    return lerpColor(glowMid, fillBottom, (tc - 0.75f) / 0.25f);
}

int main(int argc, char* argv[])
{
    const char* outDir = (argc > 1) ? argv[1] : ".opencode/screenshots";

    constexpr int imgW = 800, imgH = 130;
    constexpr float centerY = 65.0f;
    constexpr float halfH = 32.0f;
    constexpr int x1 = 20, x2 = 780;

    struct TestCase { const char* name; Color color; };
    TestCase cases[] = {
        { "green",  { 0.298f, 0.686f, 0.314f, 1.0f } },
        { "blue",   { 0.129f, 0.588f, 0.953f, 1.0f } },
        { "orange", { 1.0f,   0.600f, 0.0f,   1.0f } },
        { "red",    { 0.957f, 0.263f, 0.212f, 1.0f } },
    };
    constexpr int numCases = 4;

    const int totalH = imgH * numCases;
    std::vector<uint8_t> pixels(static_cast<size_t>(imgW) * totalH * 4, 0);

    // 深色背景
    for (auto& p : pixels) p = 0;
    // 填充背景色 (0xFF1A1A2E)
    for (int y = 0; y < totalH; ++y)
        for (int x = 0; x < imgW; ++x)
        {
            const int i = (y * imgW + x) * 4;
            pixels[i + 0] = 0x1A; // R
            pixels[i + 1] = 0x1A; // G
            pixels[i + 2] = 0x2E; // B
            pixels[i + 3] = 0xFF; // A
        }

    for (int ci = 0; ci < numCases; ++ci)
    {
        const float rowY = static_cast<float>(ci * imgH);
        const float blobCY = rowY + centerY;
        const Color dc = cases[ci].color;

        // 绘制 blob 区域（逐像素判断是否在 blob 内）
        for (int y = static_cast<int>(rowY); y < static_cast<int>(rowY + imgH); ++y)
        {
            for (int x = x1; x < x2; ++x)
            {
                // 正弦包络判断是否在 blob 内
                const float tBlob = static_cast<float>(x - x1) / static_cast<float>(x2 - x1);
                const float envelope = std::sin(tBlob * 3.14159265f);
                const float halfHAtX = halfH * 0.9f * envelope;
                const float distFromCenter = std::abs(static_cast<float>(y) - blobCY);

                if (distFromCenter <= halfHAtX)
                {
                    // 在 blob 内：采样渐变
                    Color c = sampleBlobGradient(static_cast<float>(y), blobCY, halfH, dc);
                    // 描边：靠近边缘时加亮
                    const float edgeDist = halfHAtX - distFromCenter;
                    if (edgeDist < 1.5f)
                    {
                        const float edgeBright = 0.55f;
                        c = lerpColor(c, brighter(dc, 0.45f), (1.5f - edgeDist) / 1.5f * edgeBright);
                        c.a = (c.a > 0.55f) ? c.a : 0.55f;
                    }

                    const int i = (y * imgW + x) * 4;
                    // alpha 合成到已有像素上
                    const float srcA = c.a;
                    const float dstA = pixels[i + 3] / 255.0f;
                    const float outA = srcA + dstA * (1.0f - srcA);
                    if (outA > 0.001f)
                    {
                        pixels[i + 0] = static_cast<uint8_t>((c.r * srcA + pixels[i + 0] / 255.0f * dstA * (1.0f - srcA)) / outA * 255.0f);
                        pixels[i + 1] = static_cast<uint8_t>((c.g * srcA + pixels[i + 1] / 255.0f * dstA * (1.0f - srcA)) / outA * 255.0f);
                        pixels[i + 2] = static_cast<uint8_t>((c.b * srcA + pixels[i + 2] / 255.0f * dstA * (1.0f - srcA)) / outA * 255.0f);
                        pixels[i + 3] = static_cast<uint8_t>(outA * 255.0f);
                    }
                }
            }
        }

        // 标签文字（简单的白色矩形标记）
        for (int lx = 2; lx < 80; ++lx)
        {
            const int ly = static_cast<int>(rowY) + 4;
            if (ly >= 0 && ly < totalH)
            {
                const int i = (ly * imgW + lx) * 4;
                pixels[i + 0] = 0xFF; pixels[i + 1] = 0xFF; pixels[i + 2] = 0xFF; pixels[i + 3] = 0xFF;
            }
        }
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/blob_preview.bmp", outDir);
    if (writeBmp(pixels, imgW, totalH, path))
    {
        // 采样几个关键点
        auto sample = [&](int x, int y) -> void {
            const int i = (y * imgW + x) * 4;
            printf("  pixel[%d,%d] = R:%02X G:%02X B:%02X A:%02X\n",
                   x, y, pixels[i], pixels[i+1], pixels[i+2], pixels[i+3]);
        };
        printf("saved %s (%dx%d)\n", path, imgW, totalH);
        sample(imgW/2, imgH/2);            // 第一个 blob 中心
        sample(imgW/2, imgH/2 - 30);       // 第一个 blob 顶部
        sample(imgW/2, imgH/2 + 30);       // 第一个 blob 底部
        sample(imgW/2, imgH + imgH/2);     // 第二个 blob 中心
        return 0;
    }
    printf("FAILED to save %s\n", path);
    return 1;
}
