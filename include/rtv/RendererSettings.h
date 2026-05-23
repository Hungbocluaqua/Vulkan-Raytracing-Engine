#pragma once

#include "rtv/RendererDebug.h"

#include <cstdint>

namespace rtv {

struct RendererSettings {
    bool pathTracingEnabled = true;
    bool cameraJitterEnabled = true;
    bool denoiserEnabled = true;
    bool denoiseWhileMoving = false;
    bool taaEnabled = true;
    float taaFeedback = 0.08f;
    bool sunlightEnabled = true;
    bool directLightingEnabled = true;
    bool environmentEnabled = true;
    uint32_t maxBounces = 8;
    uint32_t atrousIterations = 4;
    uint32_t environmentDirectSamples = 1;
    float denoiserStrength = 1.0f;
    float sunIntensity = 1.0f;
    float skyIntensity = 0.8f;
    float sunElevation = 0.97f;
    ToneMapper toneMapper = ToneMapper::ACES;
    float exposure = 2.0f;
    float gamma = 2.2f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float brightness = 0.0f;
    float whitePoint = 4.0f;
    bool autoExposureEnabled = false;
    float targetLuminance = 0.18f;
    float minExposure = 0.25f;
    float maxExposure = 8.0f;
    float adaptationSpeed = 2.0f;
    float histogramMinLogLuminance = -10.0f;
    float histogramMaxLogLuminance = 10.0f;
    float histogramLowPercentile = 0.05f;
    float histogramHighPercentile = 0.95f;
    float histogramTargetPercentile = 0.60f;
    float sunAngularRadius = 0.0093f;
    float rayleighScaleHeight = 8000.0f;
    float mieScaleHeight = 1200.0f;
    float mieAnisotropy = 0.8f;
    float groundAlbedo = 0.3f;
    float indirectStrength = 1.0f;
    RestirMode restirMode = RestirMode::ClassicNee;
    float environmentIntensity = 1.0f;
    float environmentRotation = 0.0f;
    float environmentBackgroundIntensity = 0.35f;
    float renderResolutionScale = 1.0f;
    uint32_t accumulationLimit = 0;
    RendererDebugView debugView = RendererDebugView::Beauty;
    float debugScale = 1.0f;

    bool usePhysicalCamera = false;
    float physicalAperture = 16.0f;
    float physicalShutterSeconds = 1.0f / 125.0f;
    float physicalIso = 100.0f;
    float physicalExposureCompensation = 0.0f;
};

} // namespace rtv
