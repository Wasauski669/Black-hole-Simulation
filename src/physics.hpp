// Physics of a non-rotating (Schwarzschild) black hole in two dimensions.
//
// Units: geometric units with G = c = 1 and black hole mass M = 1, so every
// length is measured in units of M:
//   event horizon      r = 2M
//   photon sphere      r = 3M
//   ISCO               r = 6M  (innermost stable circular orbit)
//
// Motion is restricted to the black hole's equatorial plane, which is exact
// for Schwarzschild geodesics. The equations of motion in Schwarzschild
// coordinates (r, phi) can be written in a "Newton-like" Cartesian form on
// the plane x = (r cos phi, r sin phi), with h = |x × v| conserved:
//
//   light     x'' = -3 M h^2 x / r^5                (affine parameter)
//   matter    x'' = -M x / r^3 - 3 M h^2 x / r^5    (proper time)
//
// Both reproduce the exact relativistic orbit equation
// u'' + u = M/h^2 (matter only) + 3 M u^2, with u = 1/r.
#pragma once

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace bh {

constexpr double kMass = 1.0;
constexpr double kHorizon = 2.0 * kMass;
constexpr double kPhotonSphere = 3.0 * kMass;
constexpr double kIsco = 6.0 * kMass;
// Light with an impact parameter below this value is captured.
const double kCriticalImpact = 3.0 * std::sqrt(3.0) * kMass;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}

    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    Vec2 operator/(double s) const { return {x / s, y / s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }

    double length() const { return std::sqrt(x * x + y * y); }
    double lengthSq() const { return x * x + y * y; }
    Vec2 normalized() const {
        const double len = length();
        return len > 0.0 ? *this / len : Vec2{};
    }
};

inline Vec2 operator*(double s, Vec2 v) { return v * s; }
inline double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline Vec2 perpendicular(Vec2 v) { return {-v.y, v.x}; }
inline Vec2 rotated(Vec2 v, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}

enum class Gravity {
    Relativistic,  // Schwarzschild geodesics (general relativity)
    Newtonian,     // classical inverse-square gravity, for comparison
};

// Accelerations used by the integrators (see header comment).
Vec2 photonAcceleration(Vec2 pos, Vec2 vel, Gravity gravity);
Vec2 particleAcceleration(Vec2 pos, Vec2 vel, Gravity gravity);

// Speed (in the integrator's velocity units) of a circular orbit at radius r.
// Returns NaN when no circular orbit exists (r <= 3M for relativistic gravity).
double circularOrbitSpeed(double r, Gravity gravity);

// Impact parameter of a straight line through `pos` with direction `dir`.
double impactParameter(Vec2 pos, Vec2 dir);

// Fixed-capacity history of positions, used to draw trails.
class Trail {
public:
    explicit Trail(std::size_t capacity = 0) : points_(capacity) {}

    void push(Vec2 p);
    void clear() { size_ = 0; head_ = 0; }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return points_.size(); }
    // i = 0 is the oldest point, size() - 1 the newest.
    Vec2 operator[](std::size_t i) const {
        return points_[(head_ + points_.size() - size_ + i) % points_.size()];
    }
    Vec2 newest() const { return (*this)[size_ - 1]; }

private:
    std::vector<Vec2> points_;
    std::size_t head_ = 0;  // next write position
    std::size_t size_ = 0;
};

enum class Fate { Active, Captured, Escaped };

struct Photon {
    Vec2 pos;
    Vec2 vel;               // tangent vector, |vel| = 1 at emission
    double impact = 0.0;    // impact parameter b
    double pathLength = 0.0;
    double sinceLastPoint = 0.0;
    double fadeTime = 0.0;  // seconds since the photon stopped
    Fate fate = Fate::Active;
    Trail trail;
};

struct Particle {
    Vec2 pos;
    Vec2 vel;               // dx/dtau
    double sinceLastPoint = 0.0;
    double fadeTime = 0.0;
    float hue = 0.0f;       // 0..1, chosen at spawn, used for colouring
    Fate fate = Fate::Active;
    Trail trail;
};

// Advance one photon by a path length of `distance` (in M).
void advancePhoton(Photon& photon, double distance, Gravity gravity, double escapeRadius,
                   double maxPathLength);

// Advance one particle by `dtau` units of proper time (in M).
// `samplePeriod` is the proper time between recorded trail points.
void advanceParticle(Particle& particle, double dtau, Gravity gravity, double escapeRadius,
                     double samplePeriod);

struct SimulationSettings {
    Gravity gravity = Gravity::Relativistic;
    double lightSpeed = 40.0;       // photon path length per real second (M/s)
    double timeWarp = 60.0;         // particle proper time per real second (M/s)
    double trailSeconds = 1.5;      // particle trail length in real seconds
    double photonFadeSeconds = 6.0; // how long finished light rays stay visible
    bool persistentRays = false;    // keep finished light rays forever
    double escapeRadius = 400.0;
};

struct SimulationStats {
    std::uint64_t photonsCaptured = 0;
    std::uint64_t photonsEscaped = 0;
    std::uint64_t particlesCaptured = 0;
    std::uint64_t particlesEscaped = 0;
};

class Simulation {
public:
    static constexpr std::size_t kMaxPhotons = 4000;
    static constexpr std::size_t kMaxParticles = 6000;
    static constexpr std::size_t kPhotonTrailCapacity = 6000;
    static constexpr std::size_t kParticleTrailCapacity = 48;

    SimulationSettings settings;

    void update(double realDt);
    void clear();
    void clearPhotons() { photons_.clear(); }
    void clearParticles() { particles_.clear(); }

    // Light. `dir` does not need to be normalised.
    void addPhoton(Vec2 pos, Vec2 dir);
    void addBeam(Vec2 center, Vec2 dir, double width, int count);
    void addFan(Vec2 origin, Vec2 dir, double spreadRadians, int count);
    void addRing(Vec2 center, int count);

    // Matter. `vel` is dx/dtau. A longer trail shows more of the orbit's history.
    void addParticle(Vec2 pos, Vec2 vel, float hue,
                     std::size_t trailPoints = kParticleTrailCapacity);
    // Spawns `count` particles on nearly circular orbits between rInner and rOuter.
    void addAccretionDisk(int count, double rInner, double rOuter, bool counterClockwise);

    const std::vector<Photon>& photons() const { return photons_; }
    const std::vector<Particle>& particles() const { return particles_; }
    const SimulationStats& stats() const { return stats_; }

private:
    std::vector<Photon> photons_;
    std::vector<Particle> particles_;
    SimulationStats stats_;
    std::mt19937 rng_{12345};
};

}  // namespace bh
