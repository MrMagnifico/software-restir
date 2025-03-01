#include "render_utils.h"

#include <framework/trackball.h>

#include <post_processing/tone_mapping.h>
#include <scene/light.h>
#include <utils/progressbar.hpp>
#include <utils/utils.h>

#include <algorithm>
#include <random>

PrimaryHitGrid genPrimaryRayHits(const Scene& scene, const Trackball& camera, const EmbreeInterface& embreeInterface, const Screen& screen, const Features& features) {
    glm::ivec2 windowResolution = screen.resolution();
    PrimaryHitGrid primaryHits(windowResolution.y, std::vector<RayHit>(windowResolution.x));

    progressbar progressbar(windowResolution.y);
    std::cout << "Primary rays computation..." << std::endl;
    #ifdef NDEBUG
    #pragma omp parallel for schedule(guided)
    #endif
    for (int y = 0; y < windowResolution.y; y++) {
        for (int x = 0; x != windowResolution.x; x++) {
            const glm::vec2 normalizedPixelPos { float(x) / float(windowResolution.x) * 2.0f - 1.0f,
                                                 float(y) / float(windowResolution.y) * 2.0f - 1.0f };
            primaryHits[y][x].ray = camera.generateRay(normalizedPixelPos); 
            embreeInterface.closestHit(primaryHits[y][x].ray, primaryHits[y][x].hit);
        }
        #pragma omp critical
        progressbar.update();
    }
    std::cout << std::endl;
    return primaryHits;
}

ReservoirGrid genInitialSamples(const PrimaryHitGrid& primaryHits, const Scene& scene, const EmbreeInterface& embreeInterface, const Features& features, const glm::ivec2& windowResolution) {
    ReservoirGrid initialSamples(windowResolution.y, std::vector<Reservoir>(windowResolution.x, Reservoir(features.numSamplesInReservoir)));
    progressbar progressbar(windowResolution.y);
    std::cout << "Initial light samples generation..." << std::endl;
    #ifdef NDEBUG
    #pragma omp parallel for schedule(guided)
    #endif
    for (int y = 0; y < windowResolution.y; y++) {
        for (int x = 0; x != windowResolution.x; x++) {
            initialSamples[y][x] = genCanonicalSamples(scene, embreeInterface, features, primaryHits[y][x]);
        }
        #pragma omp critical
        progressbar.update();
    }
    std::cout << std::endl;
    return initialSamples;
}

glm::vec3 finalShading(const Reservoir& reservoir, const Ray& primaryRay, const EmbreeInterface& embreeInterface, const Features& features) {
    glm::vec3 finalColor(0.0f);
    for (const SampleData& sample : reservoir.outputSamples) {
        glm::vec3 sampleColor   = testVisibilityLightSample(sample.lightSample.position, embreeInterface, features, primaryRay, reservoir.hitInfo)              ?
                                  computeShading(sample.lightSample.position, sample.lightSample.color, features, primaryRay, reservoir.hitInfo)    :
                                  glm::vec3(0.0f);
        sampleColor             *= sample.outputWeight;
        finalColor              += sampleColor;
    }
    finalColor /= reservoir.outputSamples.size(); // Divide final shading value by number of samples
    return finalColor;
}

// Average iterations and write tone-mapped value to screen
void combineToScreen(Screen& screen, const PixelGrid& finalPixelColors, const Features& features) {
    glm::ivec2 windowResolution = screen.resolution();
    std::cout << "Iteration combination..." << std::endl;
    progressbar progressbarPixels(static_cast<int32_t>(windowResolution.y));
    #ifdef NDEBUG
    #pragma omp parallel for schedule(guided)
    #endif
    for (int y = 0; y < windowResolution.y; y++) {
        for (int x = 0; x != windowResolution.x; x++) {
            glm::vec3 finalColor = finalPixelColors[y][x] / static_cast<float>(features.maxIterationsMIS);
            if (features.enableToneMapping) { finalColor = exposureToneMapping(finalColor, features); }
            screen.setPixel(x, y, finalColor);
        }
        #pragma omp critical
        progressbarPixels.update();
    }
    std::cout << std::endl;
}

