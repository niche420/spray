#version 460
#extension GL_EXT_ray_tracing : require

// Capture.miss.rmiss -- runs once per ray that doesn't hit any geometry in
// the TLAS. See Capture.raygen.rgen for the traceRayEXT call this responds
// to and the shared payload's meaning.

layout(location = 0) rayPayloadInEXT vec3 hitColor;

void main()
{
    // Matches App::RenderFrame's raster clear color (0.05, 0.05, 0.08) so a
    // capture's background is visually consistent with the raster viewport
    // -- not meaningful for training (background pixels are typically
    // masked out downstream), just keeps side-by-side comparisons sane.
    hitColor = vec3(0.05, 0.05, 0.08);
}