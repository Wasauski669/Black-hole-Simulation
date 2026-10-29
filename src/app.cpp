#include "app.hpp"

#include <glad/gl.h>
// glad must come before GLFW.
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

namespace bh {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDragThresholdPixels = 5.0;
// Screen pixels of drag per unit of particle speed.
constexpr double kPixelsPerSpeed = 350.0;
// Particles placed by hand get their own colour instead of the disk palette.
constexpr float kUserParticleHue = -1.0f;

const char* const kToolNames[] = {"Light ray", "Light beam", "Light fan", "Light flash", "Particle"};
const char* const kToolHints[] = {
    "Click and drag to aim a single light ray.",
    "Click and drag to fire a beam of parallel light rays.",
    "Click and drag to fire a fan of rays from one point.",
    "Click to emit light in every direction.",
    "Drag to throw a particle. Click for a circular orbit.",
};

Color photonColor(double impact) {
    // Warm colours: captured (b < 3*sqrt(3) M). Cool colours: escaping.
    const float t = static_cast<float>(impact / kCriticalImpact);
    if (t < 1.0f) return lerp({1.0f, 0.22f, 0.12f, 1.0f}, {1.0f, 0.72f, 0.28f, 1.0f}, t);
    return lerp({1.0f, 0.93f, 0.75f, 1.0f}, {0.45f, 0.68f, 1.0f, 1.0f},
                std::clamp((t - 1.0f) / 3.0f, 0.0f, 1.0f));
}

Color particleColor(float hue) {
    if (hue < 0.0f) return {0.45f, 1.0f, 0.75f, 1.0f};
    const Color hot{1.0f, 0.97f, 0.88f, 1.0f};
    const Color warm{1.0f, 0.70f, 0.30f, 1.0f};
    const Color cool{0.90f, 0.28f, 0.14f, 1.0f};
    return hue < 0.35f ? lerp(hot, warm, hue / 0.35f) : lerp(warm, cool, (hue - 0.35f) / 0.65f);
}

ImU32 toImColor(Color c) {
    return IM_COL32(static_cast<int>(c.r * 255), static_cast<int>(c.g * 255),
                    static_cast<int>(c.b * 255), static_cast<int>(c.a * 255));
}

void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, description);
}

std::string timestampedScreenshotName() {
    char buffer[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(buffer, sizeof buffer, "screenshot-%Y%m%d-%H%M%S.png", std::localtime(&now));
    return buffer;
}

}  // namespace

const char* const App::kSceneNames[] = {"lensing", "disk", "flash", "precession", "both"};

int App::run(const AppOptions& options) {
    if (!init(options)) return 1;

    const bool fixedRun = options.frames > 0;
    double previous = glfwGetTime();
    int frameCount = 0;
    while (!glfwWindowShouldClose(window_)) {
        const double now = glfwGetTime();
        // Fixed time step for scripted runs so screenshots are reproducible.
        const double dt = fixedRun ? 1.0 / 60.0 : now - previous;
        previous = now;

        ++frameCount;
        if (fixedRun && frameCount == options.frames) {
            pendingScreenshot_ = options.screenshotPath;
        }
        frame(dt);
        if (fixedRun && frameCount >= options.frames) break;
    }

    shutdown();
    return 0;
}

bool App::init(const AppOptions& options) {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) return false;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);  // required on macOS
    glfwWindowHint(GLFW_SAMPLES, 4);

    window_ = glfwCreateWindow(options.width, options.height, "Black Hole Simulation", nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr, "Could not create an OpenGL 3.3 window.\n");
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    if (!gladLoadGL(glfwGetProcAddress)) {
        std::fprintf(stderr, "Could not load OpenGL functions.\n");
        return false;
    }
    glEnable(GL_MULTISAMPLE);

    if (!renderer_.init()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't write imgui.ini next to the executable
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.88f;
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    showUi_ = !options.hideUi;
    glfwGetFramebufferSize(window_, &camera_.width, &camera_.height);
    resetCamera();

    if (!options.scene.empty() && !loadScene(options.scene)) {
        std::fprintf(stderr, "Unknown scene '%s'. Available:", options.scene.c_str());
        for (const char* name : kSceneNames) std::fprintf(stderr, " %s", name);
        std::fprintf(stderr, "\n");
    }
    return true;
}

