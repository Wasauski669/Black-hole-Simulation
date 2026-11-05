// Checks the integrators against known analytic results of general relativity.
#include "physics.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace bh;

namespace {

constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void check(bool ok, const std::string& name, const std::string& detail) {
    std::printf("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    if (!ok) ++failures;
}

std::string fmt(const char* format, double a, double b = 0.0) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, format, a, b);
    return buffer;
}

Photon makePhoton(Vec2 pos, Vec2 dir) {
    Photon p;
    p.pos = pos;
    p.vel = dir.normalized();
    p.trail = Trail(16);
    return p;
}

Particle makeParticle(Vec2 pos, Vec2 vel) {
    Particle p;
    p.pos = pos;
    p.vel = vel;
    p.trail = Trail(16);
    return p;
}

// Fires a photon from far away with impact parameter b and returns it after
// it has been captured or has escaped.
Photon shootPhoton(double b, Gravity gravity, double start = 1e4) {
    Photon p = makePhoton({-start, b}, {1.0, 0.0});
    for (int i = 0; i < 100000 && p.fate == Fate::Active; ++i) {
        advancePhoton(p, 100.0, gravity, start, 1e7);
    }
    return p;
}

double deflectionAngle(const Photon& p) {
    // The photon started moving along +x, so the deflection is the angle of
    // the final direction (measured towards the black hole).
    return -std::atan2(p.vel.y, p.vel.x);
}

void testLightDeflection() {
    // Weak-field deflection including the second-order term:
    // alpha = 4M/b + (15 pi / 4) (M/b)^2
    const double b = 100.0;
    const double expected = 4.0 / b + 15.0 * kPi / 4.0 / (b * b);
    const Photon p = shootPhoton(b, Gravity::Relativistic, 1e5);
    const double got = deflectionAngle(p);
    check(p.fate == Fate::Escaped && std::abs(got - expected) / expected < 0.01,
          "light deflection at b = 100M", fmt("got %.5f rad, expected %.5f rad", got, expected));

    // Newtonian gravity bends light by only half as much (2M/b).
    const Photon n = shootPhoton(b, Gravity::Newtonian, 1e5);
    const double newton = deflectionAngle(n);
    check(std::abs(newton - 2.0 / b) / (2.0 / b) < 0.02, "Newtonian deflection is half",
          fmt("got %.5f rad, expected %.5f rad", newton, 2.0 / b));
}

void testCaptureThreshold() {
    // Critical impact parameter 3*sqrt(3) M ~ 5.196 M.
    const Photon inside = shootPhoton(kCriticalImpact - 0.05, Gravity::Relativistic);
    const Photon outside = shootPhoton(kCriticalImpact + 0.05, Gravity::Relativistic);
    check(inside.fate == Fate::Captured, "light below the critical impact parameter is captured",
          fmt("b = %.3f", kCriticalImpact - 0.05));
    check(outside.fate == Fate::Escaped, "light above the critical impact parameter escapes",
          fmt("b = %.3f", kCriticalImpact + 0.05));
}

void testPhotonSphere() {
    // A photon fired tangentially at r = 3M circles the hole (unstable orbit).
    Photon p = makePhoton({kPhotonSphere, 0.0}, {0.0, 1.0});
    double worst = 0.0;
    const double circumference = 2.0 * kPi * kPhotonSphere;
    for (int i = 0; i < 100; ++i) {
        advancePhoton(p, circumference / 100.0, Gravity::Relativistic, 100.0, 1e6);
        worst = std::max(worst, std::abs(p.pos.length() - kPhotonSphere));
    }
    check(p.fate == Fate::Active && worst < 1e-3, "photon orbits on the photon sphere",
          fmt("max radius error after one orbit: %.2e M", worst));
}

void testCircularOrbit(Gravity gravity, const char* name) {
    const double r = 10.0;
    const double v = circularOrbitSpeed(r, gravity);
    Particle p = makeParticle({r, 0.0}, {0.0, v});
    double worst = 0.0;
    for (int i = 0; i < 5000; ++i) {
        advanceParticle(p, 1.0, gravity, 1000.0, 1e9);
        worst = std::max(worst, std::abs(p.pos.length() - r));
    }
    check(p.fate == Fate::Active && worst < 1e-4, name, fmt("max radius drift over ~25 orbits: %.2e M", worst));
}

