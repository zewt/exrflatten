#ifndef Stroke_H
#define Stroke_H

#include <functional>
#include <memory>
#include <OpenEXR/ImathVec.h>
#include "DeepImage.h"

using namespace std;

class DeepImage;
class SimpleImage;

namespace DeepImageStroke
{
    struct Config
    {
	void ParseOptionsString(string options);

	int objectId = 0;
	int outputObjectId = -1;
	float radius = 1.0f;

	string strokeMaskChannel, intersectionMaskChannel;

	// The distance to fade out the stroke outside of radius.  If radius is 1 and
	// fade is 5, the stroke will be solid for 1 pixel and then fade out over the
	// next 5 pixels.
	float fade = 1.0f;
	float pushTowardsCamera = 1.0f;
	Imath::V4f strokeColor = {0,0,0,1};

	bool strokeIntersections = false;
	float intersectionMinDepth = 1.0f;
	float intersectionFade = 1.0f;
    };

    // Calculate euclidean distance from each pixel to the nearest pixel where GetMask returns 0.
    void CalculateDistance(
	int width, int height,
	function<float(int x, int y)> GetMask,
	function<void(int x, int y, int sx, int sy, float distance)> PutResult);

    // Return the alpha value to draw a stroke, given the distance to the nearest pixel in
    // the shape and the radius of the stroke.
    float DistanceAndRadiusToAlpha(float distance, const Config &config);

    void AddStroke(const Config &config, shared_ptr<DeepImage> image);
    shared_ptr<SimpleImage> CreateIntersectionMask(const DeepImageStroke::Config &config,
	shared_ptr<const DeepImage> image, shared_ptr<const TypedDeepImageChannel<float>> imageMask);
    void ApplyStrokeUsingMask(const DeepImageStroke::Config &config, shared_ptr<DeepImage> image, shared_ptr<SimpleImage> mask);
}

#endif
