#ifndef Stroke_H
#define Stroke_H

#include <functional>
#include <memory>
#include <OpenEXR/ImathVec.h>
#include "DeepImage.h"
#include "EXROperation.h"

using namespace std;

class DeepImage;
class SimpleImage;

namespace DeepImageStroke
{
    struct Config
    {
	set<int> objectIds;
	int outputObjectId = 0;
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

	bool strokeOutline = true;

	// If intersectionsUseDistance and/or intersectionsUseNormals are disabled, we'll only use
	// the other, and we won't require the corresponding P or N input channel.  This is mostly
	// for troubleshooting, since you can turn one off and output the mask to see what's happening.
	bool strokeIntersections = false;
	bool intersectionsUseDistance = true;
	float intersectionMinDistance = 1.0f;
	float intersectionFade = 1.0f;
	bool intersectionsUseNormals = true;
	float intersectionAngleThreshold = 25.0f;
	float intersectionAngleFade = 10.0f;

	string saveIntersectionPattern;
    };

    // Return the alpha value to draw a stroke, given the distance to the nearest pixel in
    // the shape and the radius of the stroke.
    float DistanceAndRadiusToAlpha(float distance, const Config &config);

    shared_ptr<SimpleImage> CreateIntersectionPattern(
        const DeepImageStroke::Config &config, const SharedConfig &sharedConfig,
	shared_ptr<const DeepImage> image,
	shared_ptr<const TypedDeepImageChannel<float>> strokeMask,
	shared_ptr<const TypedDeepImageChannel<float>> intersectionMask);
    void ApplyStrokeUsingMask(const DeepImageStroke::Config &config, const SharedConfig &sharedConfig,
	shared_ptr<const DeepImage> image, shared_ptr<DeepImage> outputImage,
	shared_ptr<SimpleImage> mask);
}

// Use DeepImageStroke to add a stroke.
class EXROperation_Stroke: public EXROperation
{
public:
    EXROperation_Stroke(const SharedConfig &sharedConfig, string opt, vector<pair<string,string>> args);
    void Run(shared_ptr<EXROperationState> state) const;
    void AddChannels(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const;

private:
    void AddStroke(const DeepImageStroke::Config &config, shared_ptr<const DeepImage> image, shared_ptr<DeepImage> outputImage) const;

    const SharedConfig &sharedConfig;
    DeepImageStroke::Config strokeDesc;
};

#endif
