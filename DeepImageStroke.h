#ifndef Stroke_H
#define Stroke_H

#include <functional>
#include <memory>
#include <OpenEXR/ImathVec.h>

using namespace std;

class DeepImage;

namespace DeepImageStroke
{
    // Calculate euclidean distance from each pixel to the nearest pixel where GetMask returns 0.
    void CalculateDistance(
	int width, int height,
	function<float(int x, int y)> GetMask,
	function<void(int x, int y, int sx, int sy, float distance)> PutResult);

    // Return the alpha value to draw a stroke, given the distance to the nearest pixel in
    // the shape and the radius of the stroke.
    float DistanceAndRadiusToAlpha(float distance, float radius);


    struct Config
    {
	int objectId = 0;
	float radius = 1.0f;
	float pushTowardsCamera = 1.0f;
	Imath::V3f strokeColor = {0,0,0};
    };

    void AddStroke(const Config &config, shared_ptr<DeepImage> image);
}

#endif