void App::shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    renderer_.shutdown();
    glfwDestroyWindow(window_);
    glfwTerminate();
}

void App::frame(double dt) {
    glfwPollEvents();
    glfwGetFramebufferSize(window_, &camera_.width, &camera_.height);
    if (camera_.width <= 0 || camera_.height <= 0) return;  // minimised

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    fps_ = fps_ == 0.0 ? 1.0 / std::max(dt, 1e-6) : fps_ * 0.95 + 0.05 / std::max(dt, 1e-6);
    statusTime_ = std::max(0.0, statusTime_ - dt);

    handleInput();
    if (!paused_) sim_.update(dt);

    drawScene();
    if (showLabels_) drawLabels();
    if (showUi_) drawUi();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (!pendingScreenshot_.empty()) {
        if (saveScreenshot(pendingScreenshot_)) {
            statusMessage_ = "Saved " + pendingScreenshot_;
        } else {
            statusMessage_ = "Could not save " + pendingScreenshot_;
        }
        std::printf("%s\n", statusMessage_.c_str());
        statusTime_ = 3.0;
        pendingScreenshot_.clear();
    }
    glfwSwapBuffers(window_);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

Vec2 App::mouseScreen() const {
    const ImGuiIO& io = ImGui::GetIO();
    return {io.MousePos.x * io.DisplayFramebufferScale.x, io.MousePos.y * io.DisplayFramebufferScale.y};
}

Vec2 App::particleVelocityFromDrag(Vec2 startWorld, Vec2 dragPixels) const {
    if (dragPixels.length() < kDragThresholdPixels) {
        // Plain click: circular orbit (counter-clockwise), if one exists.
        const double speed = circularOrbitSpeed(startWorld.length(), sim_.settings.gravity);
        if (!std::isfinite(speed)) return {};
        return perpendicular(startWorld.normalized()) * speed;
    }
    // Screen y points down, world y points up.
    return Vec2{dragPixels.x, -dragPixels.y} / kPixelsPerSpeed;
}

void App::handleInput() {
    const ImGuiIO& io = ImGui::GetIO();
    const Vec2 mouse = mouseScreen();

    if (!io.WantCaptureKeyboard) {
        auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };
        if (pressed(ImGuiKey_1)) tool_ = Tool::Ray;
        if (pressed(ImGuiKey_2)) tool_ = Tool::Beam;
        if (pressed(ImGuiKey_3)) tool_ = Tool::Fan;
        if (pressed(ImGuiKey_4)) tool_ = Tool::Flash;
        if (pressed(ImGuiKey_5)) tool_ = Tool::Particle;
        if (pressed(ImGuiKey_Space)) paused_ = !paused_;
        if (pressed(ImGuiKey_C)) sim_.clear();
        if (pressed(ImGuiKey_D)) loadScene("disk");
        if (pressed(ImGuiKey_G)) showGuides_ = !showGuides_;
        if (pressed(ImGuiKey_L)) showLabels_ = !showLabels_;
        if (pressed(ImGuiKey_H)) showUi_ = !showUi_;
        if (pressed(ImGuiKey_R)) resetCamera();
        if (pressed(ImGuiKey_P)) pendingScreenshot_ = timestampedScreenshotName();
        if (pressed(ImGuiKey_N)) {
            auto& gravity = sim_.settings.gravity;
            gravity = gravity == Gravity::Relativistic ? Gravity::Newtonian : Gravity::Relativistic;
        }
        if (pressed(ImGuiKey_Escape)) glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

    if (!io.WantCaptureMouse) {
        if (io.MouseWheel != 0.0f) camera_.zoomAt(mouse, std::pow(1.15, io.MouseWheel));

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dragging_ = true;
            dragStartScreen_ = mouse;
            dragStartWorld_ = camera_.screenToWorld(mouse);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            panning_ = true;
        }
    }

    if (dragging_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dragging_ = false;
        onDragFinished(dragStartWorld_, camera_.screenToWorld(mouse), mouse - dragStartScreen_);
    }
    if (panning_) {
        camera_.center += Vec2{-(mouse.x - lastMouse_.x), mouse.y - lastMouse_.y} / camera_.zoom;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            panning_ = false;
        }
    }
    lastMouse_ = mouse;
}

