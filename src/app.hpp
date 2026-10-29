// The interactive application: window, input handling, user interface and
// drawing of the simulation state.
#pragma once

#include <string>

#include "physics.hpp"
#include "renderer.hpp"

struct GLFWwindow;

namespace bh {

struct AppOptions {
    int width = 1280;
    int height = 800;
    std::string scene;           // scene to load at startup (empty = none)
    std::string screenshotPath;  // with `frames`: save a screenshot and quit
    int frames = 0;              // run a fixed number of frames, then quit
    bool hideUi = false;
};

enum class Tool { Ray, Beam, Fan, Flash, Particle };

class App {
public:
    int run(const AppOptions& options);

    // Names accepted by loadScene() / --scene.
    static const char* const kSceneNames[];
    static constexpr int kSceneCount = 5;

private:
    bool init(const AppOptions& options);
    void shutdown();
    void frame(double dt);

    void handleInput();
    void onDragFinished(Vec2 startWorld, Vec2 endWorld, Vec2 dragPixels);
    bool loadScene(const std::string& name);
    void resetCamera();

    void drawScene();
    void drawGuides();
    void drawPhotons();
    void drawParticles();
    void drawDragPreview();
    void drawLabels();
    void drawUi();

    Vec2 mouseScreen() const;  // in framebuffer pixels
    Vec2 particleVelocityFromDrag(Vec2 startWorld, Vec2 dragPixels) const;
    bool saveScreenshot(const std::string& path);

    GLFWwindow* window_ = nullptr;
    Renderer renderer_;
    Camera camera_;
    Simulation sim_;

    // View and interaction state.
    Tool tool_ = Tool::Beam;
    bool paused_ = false;
    bool showUi_ = true;
    bool showGuides_ = true;
    bool showLabels_ = true;
    bool showStars_ = true;
    bool dragging_ = false;
    bool panning_ = false;
    Vec2 dragStartScreen_;
    Vec2 dragStartWorld_;
    Vec2 lastMouse_;
    std::string pendingScreenshot_;
    std::string statusMessage_;
    double statusTime_ = 0.0;
    double fps_ = 0.0;

    // Tool settings.
    int beamCount_ = 31;
    float beamWidth_ = 30.0f;
    int fanCount_ = 31;
    float fanSpreadDegrees_ = 50.0f;
    int flashCount_ = 240;
};

}  // namespace bh
