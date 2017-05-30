#include "DeepImageUtil.h"
#include "SimpleImage.h"
#include "helpers.h"

#include <algorithm>
#include <OpenEXR/ImathVec.h>
#include <OpenEXR/ImfChannelList.h>

using namespace Imf;
using namespace Imath;

vector<string> DeepImageUtil::GetChannelsInLayer(const Header &header, string layerName)
{
    // If layerName is a channel name itself, just return it.
    if(header.channels().findChannel(layerName) != NULL)
	return { layerName };

    vector<string> result;
    ChannelList::ConstIterator start, end;
    header.channels().channelsInLayer(layerName, start, end);
    while(start != end)
    {
	result.push_back(start.name());
	++start;
    }

    // OpenEXR layers have a really silly design flaw: they don't specify the order!  We
    // just get them alphabetized.   If you look up a normals channel with N.X, N.Y, N.Z
    // you'll get the right channel order, but if you look up a diffuse channel with
    // C.R, C.G, C.B, you'll get B, G, R.  This means you can't load a vector from a layer
    // name--you have to already know the channel names to set their order, which is
    // incredibly silly.
    //
    // Work around this by having a list of channels and their canonical order, and sorting
    // channels in that order.  This could be optimized to avoid searching channelOrder, but
    // it's not useful since we're sorting arrays of 3 or 4 elements.
    static const vector<string> channelOrder = {
	"R", "G", "B", "Y", "RY", "BY",
	"A", "AR", "AG", "AB",
	"X", "Y", "Z"
    };
    sort(result.begin(), result.end(), [&layerName](string lhs, string rhs) {
	lhs = lhs.substr(layerName.size()+1); // diffuse.G -> G
	rhs = rhs.substr(layerName.size()+1);
	auto lhsIt = find(channelOrder.begin(), channelOrder.end(), lhs);
	auto rhsIt = find(channelOrder.begin(), channelOrder.end(), rhs);
	size_t lhsOrder = distance(channelOrder.begin(), lhsIt);
	size_t rhsOrder = distance(channelOrder.begin(), rhsIt);
	return lhsOrder < rhsOrder;
    });
    return result;
}

// Flatten the color channels of a deep EXR to a simple flat layer.
shared_ptr<SimpleImage> DeepImageUtil::CollapseEXR(shared_ptr<const DeepImage> image, shared_ptr<const TypedDeepImageChannel<float>> mask, set<int> objectIds)
{
    shared_ptr<SimpleImage> result = make_shared<SimpleImage>(image->width, image->height);

    auto rgba = image->GetChannel<V4f>("rgba");
    auto Z = image->GetChannel<float>("Z");
    auto id = image->GetChannel<uint32_t>("id");

    for(int y = 0; y < image->height; y++)
    {
	for(int x = 0; x < image->width; x++)
	{
	    V4f &out = result->GetRGBA(x, y);
	    out = V4f(0,0,0,0);

	    int samples = image->NumSamples(x,y);
	    for(int s = 0; s < samples; ++s)
	    {
		bool IncludeLayer = objectIds.empty() || objectIds.find(id->Get(x,y,s)) != objectIds.end();

		V4f color = rgba->Get(x,y,s);

		if(mask)
		    color *= mask->Get(x,y,s);

		float alpha = color[3];
		for(int channel = 0; channel < 4; ++channel)
		{
		    if(IncludeLayer)
			out[channel] = color[channel] + out[channel]*(1-alpha);
		    else
			out[channel] =                  out[channel]*(1-alpha);
		}
	    }
	}
    }

    return result;
}

// Change all samples with an object ID of fromObjectId to intoObjectId.
void DeepImageUtil::CombineObjectId(shared_ptr<TypedDeepImageChannel<uint32_t>> id, int fromObjectId, int intoObjectId)
{
    for(int y = 0; y < id->height; y++)
    {
	for(int x = 0; x < id->width; x++)
	{
	    for(int s = 0; s < id->sampleCount[y][x]; ++s)
	    {
		uint32_t &thisId = id->Get(x,y,s);
		if(thisId == fromObjectId)
		    thisId = intoObjectId;
	    }
	}
    }
}

void DeepImageUtil::CopyLayerAttributes(const Header &input, Header &output)
{
    for(auto it = input.begin(); it != input.end(); ++it)
    {
	auto &attr = it.attribute();
	string headerName = it.name();
	if(headerName == "channels" ||
	    headerName == "chunkCount" ||
	    headerName == "compression" ||
	    headerName == "lineOrder" ||
	    headerName == "type" ||
	    headerName == "version")
	    continue;

	if(headerName.substr(0, 9) == "ObjectId/")
	    continue;

	output.insert(headerName, attr);
    }
}