void spatialReuse(ReservoirGrid& reservoirGrid, const EmbreeInterface& embreeInterface, const Screen& screen, const Features& features) {
    // Uniform selection of neighbours in N pixel Manhattan distance radius
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distr(-static_cast<int32_t>(features.spatialResampleRadius), features.spatialResampleRadius);

    std::cout << "Spatial reuse..." << std::endl;
    glm::ivec2 windowResolution = screen.resolution();
    ReservoirGrid prevIteration = reservoirGrid;
    for (uint32_t pass = 0U; pass < features.spatialResamplingPasses; pass++) {
        std::cout << "Pass " << pass + 1 << std::endl;
        progressbar progressBarPixels(windowResolution.y);
        #ifdef NDEBUG
        #pragma omp parallel for schedule(guided)
        #endif
        for (int y = 0; y < windowResolution.y; y++) {
            for (int x = 0; x != windowResolution.x; x++) {
                // Select candidates
                std::vector<Reservoir> selected;
                selected.reserve(features.numNeighboursToSample + 1U); // Reserve memory needed for maximum possible number of samples (neighbours + current)
                Reservoir& current = reservoirGrid[y][x];
                for (uint32_t neighbourCount = 0U; neighbourCount < features.numNeighboursToSample; neighbourCount++) {
                    int neighbourX              = std::clamp(x + distr(gen), 0, windowResolution.x - 1);
                    int neighbourY              = std::clamp(y + distr(gen), 0, windowResolution.y - 1);
                    Reservoir neighbour         = prevIteration[neighbourY][neighbourX]; // Create copy for local modification
                    
                    // Conduct heuristic check if biased combination is used
                    if (!features.unbiasedCombination) { 
                        float depthFracDiff     = std::abs(1.0f - (neighbour.cameraRay.t / current.cameraRay.t));   // Check depth difference (greater than 10% leads to rejection) 
                        float normalsDotProd    = glm::dot(neighbour.hitInfo.normal, current.hitInfo.normal);       // Check normal difference (greater than 25 degrees leads to rejection)
                        if (depthFracDiff > 0.1f || normalsDotProd < 0.90630778703f) { continue; } 
                    }
                    
                    selected.push_back(neighbour);
                }

                // Ensure pixel's own reservoir is also considered
                selected.push_back(current);

                // Combine to single reservoir (biased or unbiased depending on user selection)
                Reservoir combined(current.outputSamples.size());
                combined.cameraRay  = current.cameraRay;
                combined.hitInfo    = current.hitInfo;
                if (features.unbiasedCombination)   { Reservoir::combineUnbiased(selected, combined, embreeInterface, features); }
                else                                { Reservoir::combineBiased(selected, combined, features); }
                reservoirGrid[y][x] = combined;
            }
            #pragma omp critical
            progressBarPixels.update();
        }
        std::cout << std::endl;
        prevIteration = reservoirGrid;
    }
}

void temporalReuse(ReservoirGrid& reservoirGrid, ReservoirGrid& previousFrameGrid, const EmbreeInterface& embreeInterface,
                   Screen& screen, const Features& features) {
    glm::ivec2 windowResolution = screen.resolution();

    std::cout << "Temporal reuse..." << std::endl;
    progressbar progressBarPixels(windowResolution.y);
    #ifdef NDEBUG
    #pragma omp parallel for schedule(guided)
    #endif
    for (int y = 0; y < windowResolution.y; y++) {
        for (int x = 0; x != windowResolution.x; x++) {
            // Clamp M and wSum values to a user-defined multiple of the current frame's to bound temporal creep
            Reservoir& current              = reservoirGrid[y][x];
            Reservoir& temporalPredecessor  = previousFrameGrid[y][x];
            size_t multipleCurrentM         = (features.temporalClampM * current.totalSampleNums()) + 1ULL;
            if (temporalPredecessor.totalSampleNums() > multipleCurrentM) {
                for (size_t reservoirIdx = 0ULL; reservoirIdx < temporalPredecessor.outputSamples.size(); reservoirIdx++) {
                    if (temporalPredecessor.sampleNums[reservoirIdx] == 0ULL) { continue; } // Samples processed by this reservoir might be zero
                    temporalPredecessor.wSums[reservoirIdx]         *= multipleCurrentM / temporalPredecessor.sampleNums[reservoirIdx];
                    temporalPredecessor.sampleNums[reservoirIdx]    = multipleCurrentM;
                }
            }

            // Combine to single reservoir
            Reservoir combined(current.outputSamples.size());
            combined.cameraRay                              = current.cameraRay;
            combined.hitInfo                                = current.hitInfo;
            std::array<Reservoir, 2ULL> pixelAndPredecessor = { current, temporalPredecessor };
            Reservoir::combineBiased(pixelAndPredecessor, combined, features); // Samples from temporal predecessor should be visible, no need to do unbiased combination
            reservoirGrid[y][x]                             = combined;
        }
        #pragma omp critical
        progressBarPixels.update();
    }
    std::cout << std::endl;
}