// Measures how far the periapsis moves per orbit.
double periapsisShift(Gravity gravity, double apoapsis, double speedFactor) {
    const double v = circularOrbitSpeed(apoapsis, gravity) * speedFactor;
    Particle p = makeParticle({apoapsis, 0.0}, {0.0, v});

    std::vector<double> angles;
    double prevR = p.pos.length();
    bool falling = true;
    const double dt = 0.05;
    for (int i = 0; i < 2000000 && angles.size() < 2; ++i) {
        advanceParticle(p, dt, gravity, 1e4, 1e9);
        const double r = p.pos.length();
        if (falling && r > prevR) {
            angles.push_back(std::atan2(p.pos.y, p.pos.x));
            falling = false;
        } else if (!falling && r < prevR) {
            falling = true;
        }
        prevR = r;
    }
    if (angles.size() < 2) return std::nan("");
    double shift = angles[1] - angles[0];
    while (shift < 0) shift += 2.0 * kPi;
    while (shift >= 2.0 * kPi) shift -= 2.0 * kPi;
    if (shift > kPi) shift -= 2.0 * kPi;
    return shift;
}

// Exact relativistic periapsis shift of an orbit starting at apoapsis `apo`
// with tangential speed `v`, from the orbit equation
// (du/dphi)^2 = E^2/h^2 - (1 - 2u)(1/h^2 + u^2),  u = 1/r.
double exactPeriapsisShift(double apo, double v) {
    const double h = apo * v;
    const double e2 = (1.0 - 2.0 / apo) * (1.0 + h * h / (apo * apo));
    auto f = [&](double u) { return e2 / (h * h) - (1.0 - 2.0 * u) * (1.0 / (h * h) + u * u); };

    // Periapsis: the next root of f after u1 = 1/apo.
    const double u1 = 1.0 / apo;
    double lo = u1 * 1.0001, hi = 1.0 / kPhotonSphere;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        (f(mid) > 0.0 ? lo : hi) = mid;
    }
    const double u2 = lo;

    // Integrate dphi = du / sqrt(f) with u = u1 + (u2 - u1)(1 - cos t)/2,
    // which removes the square-root singularities at the turning points.
    const int n = 200000;
    double halfOrbit = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = (i + 0.5) * kPi / n;
        const double u = u1 + (u2 - u1) * (1.0 - std::cos(t)) / 2.0;
        halfOrbit += (u2 - u1) / 2.0 * std::sin(t) / std::sqrt(std::max(f(u), 1e-300)) * kPi / n;
    }
    return 2.0 * halfOrbit - 2.0 * kPi;
}

void testPrecession() {
    const double apo = 100.0, factor = 0.85;
    const double v = circularOrbitSpeed(apo, Gravity::Relativistic) * factor;
    const double expected = exactPeriapsisShift(apo, v);
    const double got = periapsisShift(Gravity::Relativistic, apo, factor);
    check(std::abs(got - expected) / expected < 0.005, "relativistic periapsis precession",
          fmt("got %.5f rad/orbit, exact %.5f", got, expected));

    const double newton = periapsisShift(Gravity::Newtonian, apo, factor);
    check(std::abs(newton) < 1e-3, "no precession with Newtonian gravity",
          fmt("got %.2e rad/orbit", newton));
}

void testInstabilityInsideIsco() {
    // A slightly disturbed orbit at r = 5M (inside the ISCO) falls in,
    // while the same disturbance at r = 8M stays bounded.
    auto run = [](double r) {
        const double v = circularOrbitSpeed(r, Gravity::Relativistic) * 0.99;
        Particle p = makeParticle({r, 0.0}, {0.0, v});
        for (int i = 0; i < 4000 && p.fate == Fate::Active; ++i) {
            advanceParticle(p, 1.0, Gravity::Relativistic, 1000.0, 1e9);
        }
        return p.fate;
    };
    check(run(5.0) == Fate::Captured, "orbit inside the ISCO plunges", "r = 5M, 1% slower");
    check(run(8.0) == Fate::Active, "orbit outside the ISCO is stable", "r = 8M, 1% slower");
}

void testCircularSpeed() {
    check(std::isnan(circularOrbitSpeed(2.5, Gravity::Relativistic)),
          "no circular orbits inside the photon sphere", "r = 2.5M");
    check(std::abs(circularOrbitSpeed(1e6, Gravity::Relativistic) -
                   circularOrbitSpeed(1e6, Gravity::Newtonian)) < 1e-8,
          "far away, relativity matches Newton", "r = 10^6 M");
}

void testTrail() {
    Trail t(3);
    for (int i = 1; i <= 5; ++i) t.push({static_cast<double>(i), 0.0});
    check(t.size() == 3 && t[0].x == 3.0 && t.newest().x == 5.0, "trail keeps the newest points",
          fmt("oldest %.0f, newest %.0f", t[0].x, t.newest().x));
}

}  // namespace

int main() {
    testLightDeflection();
    testCaptureThreshold();
    testPhotonSphere();
    testCircularOrbit(Gravity::Relativistic, "stable relativistic circular orbit at r = 10M");
    testCircularOrbit(Gravity::Newtonian, "stable Newtonian circular orbit at r = 10M");
    testPrecession();
    testInstabilityInsideIsco();
    testCircularSpeed();
    testTrail();

    if (failures == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) failed.\n", failures);
    return 1;
}
