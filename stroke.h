#ifndef Stroke_H
#define Stroke_H

#include <functional>

namespace Stroke
{
    // Calculate euclidean distance from each pixel to the nearest pixel where GetMask returns 0.
    void CalculateDistance(
	int width, int height,
	std::function<float(int x, int y)> GetMask,
	std::function<void(int x, int y, int sx, int sy, float distance)> PutResult);

    // Return the alpha value to draw a stroke, given the distance to the nearest pixel in
    // the shape and the radius of the stroke.
    float DistanceAndRadiusToAlpha(float distance, float radius);
}

#endif
