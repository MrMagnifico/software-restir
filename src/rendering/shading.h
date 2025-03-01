#pragma once
#include <framework/ray.h>

#include <utils/common.h>

constexpr float REFLECTION_EPSILON = 1E-3F;

// Compute the shading at the intersection point using the Phong model.
const glm::vec3 computeShading(const glm::vec3& lightPosition, const glm::vec3& lightColor, const Features& features, Ray ray, HitInfo hitInfo);

// Given a ray and a normal (in hitInfo), compute the reflected ray in the specular direction (mirror direction).
const Ray computeReflectionRay(Ray ray, HitInfo hitInfo);