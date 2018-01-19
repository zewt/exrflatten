#include "EXROperation_WriteLayers.h"
#include "helpers.h"
#include "DeepImageUtil.h"

using namespace Imf;
using namespace Imath;

EXROperation_WriteLayers::EXROperation_WriteLayers(const SharedConfig &sharedConfig_, string opt, vector<pair<string,string>> arguments):
    sharedConfig(sharedConfig_)
{
    for(auto it: arguments)
    {
        string arg = it.first;
        string value = it.second;

        if(arg == "filename-pattern")
        {
            outputPattern = value;
        }
        else if(arg == "layer")
        {
            // id=name
            vector<string> descParts;
            split(value, "=", descParts);
            if(descParts.size() != 2)
            {
                printf("Warning: ignored part of layer desc \"%s\"\n", value.c_str());
                continue;
            }

            LayerDesc layer;
            layer.objectId = atoi(descParts[0].c_str());
            layer.layerName = descParts[1];
            layerDescs.push_back(layer);
        }
        else if(arg == "layer-mask")
        {
            MaskDesc mask;
            mask.ParseOptionsString(value);
            masks.push_back(mask);
        }
        else if(arg == "combine")
        {
            const char *split = strchr(value.c_str(), ',');
            if(split == NULL)
            {
                printf("Invalid --combine (ignored)\n");
                continue;
            }

            int dst = atoi(value.c_str());
            int src = atoi(split+1);
            combines.push_back(make_pair(dst, src));
        }
        else
            throw StringException("Unknown save-layers option: " + arg);
    }
}

