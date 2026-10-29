#include "physics.hpp"

#include <algorithm>
#include <limits>

namespace bh {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Hard limit on integration steps per call, so a huge time warp can never
// freeze the program.
constexpr int kMaxStepsPerAdvance = 20000;

template <typename Accel>
void rk4Step(Vec2& pos, Vec2& vel, double h, Accel accel) {
    const Vec2 k1x = vel;
    const Vec2 k1v = accel(pos, vel);
    const Vec2 k2x = vel + k1v * (h / 2);
    const Vec2 k2v = accel(pos + k1x * (h / 2), k2x);
    const Vec2 k3x = vel + k2v * (h / 2);
    const Vec2 k3v = accel(pos + k2x * (h / 2), k3x);
    const Vec2 k4x = vel + k3v * h;
    const Vec2 k4v = accel(pos + k3x * h, k4x);
    pos += (k1x + k2x * 2 + k3x * 2 + k4x) * (h / 6);
    vel += (k1v + k2v * 2 + k3v * 2 + k4v) * (h / 6);
}

// Erases finished objects whose fade-out time has expired.
template <typename T, typename Pred>
void eraseIf(std::vector<T>& items, Pred pred) {
    items.erase(std::remove_if(items.begin(), items.end(), pred), items.end());
}

}  // namespace

Vec2 photonAcceleration(Vec2 pos, Vec2 vel, Gravity gravity) {
    const double r2 = pos.lengthSq();
    const double r = std::sqrt(r2);
    if (gravity == Gravity::Newtonian) {
        // Light treated as a classical particle moving at speed c.
        return pos * (-kMass / (r2 * r));
    }
    const double h = cross(pos, vel);
    return pos * (-3.0 * kMass * h * h / (r2 * r2 * r));
}

Vec2 particleAcceleration(Vec2 pos, Vec2 vel, Gravity gravity) {
    const double r2 = pos.lengthSq();
    const double r = std::sqrt(r2);
    Vec2 accel = pos * (-kMass / (r2 * r));
    if (gravity == Gravity::Relativistic) {
        const double h = cross(pos, vel);
        accel -= pos * (3.0 * kMass * h * h / (r2 * r2 * r));
    }
    return accel;
}

double circularOrbitSpeed(double r, Gravity gravity) {
    if (gravity == Gravity::Newtonian) {
        return r > 0.0 ? std::sqrt(kMass / r) : std::numeric_limits<double>::quiet_NaN();
    }
    // From L^2 = M r^2 / (r - 3M) and v = L / r.
    if (r <= kPhotonSphere) return std::numeric_limits<double>::quiet_NaN();
    return std::sqrt(kMass / (r - kPhotonSphere));
}

double impactParameter(Vec2 pos, Vec2 dir) {
    return std::abs(cross(pos, dir.normalized()));
}

void Trail::push(Vec2 p) {
    if (points_.empty()) return;
    points_[head_] = p;
    head_ = (head_ + 1) % points_.size();
    size_ = std::min(size_ + 1, points_.size());
}

void advancePhoton(Photon& photon, double distance, Gravity gravity, double escapeRadius,
                   double maxPathLength) {
    auto accel = [gravity](Vec2 p, Vec2 v) { return photonAcceleration(p, v, gravity); };

    double remaining = distance;
    for (int step = 0; step < kMaxStepsPerAdvance && remaining > 0.0; ++step) {
        const double r = photon.pos.length();
        // Smaller steps close to the hole, where the paths curve the most.
        const double ds = std::min(std::clamp(0.01 * r, 0.002, 2.0), remaining);
        rk4Step(photon.pos, photon.vel, ds / photon.vel.length(), accel);
        photon.pathLength += ds;
        photon.sinceLastPoint += ds;
        remaining -= ds;

        const double rNew = photon.pos.length();
        if (rNew <= kHorizon) {
            photon.pos = photon.pos * (kHorizon / rNew);
            photon.trail.push(photon.pos);
            photon.fate = Fate::Captured;
            return;
        }
        if (photon.sinceLastPoint >= std::clamp(0.02 * rNew, 0.02, 2.0)) {
            photon.trail.push(photon.pos);
            photon.sinceLastPoint = 0.0;
        }
        const bool leaving = rNew > escapeRadius && dot(photon.pos, photon.vel) > 0.0;
        if (leaving || photon.pathLength > maxPathLength) {
            photon.trail.push(photon.pos);
            photon.fate = Fate::Escaped;
            return;
        }
    }
}

void advanceParticle(Particle& particle, double dtau, Gravity gravity, double escapeRadius,
                     double samplePeriod) {
    auto accel = [gravity](Vec2 p, Vec2 v) { return particleAcceleration(p, v, gravity); };

    double remaining = dtau;
    for (int step = 0; step < kMaxStepsPerAdvance && remaining > 0.0; ++step) {
        const double r = particle.pos.length();
        const double speed = std::max(particle.vel.length(), 1e-9);
        // Roughly 2000 steps per circular orbit, and never more than 1% of
        // the current radius per step.
        double h = std::min(0.003 * r * std::sqrt(r), 0.01 * r / speed);
        h = std::min(h, remaining);
        rk4Step(particle.pos, particle.vel, h, accel);
        particle.sinceLastPoint += h;
        remaining -= h;

        const double rNew = particle.pos.length();
        if (rNew <= kHorizon) {
            particle.pos = particle.pos * (kHorizon / rNew);
            particle.trail.push(particle.pos);
            particle.fate = Fate::Captured;
            return;
        }
        if (particle.sinceLastPoint >= samplePeriod) {
            particle.trail.push(particle.pos);
            particle.sinceLastPoint = 0.0;
        }
        if (rNew > escapeRadius) {
            particle.fate = Fate::Escaped;
            return;
        }
    }
}

