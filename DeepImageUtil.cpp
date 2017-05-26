#include "DeepImageUtil.h"
#include "SimpleImage.h"
#include "helpers.h"

#include <algorithm>
#include <OpenEXR/ImathVec.h>

using namespace Imf;
using namespace Imath;


// Flatten the color channels of a deep EXR to a simple flat layer.
shared_ptr<SimpleImage> DeepImageUtil::CollapseEXR(shared_ptr<const DeepImage> image, set<int> objectIds)
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
void DeepImageUtil::CombineObjectId(shared_ptr<DeepImage> image, int fromObjectId, int intoObjectId)
{
    auto id = image->GetChannel<uint32_t>("id");
    for(int y = 0; y < image->height; y++)
    {
	for(int x = 0; x < image->width; x++)
	{
	    for(int s = 0; s < image->NumSamples(x, y); ++s)
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
