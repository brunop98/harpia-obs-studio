#pragma once

// Install the OpenGL effect engine behind GpuFx.hpp's hook. Called once at
// startup; from then on every thread that composes gets its own engine the
// first time it asks, or none (and the CPU path) if GL is unusable there.
namespace harpia {
void installGpuFx();
} // namespace harpia
