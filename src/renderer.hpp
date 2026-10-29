// Minimal OpenGL 3.3 renderer: batched lines and points, a background
// starfield, and the black hole itself (glow + event horizon).
#pragma once

#include <glad/gl.h>

#include <vector>

#include "physics.hpp"

namespace bh {

struct Color {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

inline Color withAlpha(Color c, float a) { return {c.r, c.g, c.b, a}; }
Color lerp(Color a, Color b, float t);

// Maps world coordinates (units of M) to the screen.
struct Camera {
    Vec2 center{0.0, 0.0};
    double zoom = 12.0;  // pixels per M
    int width = 1;       // framebuffer size in pixels
    int height = 1;

    Vec2 worldToScreen(Vec2 world) const;   // screen: origin top-left, y down
    Vec2 screenToWorld(Vec2 screen) const;
    void zoomAt(Vec2 screen, double factor);
};

class Renderer {
public:
    bool init();
    void shutdown();

    void beginFrame(const Camera& camera);

    // Batched primitives in world coordinates. Flushed by the draw* calls.
    void line(Vec2 a, Vec2 b, Color ca, Color cb);
    void line(Vec2 a, Vec2 b, Color c) { line(a, b, c, c); }
    void circle(Vec2 center, double radius, Color c, bool dashed = false, int segments = 128);
    void point(Vec2 p, Color c, float sizePixels);

    // `additive` blending makes overlapping trails glow.
    void drawLines(bool additive);
    void drawPoints(bool additive);

    void drawStars();
    void drawBlackHoleGlow();
    void drawEventHorizon();

private:
    struct Batch {
        GLuint vao = 0;
        GLuint vbo = 0;
        std::vector<float> data;
        int floatsPerVertex = 0;
    };

    void createBatch(Batch& batch, int floatsPerVertex, bool hasSize);
    void uploadAndDraw(Batch& batch, GLenum mode);
    void setTransformUniforms(GLuint program, bool worldSpace);
    void drawHole(int mode);  // 0 = glow, 1 = event horizon

    Camera camera_;
    GLuint lineProgram_ = 0;
    GLuint pointProgram_ = 0;
    GLuint holeProgram_ = 0;
    GLuint emptyVao_ = 0;
    Batch lines_;
    Batch points_;
    Batch stars_;
    std::size_t starCount_ = 0;
};

}  // namespace bh