void Simulation::update(double realDt) {
    const double dt = std::min(realDt, 0.1);
    const double maxPath = 50.0 * settings.escapeRadius;

    for (Photon& photon : photons_) {
        if (photon.fate == Fate::Active) {
            advancePhoton(photon, settings.lightSpeed * dt, settings.gravity,
                          settings.escapeRadius, maxPath);
            if (photon.fate == Fate::Captured) ++stats_.photonsCaptured;
            if (photon.fate == Fate::Escaped) ++stats_.photonsEscaped;
        } else {
            photon.fadeTime += dt;
        }
    }
    if (!settings.persistentRays) {
        eraseIf(photons_, [&](const Photon& p) {
            return p.fate != Fate::Active && p.fadeTime > settings.photonFadeSeconds;
        });
    }

    const double samplePeriod =
        settings.timeWarp * settings.trailSeconds / static_cast<double>(kParticleTrailCapacity);
    for (Particle& particle : particles_) {
        if (particle.fate == Fate::Active) {
            advanceParticle(particle, settings.timeWarp * dt, settings.gravity,
                            settings.escapeRadius, samplePeriod);
            if (particle.fate == Fate::Captured) ++stats_.particlesCaptured;
            if (particle.fate == Fate::Escaped) ++stats_.particlesEscaped;
        } else {
            particle.fadeTime += dt;
        }
    }
    eraseIf(particles_, [&](const Particle& p) {
        return p.fate == Fate::Escaped ||
               (p.fate == Fate::Captured && p.fadeTime > settings.trailSeconds);
    });
}

void Simulation::clear() {
    photons_.clear();
    particles_.clear();
    stats_ = {};
}

void Simulation::addPhoton(Vec2 pos, Vec2 dir) {
    if (pos.length() <= kHorizon || dir.lengthSq() == 0.0) return;
    if (photons_.size() >= kMaxPhotons) photons_.erase(photons_.begin());

    Photon photon;
    photon.pos = pos;
    photon.vel = dir.normalized();
    photon.impact = impactParameter(pos, dir);
    photon.trail = Trail(kPhotonTrailCapacity);
    photon.trail.push(pos);
    photons_.push_back(std::move(photon));
}

void Simulation::addBeam(Vec2 center, Vec2 dir, double width, int count) {
    const Vec2 side = perpendicular(dir.normalized());
    for (int i = 0; i < count; ++i) {
        const double t = count > 1 ? static_cast<double>(i) / (count - 1) - 0.5 : 0.0;
        addPhoton(center + side * (t * width), dir);
    }
}

void Simulation::addFan(Vec2 origin, Vec2 dir, double spreadRadians, int count) {
    for (int i = 0; i < count; ++i) {
        const double t = count > 1 ? static_cast<double>(i) / (count - 1) - 0.5 : 0.0;
        addPhoton(origin, rotated(dir, t * spreadRadians));
    }
}

void Simulation::addRing(Vec2 center, int count) {
    for (int i = 0; i < count; ++i) {
        addPhoton(center, rotated(Vec2{1.0, 0.0}, 2.0 * kPi * i / count));
    }
}

void Simulation::addParticle(Vec2 pos, Vec2 vel, float hue, std::size_t trailPoints) {
    if (pos.length() <= kHorizon) return;
    if (particles_.size() >= kMaxParticles) particles_.erase(particles_.begin());

    Particle particle;
    particle.pos = pos;
    particle.vel = vel;
    particle.hue = hue;
    particle.trail = Trail(trailPoints);
    particle.trail.push(pos);
    particles_.push_back(std::move(particle));
}

void Simulation::addAccretionDisk(int count, double rInner, double rOuter,
                                  bool counterClockwise) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::normal_distribution<double> jitter(0.0, 0.015);
    const double direction = counterClockwise ? 1.0 : -1.0;

    for (int i = 0; i < count; ++i) {
        // Uniform density per area.
        const double r = std::sqrt(rInner * rInner + unit(rng_) * (rOuter * rOuter - rInner * rInner));
        const double speed = circularOrbitSpeed(r, settings.gravity);
        if (!std::isfinite(speed)) continue;

        const Vec2 radial = rotated(Vec2{1.0, 0.0}, 2.0 * kPi * unit(rng_));
        const Vec2 tangential = perpendicular(radial) * direction;
        const Vec2 vel = tangential * (speed * (1.0 + jitter(rng_))) + radial * (speed * jitter(rng_));
        const float hue = static_cast<float>((r - rInner) / std::max(rOuter - rInner, 1e-9));
        addParticle(radial * r, vel, hue);
    }
}

}  // namespace bh