void EXROperation_WriteLayers::AddChannels(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const
{
    // Add channels used by masks.
    for(auto maskDesc: masks)
    {
        auto channel = image->AddChannelToFramebuffer<float>(maskDesc.maskChannel, frameBuffer);
        channel->needsUnpremultiply = true;
    }

    image->AddChannelToFramebuffer<uint32_t>(sharedConfig.idChannel, frameBuffer);
}

void EXROperation_WriteLayers::Run(shared_ptr<EXROperationState> state) const
{
    shared_ptr<DeepImage> image = state->image;

    vector<LayerDesc> layerDescsCopy = layerDescs;

    // If no layer was specified for the default object ID, add one at the beginning.
    {
        bool hasDefaultObjectId = false;
        for(auto layer: layerDescsCopy)
            if(layer.objectId == DeepImageUtil::NO_OBJECT_ID)
                hasDefaultObjectId = true;

        if(!hasDefaultObjectId)
        {
            LayerDesc layerDesc;
            layerDesc.objectId = 0;
            layerDesc.layerName = "default";
            layerDescsCopy.insert(layerDescsCopy.begin(), layerDesc);
        }
    }

    // Create the layer ordering.  This just maps each layer's object ID to its position in
    // the layer list.
    map<int,int> layerOrder;
    {
        int next = 0;
        for(auto layerDesc: layerDescsCopy)
            layerOrder[layerDesc.objectId] = next++;
    }

    // Combine layers.  This just changes the object IDs of samples, so we don't need to re-sort.
    shared_ptr<TypedDeepImageChannel<uint32_t>> collapsedId(image->GetChannel<uint32_t>(sharedConfig.idChannel)->Clone());
    for(auto combine: combines)
        DeepImageUtil::CombineObjectId(collapsedId, combine.second, combine.first);

    // Collapse any object IDs that aren't associated with layers into the default layer
    // to use with layer separation.  Do this after combines, so if we collapsed an object
    // ID into one that isn't being output, we also collapse those into NO_OBJECT_ID.
    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            for(int s = 0; s < image->NumSamples(x, y); ++s)
            {
                uint32_t value = collapsedId->Get(x,y,s);
                if(layerOrder.find(value) == layerOrder.end())
                    collapsedId->Get(x,y,s) = DeepImageUtil::NO_OBJECT_ID;
            }
        }
    }

    int nextOrder = 1;
    vector<shared_ptr<OutputImage>> outputImages;
    auto createOutputImage = [&](string layerName, string layerType, bool ordered)
    {
        outputImages.push_back(make_shared<OutputImage>());
        shared_ptr<OutputImage> newImage = outputImages.back();
        newImage->layerName = layerName;
        newImage->layerType = layerType;
        if(ordered)
            newImage->order = nextOrder++;

        newImage->filename = MakeOutputFilename(*newImage.get());

        return newImage;
    };

    auto addLayer = [&](shared_ptr<OutputImage> outputImage, shared_ptr<SimpleImage> imageToAdd)
    {
        // Copy all image attributes, except for built-in EXR headers that we shouldn't set.
        DeepImageUtil::CopyLayerAttributes(image->header, imageToAdd->header);

        // Add it to the layer list to be written.
        outputImage->layers.push_back(SimpleImage::EXRLayersToWrite(imageToAdd));

        return &outputImage->layers.back();
    };

    // Reorder the samples so we can separate it into layers.
    vector<string> maskNames;
    for(auto maskDesc: masks)
        maskNames.push_back(maskDesc.maskName);
    shared_ptr<DeepImage> newImage = DeepImageUtil::OrderSamplesByLayer(image, collapsedId, layerOrder, maskNames);

    // Separate the image into its layers.
    map<int,shared_ptr<SimpleImage>> separatedLayers;
    shared_ptr<const TypedDeepImageChannel<V4f>> rgba = newImage->GetChannel<V4f>("rgba");
    shared_ptr<const TypedDeepImageChannel<uint32_t>> id = newImage->GetChannel<uint32_t>("id");
    for(auto it: layerOrder)
    {
        // All we need to do now is blend samples with each object ID, ignoring
        // the others.
        int objectId = it.first;
        shared_ptr<SimpleImage> layerImage = make_shared<SimpleImage>(image->width, image->height);
        separatedLayers[objectId] = DeepImageUtil::CollapseEXR(newImage, id, rgba, nullptr, { objectId });
    }

    for(auto layerDesc: layerDescsCopy)
    {
        // Skip this layer if we've removed it from layerOrder.
        if(layerOrder.find(layerDesc.objectId) == layerOrder.end())
        {
            // Skip this order number, so filenames stay consistent.
            nextOrder++;
            continue;
        }

        string layerName = layerDesc.layerName;

        // Create an output image named "color", and extract the layer into it.
        auto colorImageOutput = separatedLayers.at(layerDesc.objectId);

        // If the color layer is completely empty, don't create it.
        if(colorImageOutput->IsEmpty())
        {
            // Skip this order number, so filenames stay consistent.
            nextOrder++;
            continue;
        }

        auto &colorImageOut = createOutputImage(layerName, "color", true);
        addLayer(colorImageOut, colorImageOutput);

        // Create output layers for each of this color layer's masks.
        for(auto maskDesc: masks)
        {
            auto mask = newImage->GetChannel<float>(maskDesc.maskChannel);
            if(mask == nullptr)
                continue;

            // Extract the mask.
            shared_ptr<SimpleImage> maskOut;
            if(maskDesc.maskType == MaskDesc::MaskType_CompositedRGB)
            {
                // Apply the mask to the image into maskOut.  Use CollapseMode_Visibility
                // when creating masks.
                maskOut = DeepImageUtil::CollapseEXR(newImage, id, rgba, mask,
                    { layerDesc.objectId },
                    DeepImageUtil::CollapseMode_Visibility);
            }
            else
            {
                // Output an alpha mask for MaskType_Alpha and MaskType_EXRLayer.
                maskOut = make_shared<SimpleImage>(newImage->width, newImage->height);
                bool useAlpha = maskDesc.maskType != MaskDesc::MaskType_Greyscale;
                auto A = newImage->GetAlphaChannel();
                DeepImageUtil::ExtractMask(useAlpha, true, mask, A, collapsedId, layerDesc.objectId, maskOut);
            }

            // If the baked image is completely empty, don't create it.  As an exception,
            // we do output empty masks in MaskType_EXRLayer.
            if(maskDesc.maskType != MaskDesc::MaskType_EXRLayer && maskOut->IsEmpty())
                continue;

            if(maskDesc.maskType == MaskDesc::MaskType_EXRLayer)
            {
                // Instead of creating a layer that will be output to its own EXR
                // file, put the mask in an EXR layer in the color layer file.
                SimpleImage::EXRLayersToWrite *maskLayer = addLayer(colorImageOut, maskOut);
                maskLayer->layerName = maskDesc.maskName;
                maskLayer->alphaOnly = true;
            }
            else
            {
                // Output this mask to a separate file.
                auto maskOutImage = createOutputImage(layerName, maskDesc.maskName, false);
                addLayer(maskOutImage, maskOut);
            }
        }
    }

    // Write the layers.
    for(const auto &outputImage: outputImages)
    {
        printf("Writing %s\n", outputImage->filename.c_str());
        SimpleImage::WriteEXR(outputImage->filename, outputImage->layers);
    }
}

