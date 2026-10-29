#include "renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace bh {

namespace {

constexpr double kPi = 3.14159265358979323846;

const char* kLineVertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
uniform vec2 uCenter;
uniform vec2 uScale;
out vec4 vColor;
void main() {
    gl_Position = vec4((aPos - uCenter) * uScale, 0.0, 1.0);
    vColor = aColor;
}
)";

const char* kLineFragment = R"(#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";

const char* kPointVertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSize;
uniform vec2 uCenter;
uniform vec2 uScale;
out vec4 vColor;
void main() {
    gl_Position = vec4((aPos - uCenter) * uScale, 0.0, 1.0);
    gl_PointSize = aSize;
    vColor = aColor;
}
)";

// Round, soft-edged points.
const char* kPointFragment = R"(#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() {
    float d = length(gl_PointCoord * 2.0 - 1.0);
    if (d > 1.0) discard;
    fragColor = vec4(vColor.rgb, vColor.a * (1.0 - smoothstep(0.4, 1.0, d)));
}
)";

// Full-screen triangle generated from gl_VertexID.
const char* kHoleVertex = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kHoleFragment = R"(#version 330 core
uniform vec2 uCenter;
uniform vec2 uViewport;
uniform float uZoom;
uniform int uMode;      // 0 = glow, 1 = event horizon
out vec4 fragColor;
void main() {
    vec2 world = uCenter + (gl_FragCoord.xy - 0.5 * uViewport) / uZoom;
    float r = length(world);
    float pixel = 1.0 / uZoom;
    if (uMode == 0) {
        float outside = max(r - 2.0, 0.0);
        vec3 warm = vec3(1.0, 0.45, 0.15) * 0.55 * exp(-outside / 1.2);
        vec3 haze = vec3(0.25, 0.30, 0.65) * 0.18 * exp(-outside / 10.0);
        vec3 ring = vec3(1.0, 0.85, 0.6) * 0.18 * exp(-pow((r - 3.0) / 0.12, 2.0));
        fragColor = vec4((warm + haze + ring) * step(2.0, r), 1.0);
    } else {
        float inside = 1.0 - smoothstep(2.0 - pixel, 2.0 + pixel, r);
        fragColor = vec4(0.0, 0.0, 0.0, inside);
    }
}
)";

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::fprintf(stderr, "Shader compile error:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(const char* vertexSource, const char* fragmentSource) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof log, nullptr, log);
        std::fprintf(stderr, "Shader link error:\n%s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void setBlend(bool additive) {
    glEnable(GL_BLEND);
    if (additive) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    } else {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
}

}  // namespace

Color lerp(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t};
}

Vec2 Camera::worldToScreen(Vec2 world) const {
    return {(world.x - center.x) * zoom + width * 0.5, height * 0.5 - (world.y - center.y) * zoom};
}

Vec2 Camera::screenToWorld(Vec2 screen) const {
    return {center.x + (screen.x - width * 0.5) / zoom, center.y - (screen.y - height * 0.5) / zoom};
}

void Camera::zoomAt(Vec2 screen, double factor) {
    const Vec2 before = screenToWorld(screen);
    zoom = std::clamp(zoom * factor, 0.5, 400.0);
    const Vec2 after = screenToWorld(screen);
    center += before - after;
}

bool Renderer::init() {
    lineProgram_ = linkProgram(kLineVertex, kLineFragment);
    pointProgram_ = linkProgram(kPointVertex, kPointFragment);
    holeProgram_ = linkProgram(kHoleVertex, kHoleFragment);
    if (!lineProgram_ || !pointProgram_ || !holeProgram_) return false;

    createBatch(lines_, 6, false);
    createBatch(points_, 7, true);
    createBatch(stars_, 7, true);
    glGenVertexArrays(1, &emptyVao_);
    glEnable(GL_PROGRAM_POINT_SIZE);

    // Static starfield in normalised device coordinates.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> pos(-1.0f, 1.0f);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    starCount_ = 900;
    for (std::size_t i = 0; i < starCount_; ++i) {
        const float brightness = 0.15f + 0.6f * unit(rng) * unit(rng);
        const float tint = unit(rng);
        stars_.data.insert(stars_.data.end(),
                           {pos(rng), pos(rng), 0.8f + 0.2f * tint, 0.85f + 0.1f * tint, 1.0f,
                            brightness, 1.0f + 2.0f * unit(rng) * unit(rng)});
    }
    glBindBuffer(GL_ARRAY_BUFFER, stars_.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(stars_.data.size() * sizeof(float)),
                 stars_.data.data(), GL_STATIC_DRAW);
    return true;
}

