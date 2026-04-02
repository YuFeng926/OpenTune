#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <random>
#include "ThemeTokens.h"

namespace OpenTune {

// Kawaii 主题漂浮装饰组件 - 爱心和星星持续漂浮
class KawaiiDecorationComponent : public juce::Component, public juce::Timer
{
public:
    KawaiiDecorationComponent()
    {
        setInterceptsMouseClicks(false, false);
        setOpaque(false);
        
        // 初始化随机数生成器
        std::random_device rd;
        rng_.seed(rd());
        
        // 创建初始粒子
        for (int i = 0; i < 15; ++i)
        {
            particles_.emplace_back(createRandomParticle());
        }
    }
    
    ~KawaiiDecorationComponent() override
    {
        stopTimer();
    }
    
    void start()
    {
        startTimerHz(30); // 30fps 动画
    }
    
    void stop()
    {
        stopTimer();
    }
    
    void paint(juce::Graphics& g) override
    {
        if (Theme::getActiveTheme() != ThemeId::Kawaii)
            return;
            
        for (const auto& p : particles_)
        {
            drawParticle(g, p);
        }
    }
    
    void timerCallback() override
    {
        if (Theme::getActiveTheme() != ThemeId::Kawaii)
        {
            if (isVisible())
                setVisible(false);
            return;
        }
        
        if (!isVisible())
            setVisible(true);
            
        // 更新粒子位置
        for (auto& p : particles_)
        {
            updateParticle(p);
        }
        
        repaint();
    }
    
    void resized() override
    {
        // 粒子会自动适应新边界
    }

private:
    enum class ParticleType
    {
        Heart,
        Star,
        Sparkle
    };
    
    struct Particle
    {
        ParticleType type;
        float x, y;           // 位置
        float vx, vy;         // 速度
        float size;           // 大小
        float alpha;          // 透明度
        float rotation;       // 旋转角度
        float rotationSpeed;  // 旋转速度
        float phase;          // 动画相位
    };
    
    std::vector<Particle> particles_;
    std::mt19937 rng_;
    
    Particle createRandomParticle()
    {
        Particle p;
        
        std::uniform_int_distribution<int> typeDist(0, 2);
        int type = typeDist(rng_);
        if (type == 0) p.type = ParticleType::Heart;
        else if (type == 1) p.type = ParticleType::Star;
        else p.type = ParticleType::Sparkle;
        
        std::uniform_real_distribution<float> xDist(0.0f, 1.0f);
        std::uniform_real_distribution<float> yDist(0.0f, 1.0f);
        std::uniform_real_distribution<float> sizeDist(8.0f, 18.0f);
        std::uniform_real_distribution<float> speedDist(-0.3f, 0.3f);
        std::uniform_real_distribution<float> alphaDist(0.2f, 0.5f);
        std::uniform_real_distribution<float> rotSpeedDist(-0.02f, 0.02f);
        
        p.x = xDist(rng_);
        p.y = yDist(rng_);
        p.vx = speedDist(rng_) * 0.5f;
        p.vy = -0.1f - std::abs(speedDist(rng_)) * 0.5f; // 主要向上飘
        p.size = sizeDist(rng_);
        p.alpha = alphaDist(rng_);
        p.rotation = 0.0f;
        p.rotationSpeed = rotSpeedDist(rng_);
        p.phase = xDist(rng_) * juce::MathConstants<float>::twoPi;
        
        return p;
    }
    
    void updateParticle(Particle& p)
    {
        // 缓慢向上漂浮
        p.y += p.vy * 0.003f;
        p.x += p.vx * 0.003f + std::sin(p.phase) * 0.0005f; // 添加摆动
        p.phase += 0.02f;
        p.rotation += p.rotationSpeed;
        
        // 透明度脉动
        p.alpha = 0.3f + 0.15f * std::sin(p.phase * 0.5f);
        
        // 如果飘出顶部，从底部重新出现
        if (p.y < -0.1f)
        {
            p.y = 1.1f;
            p.x = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_);
        }
        
