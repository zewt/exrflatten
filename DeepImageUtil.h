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
    // If color is null, the mask will be flattened against white.
    //
    // If objectIds isn't empty, only samples from that ID are included.  (If
    // it's empty, id won't be used and can be null.)
    //
    // Samples will be composited in sample order.  To composite in depth order,
    // sort first with SortSamplesByDepth.
    //
    // Samples can exist in a deep image that are partially or even completely
    // obscured by other samples.  There are two ways we can handle this:
    //
    // If CollapseMode is Normal, the samples will be composited normally: selected
    // samples will be blended, and samples from other objectIds will be ignored
    // entirely.
    // 
    // If CollapseMode is Visibility, excluded samples will still apply their alpha.
    // This means that if an object is covered by a 75% opacity plane, and we're
    // excluding the plane, the object will still be 25% opacity.  This is useful
    // for creating masks.
    enum CollapseMode
    {
        CollapseMode_Normal,
        CollapseMode_Visibility,
    };
    shared_ptr<SimpleImage> CollapseEXR(
	    shared_ptr<const DeepImage> image,
	    shared_ptr<const TypedDeepImageChannel<uint32_t>> id,
	    shared_ptr<const TypedDeepImageChannel<Imath::V4f>> color,
	    shared_ptr<const TypedDeepImageChannel<float>> mask = nullptr,
	    set<int> objectIds = {},
            CollapseMode mode = CollapseMode_Normal);

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
	shared_ptr<const DeepImageChannelProxy> alpha,
	shared_ptr<const TypedDeepImageChannel<uint32_t>> id,
	int objectId,
	shared_ptr<SimpleImage> layer);

    // Return the visibility of each sample at the given pixel.
    //
    // Each value in the result is the visibility of that sample.  For example, if RGBA
    // samples are:
    //
    //         R    G    B    A 
    // s[0] =  1    1    1    1.0
    // s[1] =  0.25 0.25 0.25 0.25
    //
    // then 25% of s[0] is covered by s[1], and s[1] isn't covered by anything, so the
    // result is [0.75, 1.0].  Each sample can be multiplied by its visibility to get
    // the final contribution:
    // 
    // s[0] * 0.75 = [0.75, 0.75, 0.75, 0.75]
    // s[1] * 1.0  = [0.25, 0.25, 0.25, 0.25]
    //
    // These are the final values that would be added together during normal composition.
    // This allows getting the actual contribution of each sample by itself.
    //
    // This works for additive samples.  For example:
    //
    // s[0] =  1    1    1    0
    // s[1] =  0.25 0.25 0.25 0.25
    //
    // Here, s[0] has zero alpha, so it adds 1.  The visibility values are the same as
    // above, [0.75, 1.0].
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