void App::onDragFinished(Vec2 startWorld, Vec2 endWorld, Vec2 dragPixels) {
    const bool clicked = dragPixels.length() < kDragThresholdPixels;
    const Vec2 dir = clicked ? Vec2{1.0, 0.0} : endWorld - startWorld;

    switch (tool_) {
        case Tool::Ray:
            sim_.addPhoton(startWorld, dir);
            break;
        case Tool::Beam:
            sim_.addBeam(startWorld, dir, beamWidth_, beamCount_);
            break;
        case Tool::Fan:
            sim_.addFan(startWorld, dir, fanSpreadDegrees_ * kPi / 180.0, fanCount_);
            break;
        case Tool::Flash:
            sim_.addRing(startWorld, flashCount_);
            break;
        case Tool::Particle:
            sim_.addParticle(startWorld, particleVelocityFromDrag(startWorld, dragPixels), kUserParticleHue);
            break;
    }
}

void App::resetCamera() {
    camera_.center = {0.0, 0.0};
    // Show roughly 35 M from the centre to the nearest window edge.
    camera_.zoom = std::min(camera_.width, camera_.height) / 70.0;
}

bool App::loadScene(const std::string& name) {
    if (name == "lensing") {
        sim_.clear();
        resetCamera();
        sim_.addBeam({-70.0, 0.0}, {1.0, 0.0}, 40.0, 41);
    } else if (name == "disk") {
        sim_.clearParticles();
        sim_.addAccretionDisk(1800, 4.5, 32.0, true);
    } else if (name == "flash") {
        sim_.clear();
        resetCamera();
        sim_.addRing({-7.0, 0.0}, 360);
    } else if (name == "precession") {
        // An eccentric orbit whose closest approach slowly rotates.
        sim_.clear();
        resetCamera();
        sim_.settings.timeWarp = 250.0;
        const double apoapsis = 30.0;
        const double speed = circularOrbitSpeed(apoapsis, sim_.settings.gravity) * 0.75;
        sim_.addParticle({apoapsis, 0.0}, {0.0, speed}, kUserParticleHue, 3000);
    } else if (name == "both") {
        sim_.clear();
        resetCamera();
        sim_.addAccretionDisk(1500, 4.5, 30.0, true);
        sim_.addBeam({-70.0, 0.0}, {1.0, 0.0}, 40.0, 41);
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void App::drawScene() {
    renderer_.beginFrame(camera_);
    if (showStars_) renderer_.drawStars();
    renderer_.drawBlackHoleGlow();

    if (showGuides_) {
        drawGuides();
        renderer_.drawLines(false);
    }

    drawPhotons();
    drawParticles();
    renderer_.drawLines(true);
    renderer_.drawPoints(true);

    renderer_.drawEventHorizon();

    if (dragging_) {
        drawDragPreview();
        renderer_.drawLines(false);
        renderer_.drawPoints(false);
    }
}

void App::drawGuides() {
    renderer_.circle({0, 0}, kPhotonSphere, {1.0f, 0.85f, 0.55f, 0.35f}, true, 96);
    renderer_.circle({0, 0}, kIsco, {0.55f, 0.75f, 1.0f, 0.30f}, true, 128);
    renderer_.circle({0, 0}, kHorizon, {1.0f, 0.55f, 0.25f, 0.8f}, false, 128);
}

void App::drawPhotons() {
    const auto& settings = sim_.settings;
    for (const Photon& photon : sim_.photons()) {
        float fade = 1.0f;
        if (photon.fate != Fate::Active && !settings.persistentRays) {
            fade = 1.0f - static_cast<float>(photon.fadeTime / settings.photonFadeSeconds);
        }
        const Color color = photonColor(photon.impact);
        const Color trailColor = withAlpha(color, 0.55f * fade);
        const Trail& trail = photon.trail;
        for (std::size_t i = 1; i < trail.size(); ++i) {
            renderer_.line(trail[i - 1], trail[i], trailColor);
        }
        if (photon.fate == Fate::Active) {
            if (trail.size() > 0) renderer_.line(trail.newest(), photon.pos, trailColor);
            renderer_.point(photon.pos, withAlpha(color, 1.0f), 5.0f);
        }
    }
}

void App::drawParticles() {
    const auto& settings = sim_.settings;
    for (const Particle& particle : sim_.particles()) {
        float fade = 1.0f;
        if (particle.fate != Fate::Active) {
            fade = 1.0f - static_cast<float>(particle.fadeTime / settings.trailSeconds);
        }
        const Color color = particleColor(particle.hue);
        const Trail& trail = particle.trail;
        const float n = static_cast<float>(trail.size());
        for (std::size_t i = 1; i < trail.size(); ++i) {
            // Older trail points are more transparent.
            const float a0 = 0.7f * fade * static_cast<float>(i - 1) / n;
            const float a1 = 0.7f * fade * static_cast<float>(i) / n;
            renderer_.line(trail[i - 1], trail[i], withAlpha(color, a0), withAlpha(color, a1));
        }
        if (particle.fate == Fate::Active) {
            if (trail.size() > 0) renderer_.line(trail.newest(), particle.pos, withAlpha(color, 0.7f));
            renderer_.point(particle.pos, withAlpha(color, 0.95f), 3.5f);
        }
    }
}

void App::drawDragPreview() {
    const Vec2 mouse = mouseScreen();
    const Vec2 dragPixels = mouse - dragStartScreen_;
    const Vec2 end = camera_.screenToWorld(mouse);
    const bool clicked = dragPixels.length() < kDragThresholdPixels;
    const Color guide{1.0f, 1.0f, 1.0f, 0.55f};
    const Color path{1.0f, 1.0f, 1.0f, 0.30f};
    const auto& settings = sim_.settings;

    if (tool_ == Tool::Particle) {
        Particle ghost;
        ghost.pos = dragStartWorld_;
        ghost.vel = particleVelocityFromDrag(dragStartWorld_, dragPixels);
        if (!clicked) renderer_.line(dragStartWorld_, end, guide);
        // Predicted path.
        Vec2 previous = ghost.pos;
        for (int i = 0; i < 600 && ghost.fate == Fate::Active; ++i) {
            advanceParticle(ghost, 2.0, settings.gravity, settings.escapeRadius, 1e9);
            if (i % 2 == 0) renderer_.line(previous, ghost.pos, path);
            previous = ghost.pos;
        }
        renderer_.point(dragStartWorld_, particleColor(kUserParticleHue), 6.0f);
        return;
    }

    if (tool_ == Tool::Flash) {
        renderer_.point(dragStartWorld_, guide, 8.0f);
        return;
    }

    const Vec2 dir = (clicked ? Vec2{1.0, 0.0} : end - dragStartWorld_).normalized();
    if (!clicked) renderer_.line(dragStartWorld_, end, guide);
    if (tool_ == Tool::Beam) {
        const Vec2 side = perpendicular(dir) * (beamWidth_ * 0.5);
        renderer_.line(dragStartWorld_ - side, dragStartWorld_ + side, guide);
    }
    if (tool_ == Tool::Fan) {
        const double half = fanSpreadDegrees_ * kPi / 360.0;
        const double length = std::max(3.0, (end - dragStartWorld_).length());
        renderer_.line(dragStartWorld_, dragStartWorld_ + rotated(dir, half) * length, path);
        renderer_.line(dragStartWorld_, dragStartWorld_ + rotated(dir, -half) * length, path);
    }

    // Predicted path of the central ray.
    if (dragStartWorld_.length() > kHorizon) {
        Photon ghost;
        ghost.pos = dragStartWorld_;
        ghost.vel = dir;
        Vec2 previous = ghost.pos;
        for (int i = 0; i < 500 && ghost.fate == Fate::Active; ++i) {
            advancePhoton(ghost, 1.5, settings.gravity, settings.escapeRadius, 2000.0);
            if (i % 2 == 0) renderer_.line(previous, ghost.pos, path);
            previous = ghost.pos;
        }
    }
}

void App::drawLabels() {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    // align: 0 = text starts at the point, 0.5 = centred, 1 = text ends at the point.
    auto label = [&](Vec2 world, float align, const char* text, Color color) {
        const Vec2 s = camera_.worldToScreen(world);
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(static_cast<float>(s.x / io.DisplayFramebufferScale.x) - size.x * align,
                             static_cast<float>(s.y / io.DisplayFramebufferScale.y) - size.y * 0.5f),
                      toImColor(color), text);
    };
    // Labels on different sides of the hole so they never overlap.
    if (showGuides_ && kHorizon * camera_.zoom > 15.0) {
        label({-kHorizon - 6.0 / camera_.zoom, 0.0}, 1.0f, "Event horizon (2M)", {1.0f, 0.65f, 0.35f, 0.9f});
        label({0.0, -kPhotonSphere - 10.0 / camera_.zoom}, 0.5f, "Photon sphere (3M)",
              {1.0f, 0.88f, 0.6f, 0.85f});
        label({kIsco + 6.0 / camera_.zoom, 0.0}, 0.0f, "ISCO (6M)", {0.6f, 0.8f, 1.0f, 0.85f});
    }

    if (statusTime_ > 0.0) {
        const ImVec2 size = ImGui::CalcTextSize(statusMessage_.c_str());
        draw->AddText(ImVec2((io.DisplaySize.x - size.x) * 0.5f, io.DisplaySize.y - 54.0f),
                      IM_COL32(255, 255, 255, 230), statusMessage_.c_str());
    }
    if (showUi_) {
        const char* hint = kToolHints[static_cast<int>(tool_)];
        const ImVec2 size = ImGui::CalcTextSize(hint);
        draw->AddText(ImVec2((io.DisplaySize.x - size.x) * 0.5f, io.DisplaySize.y - 30.0f),
                      IM_COL32(200, 205, 220, 200), hint);
    }
    if (paused_) {
        const char* text = "PAUSED";
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2((io.DisplaySize.x - size.x) * 0.5f, 16.0f), IM_COL32(255, 210, 120, 255), text);
    }
}