void DeepImageUtil::SortSamplesByDepth(shared_ptr<DeepImage> image)
{
    const auto Z = image->GetChannel<float>("Z");

    // Keep these outside the loop, since reallocating these for every pixel is slow.
    vector<int> order;
    vector<pair<int,int>> swaps;
    for(int y = 0; y < image->height; y++)
    {
	for(int x = 0; x < image->width; x++)
	{
	    order.resize(image->sampleCount[y][x]);
	    for(int sample = 0; sample < order.size(); ++sample)
		order[sample] = sample;

	    // Sort samples by depth.
	    const float *depth = Z->GetSamples(x, y);
	    sort(order.begin(), order.end(), [&](int lhs, int rhs)
	    {
		float lhsZNear = depth[lhs];
		float rhsZNear = depth[rhs];
		return lhsZNear > rhsZNear;
	    });

	    make_swaps(order, swaps);
	    if(swaps.empty())
		continue;

	    for(auto it: image->channels)
	    {
		shared_ptr<DeepImageChannel> &channel = it.second;
		channel->Reorder(x, y, swaps);
	    }
	}
    }
}

/*
 * Each pixel in an OpenEXR image can have multiple samples, and each sample can be tagged
 * with a different object ID.  Normally to composite a deep EXR image into a regular image
 * software needs to understand deep samples, to composite each sample, which makes them hard
 * to use in traditional tools like Photoshop.  When you import the image, you just get a flat
 * image and can't manipulate individual objects because the importer has to discard the deep
 * data.
 *
 * Transform samples to a set of regular flattened layers that can be composited with normal
 * "over" compositing.  This still loses deep data (there are a lot of things deep data can do
 * that you can't do with this scheme), but this allows many compositing operations in 2D
 * packages like After Effects and Photoshop to work.
 *
 * The resulting layer order is significant: the layers must be composited in the order specified
 * by layerOrder.  Layers can be hidden from the bottom-up only: if you have layers [1,2,3,4],
 * you can hide 1 or 1 and 2 and get correct output, but you can't hide 3 by itself.
 */
void DeepImageUtil::SeparateLayer(
    shared_ptr<const DeepImage> image,
    shared_ptr<const TypedDeepImageChannel<uint32_t>> id,
    int objectId,
    shared_ptr<SimpleImage> layer,
    const map<int,int> &layerOrder,
    shared_ptr<const TypedDeepImageChannel<float>> mask)
{
    auto rgba = image->GetChannel<V4f>("rgba");

    for(int y = 0; y < image->height; y++)
    {
	for(int x = 0; x < image->width; x++)
	{
	    auto rgba = image->GetChannel<V4f>("rgba");

	    V4f color(0,0,0,0);
	    for(int s = 0; s < image->NumSamples(x, y); ++s)
	    {

		// The layers we're creating are in a fixed order specified by the user, but
		// the samples can be in any order.  We have samples that are supposed to be
		// behind others, but which will actually be in the top layer.
		//
		// Figure out how much opacity is covering us in later layers that's actually
		// supposed to be behind us.  If coveringAlpha is 0.25, we're being covered up
		// by 25% in a later layer that's really behind us.
		float coveringAlpha = 0;
		for(int s2 = 0; s2 <= s; ++s2)
		{
		    float coveringAlphaSample = rgba->Get(x,y,s2)[3];
		    int actualObjectId = id->Get(x, y, s2);

		    // layerCmp < 0 if this sample is in an earlier layer than the one we're outputting,
		    // layerCmp > 0 if this sample is in a later layer.
		    // If this sample is in an earlier layer, ignore it.  If it's in a later layer,
		    // apply it.  If it's in this layer, reduce alpha by 1-a, but don't add it.
		    int layerCmp = layerOrder.at(actualObjectId) - layerOrder.at(objectId);
		    if(layerCmp >= 0) // above or same
			coveringAlpha *= 1-coveringAlphaSample;
		    if(layerCmp > 0) // above
			coveringAlpha += coveringAlphaSample;
		}

		V4f sampleColor = rgba->Get(x,y,s);
		const float &alpha = sampleColor[3];

		// If this sample is part of this layer, composite it in.  If it's not, it still causes this
		// color to become less visible, so apply alpha, but don't add color.
		int layerCmp = layerOrder.at(id->Get(x, y, s)) - layerOrder.at(objectId);
		if(layerCmp > 0)
		    continue;

		if(layerCmp < 0)
		{
		    // This sample is in an earlier layer (comped before this one).
		    color *= 1-alpha;
		    continue;
		}

		// If coveringAlpha is .5, we're being covered 50% in a later layer by
		// things that are supposed to be behind us, so make this pixel 2x more
		// visible.
		if(1-coveringAlpha > 0.00001f)
		    sampleColor /= 1-coveringAlpha;

		color = color*(1-alpha);

		// If we have a mask, multiply rgba by it to get a masked version.
		if(mask != nullptr)
		    sampleColor *= mask->Get(x, y, s);

		color += sampleColor;
	    }

	    // Save the result.
	    layer->GetRGBA(x,y) = color;
	}
    }
}

