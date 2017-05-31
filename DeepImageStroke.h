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

	// The minimum number of pixels that can be covered by one world space unit before
	// we begin to scale intersectionMinDistance up to compensate for the low resolution.
	// At 5, we want at least 5 pixels per cm.
	//
	// This default is intended for cm.  If worldSpaceScale is 100 for meters, this will be
	// scaled to 500, giving a minimum of 500 pixels per meter.
	float minPixelsPerCm = 5;

	bool strokeIntersections = false;
	float intersectionMinDistance = 1.0f;
	float intersectionFade = 1.0f;

	string saveIntersectionMask;
    };

    // Calculate euclidean distance from each pixel to the nearest pixel where GetMask returns 0.
    void CalculateDistance(
	int width, int height,
	function<float(int x, int y)> GetMask,
	function<void(int x, int y, int sx, int sy, float distance)> PutResult);

    // Return the alpha value to draw a stroke, given the distance to the nearest pixel in
    // the shape and the radius of the stroke.
    float DistanceAndRadiusToAlpha(float distance, const Config &config);

    shared_ptr<SimpleImage> CreateIntersectionMask(const DeepImageStroke::Config &config,
	shared_ptr<const DeepImage> image, shared_ptr<const TypedDeepImageChannel<float>> imageMask);
    void ApplyStrokeUsingMask(const DeepImageStroke::Config &config, shared_ptr<DeepImage> image, shared_ptr<SimpleImage> mask);
}

// Use DeepImageStroke to add a stroke.
#include "EXROperation.h"
class EXROperation_Stroke: public EXROperation
{
public:
    EXROperation_Stroke(const SharedConfig &sharedConfig, string opt, vector<pair<string,string>> args);
    void Run(shared_ptr<DeepImage> image) const;
    void AddChannels(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const;

private:
    void AddStroke(const DeepImageStroke::Config &config, shared_ptr<DeepImage> image) const;

    const SharedConfig &sharedConfig;
    DeepImageStroke::Config strokeDesc;
};

#endif