// Do simple substitutions on the output filename.
string EXROperation_WriteLayers::MakeOutputFilename(const OutputImage &layer) const
{
    string outputName = outputPattern;

    const string originalOutputName = outputName;

    // <name>: the name of the object ID that we got from the EXR file, or "#100" if we
    // only have a number.
    outputName = subst(outputName, "<name>", layer.layerName);

    string orderName = "";
    if(layer.order > 0)
        orderName += ssprintf("#%i ", layer.order);
    orderName += layer.layerName;
    outputName = subst(outputName, "<ordername>", orderName);

    // <layer>: the output layer that we generated.  This is currently always "color".
    outputName = subst(outputName, "<layer>", layer.layerType);

    // <order>: the order this layer should be composited.  Putting this early in the
    // filename makes filenames sort in comp order, which can be convenient.
    outputName = subst(outputName, "<order>", ssprintf("%i", layer.order));

    // <inputname>: the input filename, with the directory and ".exr" removed.
    string inputName = sharedConfig.inputFilenames[0];
    inputName = basename(inputName);
    inputName = setExtension(inputName, "");
    outputName = subst(outputName, "<inputname>", inputName);

    // <frame>: the input filename's frame number, given a "abcdef.1234.exr" filename.
    // It would be nice if there was an EXR attribute contained the frame number.
    outputName = subst(outputName, "<frame>", GetFrameNumberFromFilename(sharedConfig.inputFilenames[0]));

    static bool warned = false;
    if(!warned && outputName == originalOutputName)
    {
        // If the output filename hasn't changed, there are no substitutions in it, which
        // means we'll write a single file over and over.  That's probably not what was
        // wanted.
        fprintf(stderr, "Warning: output path \"%s\" doesn't contain any substitutions, so only one file will be written.\n", outputName.c_str());
        fprintf(stderr, "Try \"%s\" instead.\n", (outputName + "_<name>.exr").c_str());
        warned = true;
    }

    outputName = sharedConfig.GetFilename(outputName);

    return outputName;
}

// Given a filename like "abcdef.1234.exr", return "1234".
string EXROperation_WriteLayers::GetFrameNumberFromFilename(string s) const
{
    // abcdef.1234.exr -> abcdef.1234
    s = setExtension(s, "");

    auto pos = s.rfind(".");
    if(pos == string::npos)
        return "";

    string frameString = s.substr(pos+1);
    return frameString;
}

void EXROperation_WriteLayers::MaskDesc::ParseOptionsString(string optionsString)
{
    vector<string> options;
    split(optionsString, ";", options);
    for(string option: options)
    {
        vector<string> args;
        split(option, "=", args);
        if(args.size() < 1)
            continue;

        if(args[0] == "channel" && args.size() > 1)
        {
            maskChannel = args[1].c_str();

            // If no mask name is specified, use the input channel by default.
            if(maskName.empty())
                maskName = maskChannel;
        }
        else if(args[0] == "name" && args.size() > 1)
            maskName = args[1].c_str();
        else if(args[0] == "grey")
            maskType = MaskType_Greyscale;
        else if(args[0] == "alpha")
            maskType = MaskType_Alpha;
        else if(args[0] == "rgb")
            maskType = MaskType_CompositedRGB;
        else if(args[0] == "exrlayer")
            maskType = MaskType_EXRLayer;
    }
}