float generalisedBalanceHeuristic(const LightSample& sample, const std::vector<Reservoir>& allPixels,
                                  const Ray& primaryRay, const HitInfo& primaryHitInfo,
                                  const Features& features) {
    // Redundant computation in denominator, but sufficient for a quick and dirty prototype
    float numerator     = targetPDF(sample, primaryRay, primaryHitInfo, features);
    float denominator   = std::numeric_limits<float>::min();
    for (const Reservoir& pixel : allPixels) { denominator += targetPDF(sample, pixel.cameraRay, pixel.hitInfo, features); }
    return numerator / denominator;
}

void visualiseAlphas(const MatrixGrid& techniqueMatrices,
                     const VectorGrid& contributionVectorsRed,
                     const VectorGrid& contributionVectorsGreen,
                     const VectorGrid& contributionVectorsBlue,
                     const glm::ivec2& windowResolution, const Features& features) {
    constexpr glm::vec3 purePositiveColor   = { 1.0f, 0.5f, 0.0f };
    constexpr glm::vec3 pureNegativeColor   = { 0.0f, 0.5f, 1.0f };
    constexpr glm::vec3 zeroColor           = { 0.0f, 0.0f, 0.0f };

    // Generate images
    const uint32_t totalDistributions = features.numNeighboursToSample + 1U;
    std::vector<Screen> images(totalDistributions * 3, Screen(windowResolution, false));
    std::cout << "Saving alpha visualisations..." << std::endl;
    progressbar progressbarPixels(static_cast<int32_t>(windowResolution.y));
    #ifdef NDEBUG
    #pragma omp parallel for schedule(guided)
    #endif
    for (int y = 0; y < windowResolution.y; y++) {
        for (int x = 0; x != windowResolution.x; x++) {
            // Compute weighted contributions to final integral value for each color component 
            std::array<Eigen::VectorXf, 3> integralComponents = { solveSystem(techniqueMatrices[y][x], contributionVectorsRed[y][x]),
                                                                  solveSystem(techniqueMatrices[y][x], contributionVectorsGreen[y][x]),
                                                                  solveSystem(techniqueMatrices[y][x], contributionVectorsBlue[y][x]) };

            // Set color for each technique for each color channel
            for (uint32_t neighbourIdx = 0U; neighbourIdx < totalDistributions; neighbourIdx++) {
                for (uint32_t colour = 0U; colour < 3U; colour++) {
                    float alphaVal      = integralComponents[colour](neighbourIdx);
                    glm::vec3 visColor  = alphaVal > 0.0f                                   ?
                                          glm::mix(zeroColor, purePositiveColor, alphaVal)  :
                                          glm::mix(zeroColor, pureNegativeColor, -alphaVal);
                    images[(neighbourIdx * 3U) + colour].setPixel(x, y, visColor);
                }
            }
        }
        #pragma omp critical
        progressbarPixels.update();
    }
    std::cout << std::endl;

    // Save images to timestamped folder
    std::string time                    = currentTime();
    std::filesystem::path renderDirPath = RENDERS_DIR;
    std::filesystem::path visDirPath    = renderDirPath / time;
    if (!std::filesystem::exists(renderDirPath))    { std::filesystem::create_directory(renderDirPath); }
    if (!std::filesystem::exists(visDirPath))       { std::filesystem::create_directory(visDirPath); }
    for (uint32_t neighbourIdx = 0U; neighbourIdx < totalDistributions; neighbourIdx++) {
        for (uint32_t colour = 0U; colour < 3U; colour++) {
            Screen& visScreen                   = images[(neighbourIdx * 3U) + colour];
            auto colorName                      = magic_enum::enum_name(magic_enum::enum_value<Color>(colour));
            std::filesystem::path visFilePath   = visDirPath / std::format("Distribution {} - {}.bmp", neighbourIdx, colorName);
            visScreen.writeBitmapToFile(visFilePath);
        }
    }
}

float arbitraryUnbiasedContributionWeightReciprocal(const LightSample& sample, const Reservoir& pixel, const Scene& scene,
                                                    size_t sampleIdx,
                                                    const Features& features) {
    float targetPdfValue = targetPDF(sample, pixel.cameraRay, pixel.hitInfo, features);
    if (targetPdfValue == 0.0f) { return 0.0f; } // If target function value is zero, theoretical normalised PDF would also be zero

    // Compute mock unbiased contribution weight
    float mockSampleWeight  = targetPdfValue / (1.0f / scene.lights.size()); // Samples all generated via uniform light sampling, so equal original PDF
    float arbitraryWeight   = (1.0f / targetPdfValue) *
                              (1.0f / pixel.sampleNums[sampleIdx]) * // Account for MIS weights in unbiased contribution because that's how it is in the rest of the codebas
                              (pixel.wSums[sampleIdx] - pixel.chosenSampleWeights[sampleIdx] + mockSampleWeight); // Emulate replacing weight of chosen sample with the given sample
    return (1.0f / arbitraryWeight);
}
