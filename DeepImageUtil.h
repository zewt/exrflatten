#ifndef DeepImageUtil_H
#define DeepImageUtil_H

#include <memory>
#include <set>
using namespace std;

#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImathVec.h>

#include "DeepImage.h"
class SimpleImage;

namespace DeepImageUtil {
    const int NO_OBJECT_ID = 0;

    // The layer API in OpenEXR is awkward.  This is a helper to simply get the
    // names of channels in a layer.
    vector<string> GetChannelsInLayer(const Imf::Header &header, string layerName);

    // Flatten the color channels of a deep EXR to a simple flat layer.
    shared_ptr<SimpleImage> CollapseEXR(shared_ptr<const DeepImage> image, shared_ptr<const TypedDeepImageChannel<float>> mask = nullptr, set<int> objectIds = {});

    // Change all samples with an object ID of fromObjectId to intoObjectId.
    void CombineObjectId(shared_ptr<TypedDeepImageChannel<uint32_t>> id, int fromObjectId, int intoObjectId);

    // Copy all image attributes from one header to another, except for built-in EXR headers that
    // we shouldn't set.
    void CopyLayerAttributes(const Imf::Header &input, Imf::Header &output);

    // Sort samples based on the depth of each pixel, furthest from the camera first.
    void SortSamplesByDepth(shared_ptr<DeepImage> image);

    /* Separate a simple composited layer from a DeepImage. */
    void SeparateLayer(
	shared_ptr<const DeepImage> image,
	shared_ptr<const TypedDeepImageChannel<uint32_t>> id,
	int objectId,
	shared_ptr<SimpleImage> layer,
	const map<int,int> &layerOrder,
	shared_ptr<const TypedDeepImageChannel<float>> mask = nullptr);

    // Create a layer from an object ID and a mask.
    //
    // If alphaMask is false, the mask will be on the color channels and alpha
    // will be 1.  This is the way most people are used to dealing with masks.
    //
    // If alphaMask is true, the mask will be black and on the alpha channel.  This
    // is awkward, but it's the only way to use masks with Photoshop clipping masks.
    //
    // If compositeAlpha is true, the mask values will be composited with the alpha
    // value of the sample.  If false, only the sample nearest to the camera will be
    // used.
    void ExtractMask(
	bool alphaMask,
	bool compositeAlpha,
	shared_ptr<const TypedDeepImageChannel<float>> mask,
	shared_ptr<const TypedDeepImageChannel<Imath::V4f>> rgba,
	shared_ptr<const TypedDeepImageChannel<uint32_t>> id,
	int objectId,
	shared_ptr<SimpleImage> layer);

    // Return the final visibility of each sample at the given pixel.
    //
    // If a sample has three pixels with alpha 1.0, 0.5 and 0.5, the first sample is covered by the
    // samples on top of it, and the final visibility is { 0.25, 0.25, 0.5 }.
    vector<float> GetSampleVisibility(shared_ptr<const DeepImage> image, int x, int y);
    void GetSampleVisibilities(shared_ptr<const DeepImage> image, Imf::Array2D<vector<float>> &SampleVisibilities);

    // Copy all samples from all channels of images into a single image.
    shared_ptr<DeepImage> CombineImages(vector<shared_ptr<DeepImage>> images);

    /* Arnold has a bug: it premultiplies a lot of EXR channels by alpha that it shouldn't.
     * Only color channels should be multiplied by alpha, but it premultiplies world space
     * positions, simple float channels, etc.  Undo this. */
    template<typename T>
    void UnpremultiplyChannel(shared_ptr<const TypedDeepImageChannel<Imath::V4f>> rgba, shared_ptr<TypedDeepImageChannel<T>> channel);
}

template<typename T>
void DeepImageUtil::UnpremultiplyChannel(shared_ptr<const TypedDeepImageChannel<Imath::V4f>> rgba, shared_ptr<TypedDeepImageChannel<T>> channel)
{
    for(int y = 0; y < channel->height; y++)
    {
	for(int x = 0; x < channel->width; x++)
	{
	    const V4f *rgbaSamples = rgba->GetSamples(x, y);
	    T *channelSamples = channel->GetSamples(x, y);
	    for(int s = 0; s < channel->sampleCount[y][x]; ++s)
	    {
		float a = rgbaSamples[s][3];
		if(a > 0.00001f)
		    channelSamples[s] /= a;
	    }
	}
    }
}

#endif