        // 左右边界循环
        if (p.x < -0.1f) p.x = 1.1f;
        if (p.x > 1.1f) p.x = -0.1f;
    }
    
    void drawParticle(juce::Graphics& g, const Particle& p)
    {
        float drawX = p.x * getWidth();
        float drawY = p.y * getHeight();
        
        g.addTransform(juce::AffineTransform::translation(drawX, drawY)
                                            .rotated(p.rotation, drawX, drawY));
        
        switch (p.type)
        {
            case ParticleType::Heart:
                drawHeart(g, 0, 0, p.size, p.alpha);
                break;
            case ParticleType::Star:
                drawStar(g, 0, 0, p.size, p.alpha);
                break;
            case ParticleType::Sparkle:
                drawSparkle(g, 0, 0, p.size, p.alpha);
                break;
        }
        
        g.addTransform(juce::AffineTransform::identity);
    }
    
    void drawHeart(juce::Graphics& g, float x, float y, float size, float alpha)
    {
        float s = size * 0.5f;
        
        juce::Path heart;
        heart.startNewSubPath(x, y + s * 0.3f);
        
        // 左侧曲线
        heart.cubicTo(x - s * 0.5f, y - s * 0.5f, x - s, y - s * 0.2f, x, y - s);
        // 右侧曲线
        heart.cubicTo(x + s, y - s * 0.2f, x + s * 0.5f, y - s * 0.5f, x, y + s * 0.3f);
        
        heart.closeSubPath();
        
        // 粉色渐变填充
        juce::ColourGradient grad(
            juce::Colour(0xFFFFB7D5).withAlpha(alpha), x - s, y - s,
            juce::Colour(0xFFFF69B4).withAlpha(alpha), x + s, y + s, false);
        g.setGradientFill(grad);
        g.fillPath(heart);
        
        // 白色高光
        g.setColour(juce::Colours::white.withAlpha(alpha * 0.5f));
        g.strokePath(heart, juce::PathStrokeType(1.0f));
    }
    
    void drawStar(juce::Graphics& g, float x, float y, float size, float alpha)
    {
        juce::Path star;
        float outerRadius = size * 0.5f;
        float innerRadius = outerRadius * 0.4f;
        int numPoints = 5;
        
        for (int i = 0; i < numPoints * 2; ++i)
        {
            float angle = i * juce::MathConstants<float>::pi / numPoints - juce::MathConstants<float>::halfPi;
            float r = (i % 2 == 0) ? outerRadius : innerRadius;
            float px = x + std::cos(angle) * r;
            float py = y + std::sin(angle) * r;
            
            if (i == 0)
                star.startNewSubPath(px, py);
            else
                star.lineTo(px, py);
        }
        star.closeSubPath();
        
        // 天蓝色填充
        g.setColour(juce::Colour(0xFF4FC3F7).withAlpha(alpha));
        g.fillPath(star);
        
        // 白色高光
        g.setColour(juce::Colours::white.withAlpha(alpha * 0.6f));
        g.strokePath(star, juce::PathStrokeType(1.0f));
    }
    
    void drawSparkle(juce::Graphics& g, float x, float y, float size, float alpha)
    {
        float s = size * 0.5f;
        
        // 十字形闪光
        juce::Path sparkle;
        sparkle.addEllipse(x - s * 0.15f, y - s, s * 0.3f, s * 2.0f);
        sparkle.addEllipse(x - s, y - s * 0.15f, s * 2.0f, s * 0.3f);
        
        // 45度旋转的十字
        juce::Path sparkle2;
        sparkle2.addEllipse(x - s * 0.1f, y - s * 0.7f, s * 0.2f, s * 1.4f);
        sparkle2.applyTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::quarterPi, x, y));
        
        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.fillPath(sparkle);
        g.fillPath(sparkle2);
    }
};

} // namespace OpenTune