void Renderer::shutdown() {
    for (Batch* batch : {&lines_, &points_, &stars_}) {
        glDeleteVertexArrays(1, &batch->vao);
        glDeleteBuffers(1, &batch->vbo);
    }
    glDeleteVertexArrays(1, &emptyVao_);
    glDeleteProgram(lineProgram_);
    glDeleteProgram(pointProgram_);
    glDeleteProgram(holeProgram_);
}

void Renderer::createBatch(Batch& batch, int floatsPerVertex, bool hasSize) {
    batch.floatsPerVertex = floatsPerVertex;
    glGenVertexArrays(1, &batch.vao);
    glGenBuffers(1, &batch.vbo);
    glBindVertexArray(batch.vao);
    glBindBuffer(GL_ARRAY_BUFFER, batch.vbo);
    const GLsizei stride = floatsPerVertex * static_cast<GLsizei>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(2 * sizeof(float)));
    if (hasSize) {
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(6 * sizeof(float)));
    }
    glBindVertexArray(0);
}

void Renderer::beginFrame(const Camera& camera) {
    camera_ = camera;
    glViewport(0, 0, camera.width, camera.height);
    glClearColor(0.008f, 0.01f, 0.025f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::setTransformUniforms(GLuint program, bool worldSpace) {
    glUseProgram(program);
    if (worldSpace) {
        glUniform2f(glGetUniformLocation(program, "uCenter"), static_cast<float>(camera_.center.x),
                    static_cast<float>(camera_.center.y));
        glUniform2f(glGetUniformLocation(program, "uScale"),
                    static_cast<float>(2.0 * camera_.zoom / camera_.width),
                    static_cast<float>(2.0 * camera_.zoom / camera_.height));
    } else {
        glUniform2f(glGetUniformLocation(program, "uCenter"), 0.0f, 0.0f);
        glUniform2f(glGetUniformLocation(program, "uScale"), 1.0f, 1.0f);
    }
}

void Renderer::line(Vec2 a, Vec2 b, Color ca, Color cb) {
    lines_.data.insert(lines_.data.end(),
                       {static_cast<float>(a.x), static_cast<float>(a.y), ca.r, ca.g, ca.b, ca.a,
                        static_cast<float>(b.x), static_cast<float>(b.y), cb.r, cb.g, cb.b, cb.a});
}

void Renderer::circle(Vec2 center, double radius, Color c, bool dashed, int segments) {
    for (int i = 0; i < segments; ++i) {
        if (dashed && i % 2 == 1) continue;
        const double a0 = 2.0 * kPi * i / segments;
        const double a1 = 2.0 * kPi * (i + 1) / segments;
        line(center + Vec2{std::cos(a0), std::sin(a0)} * radius,
             center + Vec2{std::cos(a1), std::sin(a1)} * radius, c);
    }
}

void Renderer::point(Vec2 p, Color c, float sizePixels) {
    points_.data.insert(points_.data.end(), {static_cast<float>(p.x), static_cast<float>(p.y), c.r,
                                             c.g, c.b, c.a, sizePixels});
}

void Renderer::uploadAndDraw(Batch& batch, GLenum mode) {
    if (batch.data.empty()) return;
    glBindVertexArray(batch.vao);
    glBindBuffer(GL_ARRAY_BUFFER, batch.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(batch.data.size() * sizeof(float)),
                 batch.data.data(), GL_STREAM_DRAW);
    glDrawArrays(mode, 0, static_cast<GLsizei>(batch.data.size() / batch.floatsPerVertex));
    glBindVertexArray(0);
    batch.data.clear();
}

void Renderer::drawLines(bool additive) {
    setBlend(additive);
    setTransformUniforms(lineProgram_, true);
    uploadAndDraw(lines_, GL_LINES);
}

void Renderer::drawPoints(bool additive) {
    setBlend(additive);
    setTransformUniforms(pointProgram_, true);
    uploadAndDraw(points_, GL_POINTS);
}

void Renderer::drawStars() {
    setBlend(true);
    setTransformUniforms(pointProgram_, false);
    glBindVertexArray(stars_.vao);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(starCount_));
    glBindVertexArray(0);
}

void Renderer::drawBlackHoleGlow() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    drawHole(0);
}

void Renderer::drawEventHorizon() {
    setBlend(false);
    drawHole(1);
}

void Renderer::drawHole(int mode) {
    glUseProgram(holeProgram_);
    glUniform2f(glGetUniformLocation(holeProgram_, "uCenter"), static_cast<float>(camera_.center.x),
                static_cast<float>(camera_.center.y));
    glUniform2f(glGetUniformLocation(holeProgram_, "uViewport"), static_cast<float>(camera_.width),
                static_cast<float>(camera_.height));
    glUniform1f(glGetUniformLocation(holeProgram_, "uZoom"), static_cast<float>(camera_.zoom));
    glUniform1i(glGetUniformLocation(holeProgram_, "uMode"), mode);
    glBindVertexArray(emptyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

}  // namespace bh