void App::drawUi() {
    auto& settings = sim_.settings;

    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Black Hole Simulation")) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Schwarzschild black hole, units of M (G = c = 1)");

    ImGui::SeparatorText("Tool");
    int tool = static_cast<int>(tool_);
    for (int i = 0; i < 5; ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        ImGui::RadioButton(kToolNames[i], &tool, i);
    }
    tool_ = static_cast<Tool>(tool);
    switch (tool_) {
        case Tool::Beam:
            ImGui::SliderInt("Rays", &beamCount_, 2, 200);
            ImGui::SliderFloat("Width (M)", &beamWidth_, 1.0f, 120.0f, "%.0f");
            break;
        case Tool::Fan:
            ImGui::SliderInt("Rays", &fanCount_, 2, 200);
            ImGui::SliderFloat("Spread", &fanSpreadDegrees_, 1.0f, 360.0f, "%.0f deg");
            break;
        case Tool::Flash:
            ImGui::SliderInt("Rays", &flashCount_, 8, 1000);
            break;
        case Tool::Particle:
            ImGui::TextWrapped("Drag length sets the speed. The dotted line shows the predicted path.");
            break;
        case Tool::Ray:
            break;
    }

    ImGui::SeparatorText("Scenes");
    const char* sceneLabels[] = {"Gravitational lensing", "Accretion disk", "Light flash",
                                 "Orbit precession", "Lensing + disk"};
    for (int i = 0; i < kSceneCount; ++i) {
        if (i % 2 == 1) ImGui::SameLine();
        if (ImGui::Button(sceneLabels[i], ImVec2(155, 0))) loadScene(kSceneNames[i]);
    }
    if (ImGui::Button("Clear everything", ImVec2(155, 0))) sim_.clear();
    ImGui::SameLine();
    if (ImGui::Button(paused_ ? "Resume" : "Pause", ImVec2(155, 0))) paused_ = !paused_;

    if (ImGui::CollapsingHeader("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
        int gravity = static_cast<int>(settings.gravity);
        ImGui::RadioButton("General relativity", &gravity, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Newtonian", &gravity, 1);
        settings.gravity = static_cast<Gravity>(gravity);
        if (settings.gravity == Gravity::Newtonian) {
            ImGui::TextWrapped("Classical gravity: light bends half as much, orbits don't precess "
                               "and there is no ISCO.");
        }

        float lightSpeed = static_cast<float>(settings.lightSpeed);
        if (ImGui::SliderFloat("Light speed", &lightSpeed, 2.0f, 300.0f, "%.0f M/s",
                               ImGuiSliderFlags_Logarithmic)) {
            settings.lightSpeed = lightSpeed;
        }
        float timeWarp = static_cast<float>(settings.timeWarp);
        if (ImGui::SliderFloat("Time warp", &timeWarp, 1.0f, 1000.0f, "%.0f M/s",
                               ImGuiSliderFlags_Logarithmic)) {
            settings.timeWarp = timeWarp;
        }
        float trail = static_cast<float>(settings.trailSeconds);
        if (ImGui::SliderFloat("Particle trails", &trail, 0.1f, 10.0f, "%.1f s")) {
            settings.trailSeconds = trail;
        }
        ImGui::Checkbox("Keep light rays", &settings.persistentRays);
    }

    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Guides", &showGuides_);
        ImGui::SameLine();
        ImGui::Checkbox("Labels", &showLabels_);
        ImGui::SameLine();
        ImGui::Checkbox("Stars", &showStars_);
        if (ImGui::Button("Reset view")) resetCamera();
        ImGui::SameLine();
        if (ImGui::Button("Save screenshot")) pendingScreenshot_ = timestampedScreenshotName();
    }

    if (ImGui::CollapsingHeader("Statistics")) {
        const auto& stats = sim_.stats();
        std::size_t activePhotons = 0, activeParticles = 0;
        for (const auto& p : sim_.photons()) activePhotons += p.fate == Fate::Active;
        for (const auto& p : sim_.particles()) activeParticles += p.fate == Fate::Active;
        ImGui::Text("Light rays    %zu in flight", activePhotons);
        ImGui::Text("              %llu captured, %llu escaped",
                    static_cast<unsigned long long>(stats.photonsCaptured),
                    static_cast<unsigned long long>(stats.photonsEscaped));
        ImGui::Text("Particles     %zu orbiting", activeParticles);
        ImGui::Text("              %llu captured, %llu escaped",
                    static_cast<unsigned long long>(stats.particlesCaptured),
                    static_cast<unsigned long long>(stats.particlesEscaped));
        const Vec2 cursor = camera_.screenToWorld(mouseScreen());
        ImGui::Text("Cursor        r = %.2f M", cursor.length());
        ImGui::Text("Frame rate    %.0f FPS", fps_);
    }

    if (ImGui::CollapsingHeader("Controls")) {
        ImGui::TextUnformatted(
            "Left mouse     use the current tool\n"
            "Right mouse    pan (or middle mouse)\n"
            "Mouse wheel    zoom\n"
            "1 - 5          select tool\n"
            "Space          pause / resume\n"
            "N              toggle relativity / Newton\n"
            "D              add an accretion disk\n"
            "C              clear everything\n"
            "G / L          toggle guides / labels\n"
            "R              reset view\n"
            "P              save screenshot\n"
            "H              hide this panel\n"
            "Esc            quit");
    }
    ImGui::End();
}

bool App::saveScreenshot(const std::string& path) {
    const int w = camera_.width, h = camera_.height;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // OpenGL's origin is bottom-left; images start at the top.
    std::vector<unsigned char> flipped(pixels.size());
    const std::size_t row = static_cast<std::size_t>(w) * 4;
    for (int y = 0; y < h; ++y) {
        std::copy_n(&pixels[static_cast<std::size_t>(h - 1 - y) * row], row, &flipped[y * row]);
    }
    for (std::size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;  // opaque
    return stbi_write_png(path.c_str(), w, h, 4, flipped.data(), static_cast<int>(row)) != 0;
}

}  // namespace bh