void DeepImageUtil::ExtractMask(
    bool alphaMask,
    bool compositeAlpha,
    shared_ptr<const TypedDeepImageChannel<float>> mask,
    shared_ptr<const TypedDeepImageChannel<V4f>> rgba,
    shared_ptr<const TypedDeepImageChannel<uint32_t>> id,
    int objectId,
    shared_ptr<SimpleImage> layer)
{
    for(int y = 0; y < rgba->height; y++)
    {
	for(int x = 0; x < rgba->width; x++)
	{
	    float resultValue = 0;
	    if(compositeAlpha)
	    {
		// If compositeAlpha is true, blend the mask like a color value, giving us a
		// composited mask value and its transparency: (mask, alpha).
		V2f result(0,0);
		for(int s = 0; s < rgba->sampleCount[y][x]; ++s)
		{
		    if(id->Get(x, y, s) != objectId)
			continue;

		    float maskValue = mask->Get(x, y, s);
		    float alpha = rgba->Get(x,y,s)[3];
		    result *= 1-alpha;
		    result += V2f(maskValue*alpha, alpha);
		}

		// If the mask value for an object is 1, the mask output should be 1 even if the
		// object is transparent, or else transparency will cause the object to be masked.
		// If the object has alpha 0.5 and a mask of 1, we have (0.5, 0.5).  Divide out
		// alpha to get 1.
		if(result[1] > 0.0001f)
		    result /= result[1];
		resultValue = result[0];
	    }
	    else
	    {
		// If false, just find the nearest sample to the camera that isn't completely
		// transparent.
		for(int s = rgba->sampleCount[y][x]-1; s >= 0; --s)
		{
		    if(id->Get(x, y, s) != objectId)
			continue;

		    float alpha = rgba->Get(x,y,s)[3];
		    if(alpha < 0.00001f)
			continue;

		    resultValue = mask->Get(x, y, s);
		    break;
		}
	    }

	    // Save the result.
	    V4f color(0,0,0,0);
	    if(alphaMask)
		color = V4f(resultValue,resultValue,resultValue,resultValue);
	    else
		color = V4f(resultValue,resultValue,resultValue,1);
	    layer->GetRGBA(x,y) = color;
	}
    }
}

vector<float> DeepImageUtil::GetSampleVisibility(shared_ptr<const DeepImage> image, int x, int y)
{
    vector<float> result;

    auto rgba = image->GetChannel<V4f>("rgba");

    for(int s = 0; s < image->sampleCount[y][x]; ++s)
    {
	float alpha = rgba->Get(x, y, s)[3];

	// Apply the alpha term to each sample underneath this one.
	for(float &sampleAlpha: result)
	    sampleAlpha *= 1-alpha;

	result.push_back(alpha);
    }
    return result;
}

shared_ptr<DeepImage> DeepImageUtil::CombineImages(vector<shared_ptr<DeepImage>> images)
{
    shared_ptr<DeepImage> result = make_shared<DeepImage>(images[0]->width, images[0]->height);
    DeepImageUtil::CopyLayerAttributes(images[0]->header, result->header);

    // Sum up the sampleCount for all images.
    Array2D<unsigned int> &totalSampleCount = result->sampleCount;
    for(int y = 0; y < result->height; y++)
    {
	for(int x = 0; x < result->width; x++)
	{
	    result->sampleCount[y][x] = 0;
	    for(auto image: images)
		result->sampleCount[y][x] += image->sampleCount[y][x];
	}
    }

    for(auto it: images[0]->channels)
    {
	// Create the combined channel for the new image.  CreateSameType will create a
	// channel of the same type as the existing channel.
	string channelName = it.first;
	shared_ptr<const DeepImageChannel> channel = it.second;
	shared_ptr<DeepImageChannel> newChannel(channel->CreateSameType(result->sampleCount));
	result->channels[channelName] = newChannel;

	// Copy samples from each input image.
	for(int y = 0; y < result->height; y++)
	{
	    for(int x = 0; x < result->width; x++)
	    {
		int nextSample = 0;
		for(auto image: images)
		{
		    shared_ptr<const DeepImageChannel> srcChannel = map_get(image->channels, channelName, nullptr);
		    if(srcChannel == nullptr)
			continue;

		    newChannel->CopySamples(srcChannel, x, y, nextSample);
		    nextSample += image->sampleCount[y][x];
		}
	    }
	}
    }

    return result;
}
