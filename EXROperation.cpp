#include "EXROperation.h"
#include "DeepImage.h"
#include "DeepImageUtil.h"

#include <OpenEXR/ImfChannelList.h>

bool SharedConfig::ParseOption(string opt, string value)
{
    if(opt == "input")
    {
        inputFilenames.push_back(value);
        return true;
    }
    else if(opt == "output")
    {
        outputPath = value;
        return true;
    }
    else if(opt == "units")
    {
        if(value == "cm")
            worldSpaceScale = 100;
        else if(value == "meters")
            worldSpaceScale = 100; // cm per meter
        else if(value == "feet")
            worldSpaceScale = 30.48f; // cm per foot
        else
            worldSpaceScale = (float) atof(value.c_str());

        if(worldSpaceScale < 0.0001f)
            throw StringException("Invalid world space scale: " + value);
    }
    else if(opt == "id")
    {
        // Change the name of the layer used for IDs.
        explicitIdChannel = value;
        return true;
    }

    return false;
}

string SharedConfig::GetIdChannel(const Imf::Header &header) const
{
    if(!explicitIdChannel.empty())
        return explicitIdChannel;

    // If no ID channel was specified explicitly with --id, search for both "ID" and "id".
    if(header.channels().findChannel("id") != NULL)
        return "id";
    else
        return "ID";
}

shared_ptr<DeepImage> EXROperationState::GetOutputImage()
{
    if(newImage)
        return newImage;

    newImage = make_shared<DeepImage>(image->width, image->height);
    newImage->header = image->header;
    for(auto it: image->channels)
    {
        string name = it.first;
        shared_ptr<const DeepImageChannel> channel = it.second;
        shared_ptr<DeepImageChannel> newChannel(channel->CreateSameType(newImage->sampleCount));
        newImage->channels[name] = newChannel;
    }

    waitingImages.push_back(newImage);

    return newImage;
}

void EXROperationState::CombineWaitingImages()
{
    if(waitingImages.empty())
        return;

    // Add the old image first, so we copy its attributes.
    waitingImages.insert(waitingImages.begin(), image);
    image = DeepImageUtil::CombineImages(waitingImages);
    waitingImages.clear();

    // Sort samples in the combined image.
    DeepImageUtil::SortSamplesByDepth(image);
}
