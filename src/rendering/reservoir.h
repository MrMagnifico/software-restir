#pragma once
#ifndef _RESERVOIR_H_
#define _RESERVOIR_H_

#include <ray_tracing/embree_interface.h>
#include <utils/common.h>

#include <framework/disable_all_warnings.h>
DISABLE_WARNINGS_PUSH()
#include <glm/vec3.hpp>
DISABLE_WARNINGS_POP()
#include <framework/ray.h>

#include <limits>
#include <span>
#include <vector>

struct LightSample {
    glm::vec3 position  = {0.0f, 0.0f, 0.0f},
              color     = {0.0f, 0.0f, 0.0f};
};

struct SampleData {
    LightSample lightSample;
    float outputWeight = 0.0f;
};

struct Reservoir {
    Reservoir(size_t numSamples) : outputSamples(numSamples)
                                 , sampleNums(numSamples, 1ULL)
                                 , wSums(numSamples, std::numeric_limits<float>::min())
                                 , chosenSampleWeights(numSamples, 0.0f) {}

    // Intersection position info
    Ray cameraRay;
    HitInfo hitInfo;

    // Light sampling
    std::vector<SampleData> outputSamples;   
    std::vector<size_t> sampleNums;
    std::vector<float> wSums;
    std::vector<float> chosenSampleWeights;

    /**
     * Process the given sample for potential storage by a sub-reservoir
     * 
     * @param sample Light sample
     * @param weight Selection weight of the sample
     * 
     * @return The index of the sub-reservoir which processed this sample
    */
    size_t update(LightSample sample, float weight);

    size_t totalSampleNums() const;

    /**
     * Combine a number of reservoirs in a single final reservoir in a biased fashion (Algorithm 5 in ReSTIR paper)
     * 
     * @param reservoirs The reservoirs to be combined
     * @param finalReservoir Struct where final combined reservoir data will be stored. Should have the intersection position info of the relevant pixel
     * @param features Features configuration
    */
    static void combineBiased(const std::span<Reservoir>& reservoirStream, Reservoir& finalReservoir, const Features& features);

    /**
     * Combine a number of reservoirs in a single final reservoir in an unbiased fashion (Algorithm 6 in ReSTIR paper)
     * 
     * @param reservoirs The reservoirs to be combined
     * @param finalReservoir Struct where final combined reservoir data will be stored. Should have the intersection position info of the relevant pixel
     * @param features Features configuration
    */
    static void combineUnbiased(const std::span<Reservoir>& reservoirStream, Reservoir& finalReservoir, const EmbreeInterface& embreeInterface, const Features& features);
};

using ReservoirGrid = std::vector<std::vector<Reservoir>>;

float targetPDF(const LightSample& sample, const Ray& cameraRay, const HitInfo& hitInfo, const Features& features);

#endif
