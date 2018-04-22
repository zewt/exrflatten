#include "EXROperation_CreateMask.h"
#include "DeepImageUtil.h"
#include "helpers.h"

#include <OpenEXR/ImfMatrixAttribute.h>

#include <algorithm>

using namespace Imf;
using namespace Imath;

// Get the layer we'll read if one isn't specified.
string CreateMask::GetSrcLayer() const
{
    if(!srcLayer.empty())
        return srcLayer;

    switch(mode)
    {
    case CreateMaskMode_FacingAngle: return "N";
    case CreateMaskMode_Depth: return "Z";
    case CreateMaskMode_Distance: return "P";
    }
    return "";
}

void CreateMask::AddLayers(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const
{
    string layer = GetSrcLayer();
    switch(mode)
    {
    case CreateMaskMode_FacingAngle:
    case CreateMaskMode_Distance:
    {
        image->AddChannelToFramebuffer<V3f>(layer, frameBuffer);
        break;
    }
    case CreateMaskMode_Depth:
        image->AddChannelToFramebuffer<float>(layer, frameBuffer);
        break;
    }

    image->AddChannel<float>(outputChannelName);
}

shared_ptr<TypedDeepImageChannel<float>> CreateMask::Create(shared_ptr<DeepImage> image) const
{
    shared_ptr<TypedDeepImageChannel<float>> mask;

    switch(mode)
    {
    case CreateMaskMode_FacingAngle: mask = CreateFacingAngle(image); break;
    case CreateMaskMode_Depth: mask = CreateDepth(image); break;
    case CreateMaskMode_Distance: mask = CreateDistance(image); break;
    }

    if(normalize)
    {
        float minMaskValue = 99999999.0f, maxMaskValue = -99999999.0f;
        for(int y = 0; y < image->height; y++)
        {
            for(int x = 0; x < image->width; x++)
            {
                for(int s = 0; s < image->NumSamples(x, y); ++s)
                {
                    float value = mask->Get(x,y,s);
                    minMaskValue = min(minMaskValue, value);
                    maxMaskValue = max(maxMaskValue, value);
                }
            }
        }

        if(minMaskValue != 99999999)
        {
            for(int y = 0; y < image->height; y++)
            {
                for(int x = 0; x < image->width; x++)
                {
                    for(int s = 0; s < image->NumSamples(x, y); ++s)
                        mask->Get(x,y,s) = scale(mask->Get(x,y,s), minMaskValue, maxMaskValue, 0.0f, 1.0f);
                }
            }
        }
    }

    // Clamp the mask, and optionally invert it.
    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            for(int s = 0; s < image->NumSamples(x, y); ++s)
            {
                float value = mask->Get(x,y,s);
                if(clamp)
                    value = ::clamp(value, 0.0f, 1.0f);
                if(invert)
                    value = 1.0f - value;

                mask->Get(x,y,s) = value;
            }
        }
    }

    return mask;
}

shared_ptr<TypedDeepImageChannel<float>> CreateMask::CreateFacingAngle(shared_ptr<DeepImage> image) const
{
    auto outputMask = image->AddChannel<float>(outputChannelName);
    auto src = image->GetChannel<V3f>(GetSrcLayer());
    
    auto *worldToCameraAttr = image->header.findTypedAttribute<M44fAttribute>("worldToCamera");
    if(worldToCameraAttr == nullptr)
        throw exception("Can't create mask because worldToCamera matrix attribute is missing");

    // Get the angle to compare the normal against.  This is usually away from the camera, but
    // can be changed to get a mask for a different angle.
    M44f worldToCamera = worldToCameraAttr->value();
    V3f towardsCamera = angle;
    if(towardsCamera == V3f(0,0,0))
        towardsCamera = V3f(0,0,-1);
    towardsCamera.normalize();

    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            for(int s = 0; s < image->NumSamples(x, y); ++s)
            {
                // Convert the normal to camera space.
                V3f worldSpaceNormal = src->Get(x,y,s);
                worldSpaceNormal.normalize();

                V3f cameraSpaceNormal;
                worldToCamera.multDirMatrix(worldSpaceNormal, cameraSpaceNormal);
                float angle = acos(cameraSpaceNormal.dot(towardsCamera)) * 180 / float(M_PI);

                outputMask->Get(x,y,s) = scale(angle, 0.0f, 90.0f, 0.0f, 1.0f);
            }
        }
    }
    return outputMask;
}

shared_ptr<TypedDeepImageChannel<float>> CreateMask::CreateDepth(shared_ptr<DeepImage> image) const
{
    auto outputMask = image->AddChannel<float>(outputChannelName);
    auto src = image->GetChannel<float>(GetSrcLayer());

    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            for(int s = 0; s < image->NumSamples(x, y); ++s)
            {
                float depth = src->Get(x,y,s);
                outputMask->Get(x,y,s) = scale(depth, minValue, maxValue, 0.0f, 1.0f);
            }
        }
    }
    return outputMask;
}

shared_ptr<TypedDeepImageChannel<float>> CreateMask::CreateDistance(shared_ptr<DeepImage> image) const
{
    auto outputMask = image->AddChannel<float>(outputChannelName);
    auto src = image->GetChannel<V3f>(GetSrcLayer());
    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            for(int s = 0; s < image->NumSamples(x, y); ++s)
            {
                V3f samplePos = src->Get(x,y,s);
                float distance = (samplePos - pos).length();
                outputMask->Get(x,y,s) = scale(distance, minValue, maxValue, 0.0f, 1.0f);
            }
        }
    }
    return outputMask;
}

EXROperation_CreateMask::EXROperation_CreateMask(const SharedConfig &sharedConfig, string opt, vector<pair<string,string>> arguments)
{
    if(opt == "facing")
        createMask.mode = CreateMask::CreateMaskMode_FacingAngle;
    else if(opt == "depth")
        createMask.mode = CreateMask::CreateMaskMode_Depth;
    else if(opt == "distance")
        createMask.mode = CreateMask::CreateMaskMode_Distance;
    else
        throw exception("Unknown --create-mask type");

    // The type of the mask is in opt, which lets us check that options aren't being
    // used that don't apply to this mask type, but this isn't currently done.
    for(auto it: arguments)
    {
        string arg = it.first;
        string value = it.second;

        auto getVectorArg = [&value] {
            vector<string> args;
            split(value, ",", args);
            if(args.size() < 3)
                return V3f(0,0,0);
            return V3f(
                float(atof(args[0].c_str())),
                float(atof(args[1].c_str())),
                float(atof(args[2].c_str())));
        };

        if(arg == "name")
            createMask.outputChannelName = value;
        else if(arg == "src")
            createMask.srcLayer = value;
        else if(arg == "min")
            createMask.minValue = float(atof(value.c_str()));
        else if(arg == "max")
            createMask.maxValue = float(atof(value.c_str()));
        else if(arg == "noclamp")
            createMask.clamp = false;
        else if(arg == "invert")
            createMask.invert = true;
        else if(arg == "normalize")
            createMask.normalize = true;
        else if(arg == "angle")
            createMask.angle = getVectorArg();
        else if(arg == "pos")
            createMask.pos = getVectorArg();
        else
            throw StringException("Unknown create-mask option: " + arg);
    }

    // Check that we received all of our required arguments.
    if(createMask.outputChannelName.empty())
        throw StringException("--create-mask: no --name was specified");
}

void EXROperation_CreateMask::Run(shared_ptr<EXROperationState> state) const
{
    createMask.Create(state->image);
}

void EXROperation_CreateMask::AddChannels(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const
{
    createMask.AddLayers(image, frameBuffer);
}
