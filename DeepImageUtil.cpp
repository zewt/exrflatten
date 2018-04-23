#include "DeepImageUtil.h"
#include "SimpleImage.h"
#include "helpers.h"

#include <algorithm>
#include <OpenEXR/ImathVec.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfMatrixAttribute.h>

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
    //
    // This is complicated by another silliness: "Y" can mean a Y coordinate or luminance.
    // We want to make sure both the orders "XYZ" and "YRyByA" are preserved, so make sure
    // XYZ is at the front of the list.
    static const vector<string> channelOrder = {
        "X", "Y", "Z",
        "R", "G", "B", /* "Y", */ "RY", "BY",
        "A", "AR", "AG", "AB",
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

shared_ptr<SimpleImage> DeepImageUtil::CollapseEXR(
        shared_ptr<const DeepImage> image,
        shared_ptr<const TypedDeepImageChannel<uint32_t>> id,
        shared_ptr<const TypedDeepImageChannel<V4f>> rgba,
        shared_ptr<const TypedDeepImageChannel<float>> mask,
        set<int> objectIds,
        CollapseMode mode)
{
    shared_ptr<SimpleImage> result = make_shared<SimpleImage>(image->width, image->height);

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

                // In CollapseMode_Normal, just ignore excluded samples entirely.
                if(mode == CollapseMode_Normal && !IncludeLayer)
                    continue;

                V4f color(1,1,1,1);
                if(rgba)
                    color = rgba->Get(x,y,s);

                float alpha = color.w;

                if(IncludeLayer && mask)
                {
                    // When we apply C1 + (C2*C1.w), apply the mask to the first C1
                    // term, but not to the final C1.w term.  If the mask is 0 and
                    // alpha is 1, that means the output color should become completely
                    // transparent, not that the sample has no effect.
                    color *= ::clamp(mask->Get(x, y, s), 0.0f, 1.0f);
                }

                if(IncludeLayer)
                {
                    out = color + out*(1-alpha);
                }
                else if(mode == CollapseMode_Visibility)
                {
                    // This sample is excluded.  In Visibility mode, still apply
                    // its alpha, so we make our samples less visible, and just
                    // don't add the color.
                    out = out*(1-alpha);
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
shared_ptr<DeepImage> DeepImageUtil::OrderSamplesByLayer(
    shared_ptr<const DeepImage> image,
    shared_ptr<const TypedDeepImageChannel<uint32_t>> id_,
    const map<int,int> &layerOrder,
    set<string> extraChannels)
{
    // Create a new, empty image with the same sample count.
    shared_ptr<DeepImage> newImage = make_shared<DeepImage>(image->width, image->height);
    for(int y = 0; y < image->height; y++)
        for(int x = 0; x < image->width; x++)
            newImage->sampleCount[y][x] = image->sampleCount[y][x];

    // Copy off the channels we're working with.
    shared_ptr<TypedDeepImageChannel<V4f>> rgba(image->GetChannel<V4f>("rgba")->Clone());
    newImage->AddChannel<V4f>("rgba", rgba);
    shared_ptr<TypedDeepImageChannel<uint32_t>> id(id_->Clone());
    newImage->AddChannel<uint32_t>("id", id);

    vector<shared_ptr<TypedDeepImageChannel<float>>> masks;
    for(string extraChannel: extraChannels)
    {
        shared_ptr<TypedDeepImageChannel<float>> mask(image->GetChannel<float>(extraChannel)->Clone());
        masks.push_back(mask);
        newImage->AddChannel<float>(extraChannel, mask);
    }

    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            for(int i = 0; i < image->NumSamples(x, y)-1; ++i)
            {
                for(int j = 0; j < image->NumSamples(x, y)-i-1; ++j)
                {
                    int s1 = j;
                    int s2 = j+1;

                    int objectId1 = id->Get(x, y, s1);
                    int objectId2 = id->Get(x, y, s2);
                    int layerOrder1 = layerOrder.at(objectId1);
                    int layerOrder2 = layerOrder.at(objectId2);
                    if(layerOrder1 <= layerOrder2)
                        continue;

                    SwapSamples(image,
                        rgba,
                        id,
                        x, y,
                        s1, s2,
                        masks);
                }
            }
        }
    }

    return newImage;
}

// Swap two samples in an image, without changing the result of compositing
// them in sample (not depth) order.
//
// The basic premise is that we have two premultiplied layers:
//
//    R G B   A
// A: 1 1 0   1.0
// B: 0 0 0.3 0.25
//
// When this is composited, we get
// 
//    0.75 0.75 0.3 1.0
// 
// Sample A is further away from the camera (sample B covers sample A).  Normally, you'd
// comp A in with its 1.0 alpha, then comp B on top of it with its .25 alpha.  However,
// we want to comp B first.  To do this, we notice that since B should be covering A by 25%
// A needs to have an alpha of .75:
// 
// B: 0    0    0.3  0.25
// A: 0.75 0.75 0    0.75
// 
// Now we need to adjust layer B so the result is the same as before, by multiplying by 1/.25:
// 
// B: 0    0    1.2  1.0
// A: 0.75 0.75 0    0.75
//
// Compositing this gives the same result as the original.
//
// This is more complicated when more than two samples are involved and I've only
// solved this for the two sample case.  To generalize it to sorting whole layers,
// we bubble sort the samples, which allows us to sort them in any order while only
// swapping adjacent entries in any one step.
void DeepImageUtil::SwapSamples(
    shared_ptr<const DeepImage> image,
    shared_ptr<TypedDeepImageChannel<V4f>> rgba,
    shared_ptr<TypedDeepImageChannel<uint32_t>> id,
    int x, int y,
    int s1, int s2,
    vector<shared_ptr<TypedDeepImageChannel<float>>> masks)
{
    swap(id->Get(x, y, s1), id->Get(x, y, s2));

    // If we have any mask, swap them too.
    for(auto mask: masks)
        swap(mask->Get(x, y, s1), mask->Get(x, y, s2));

    const V4f origColor1 = rgba->Get(x, y, s1);
    const V4f origColor2 = rgba->Get(x, y, s2);

    // If this sample is part of this layer, composite it in.  If it's in a later
    // layer, it still causes this color to become less visible, so apply alpha,
    // but don't add color.
    // [1] This sample is in an earlier layer (comped before this one).
    // This is color that should have been comped after us.
    V4f newColor1 = origColor1 * (1-origColor2.w);

    // The amount s2 covers s1.  If this is .75, s2 covers s1 by 75%,
    // so make s2 4x more visible when we put it underneath s1.
    float coveringAlpha = origColor1.w * (1-origColor2.w);

    V4f newColor2 = origColor2;
    if(1-coveringAlpha > 0.00001f)
        newColor2 /= 1-coveringAlpha;

    // Store the swapped colors.
    rgba->Get(x, y, s1) = newColor2;
    rgba->Get(x, y, s2) = newColor1;
}

void DeepImageUtil::ExtractMask(
    bool alphaMask,
    bool compositeAlpha,
    shared_ptr<const TypedDeepImageChannel<float>> mask,
    shared_ptr<const DeepImageChannelProxy> A,
    shared_ptr<const TypedDeepImageChannel<uint32_t>> id,
    int objectId,
    shared_ptr<SimpleImage> layer)
{
    for(int y = 0; y < A->height; y++)
    {
        for(int x = 0; x < A->width; x++)
        {
            float resultValue = 0;
            if(compositeAlpha)
            {
                // If compositeAlpha is true, blend the mask like a color value, giving us a
                // composited mask value and its transparency: (mask, alpha).
                V2f result(0,0);
                for(int s = 0; s < A->sampleCount[y][x]; ++s)
                {
                    if(id->Get(x, y, s) != objectId)
                        continue;

                    float maskValue = ::clamp(mask->Get(x, y, s), 0.0f, 1.0f);
                    float alpha = A->Get(x,y,s);
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
                for(int s = A->sampleCount[y][x]-1; s >= 0; --s)
                {
                    if(id->Get(x, y, s) != objectId)
                        continue;

                    float alpha = A->Get(x,y,s);
                    if(alpha < 0.00001f)
                        continue;

                    resultValue = ::clamp(mask->Get(x, y, s), 0.0f, 1.0f);
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

    auto A = image->GetAlphaChannel();

    for(int s = 0; s < image->sampleCount[y][x]; ++s)
    {
        float alpha = A->Get(x, y, s);

        // Apply the alpha term to each sample underneath this one.
        for(float &sampleAlpha: result)
            sampleAlpha *= 1-alpha;

        result.push_back(1.0f);
    }
    return result;
}

void DeepImageUtil::GetSampleVisibilities(shared_ptr<const DeepImage> image, Array2D<vector<float>> &SampleVisibilities)
{
    SampleVisibilities.resizeErase(image->height, image->width);
    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
            SampleVisibilities[y][x] = DeepImageUtil::GetSampleVisibility(image, x, y);
    }
}

namespace {
    void SumSampleCounts(Array2D<unsigned int> &totalSampleCount, const vector<shared_ptr<DeepImage>> &images)
    {
        for(int y = 0; y < totalSampleCount.height(); y++)
        {
            for(int x = 0; x < totalSampleCount.width(); x++)
            {
                totalSampleCount[y][x] = 0;
                for(auto image: images)
                    totalSampleCount[y][x] += image->sampleCount[y][x];
            }
        }
    }
}

shared_ptr<DeepImage> DeepImageUtil::CombineImages(vector<shared_ptr<DeepImage>> images)
{
    shared_ptr<DeepImage> result = make_shared<DeepImage>(images[0]->width, images[0]->height);
    DeepImageUtil::CopyLayerAttributes(images[0]->header, result->header);

    // Sum up the sampleCount for all images.
    SumSampleCounts(result->sampleCount, images);

    for(auto it: images[0]->channels)
    {
        // Create the combined channel for the new image.  CreateSameType will create a
        // channel of the same type as the existing channel.
        string channelName = it.first;
        shared_ptr<const DeepImageChannel> channel = it.second;
        shared_ptr<DeepImageChannel> newChannel(channel->CreateSameType(result->sampleCount));
        result->channels[channelName] = newChannel;

        Array2D<unsigned int> sampleCountSoFar;
        sampleCountSoFar.resizeErase(result->height, result->width);
        memset(&sampleCountSoFar[0][0], 0, sizeof(sampleCountSoFar[0][0]) * result->height * result->width);

#if 1
        // Copy samples from each input image.  This is optimized by getting a raw pointer to
        // the samples and copying data directly.
        for(auto image: images)
        {
            shared_ptr<const DeepImageChannel> srcChannel = map_get(image->channels, channelName, nullptr);

            const char * const*srcData = srcChannel->GetSamplesBlind();
                  char **dstData = newChannel->GetSamplesBlind();
            int BytesPerSample = newChannel->GetBytesPerSample();
            for(int y = 0; y < result->height; y++)
            {
                const unsigned int *SampleCounts = image->sampleCount[y];
                for(int x = 0; x < result->width; x++)
                {
                    const char *pSrc = srcData[x + y*result->width];
                    char *pDst = dstData[x + y*result->width];
                    pDst += sampleCountSoFar[y][x] * BytesPerSample;
                    memcpy(pDst, pSrc, BytesPerSample*SampleCounts[x]);

                    sampleCountSoFar[y][x] += SampleCounts[x];
                }
            }
        }
#else
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
#endif
    }

    return result;
}

void DeepImageUtil::TransformNormalMap(shared_ptr<const DeepImage> image,
    shared_ptr<const TypedDeepImageChannel<V3f>> inputChannel,
    shared_ptr<TypedDeepImageChannel<V3f>> outputChannel,
    M44f matrix)
{
    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            for(int s = 0; s < image->NumSamples(x, y); ++s)
            {
                V3f vec = inputChannel->Get(x,y,s);

                // We're working with normal maps, and Arnold doesn't always output normalized
                // normals due to a bug, so normalize now.
                vec.normalize();

                V3f result;
                matrix.multDirMatrix(vec, result);
                outputChannel->Get(x,y,s) = result;
            }
        }
    }
}
