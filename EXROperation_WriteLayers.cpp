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
	image->AddChannelToFramebuffer<float>(maskDesc.maskChannel, frameBuffer, true);
}

void EXROperation_WriteLayers::Run(shared_ptr<EXROperationState> state) const
{
    shared_ptr<DeepImage> image = state->image;
    vector<Layer> layers;

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
	    // XXX: do locally, make this const
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
    shared_ptr<TypedDeepImageChannel<uint32_t>> collapsedId(image->GetChannel<uint32_t>("id")->Clone());
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

    // Separate the image into layers.
    int nextOrder = 1;
    auto getLayer = [&](string layerName, string layerType, int width, int height, bool ordered)
    {
	layers.push_back(Layer(width, height));
	Layer &layer = layers.back();
	layer.layerName = layerName;
	layer.layerType = layerType;
	if(ordered)
	    layer.order = nextOrder++;

	// Copy all image attributes, except for built-in EXR headers that we shouldn't set.
	DeepImageUtil::CopyLayerAttributes(image->header, layer.image->header);

	layer.filename = MakeOutputFilename(layer);

	return layer.image;
    };

    for(auto layerDesc: layerDescsCopy)
    {
	// Skip this layer if we've removed it from layerOrder.
	if(layerOrder.find(layerDesc.objectId) == layerOrder.end())
	    continue;

	string layerName = layerDesc.layerName;

	auto colorOut = getLayer(layerName, "color", image->width, image->height, true);
	DeepImageUtil::SeparateLayer(image, collapsedId, layerDesc.objectId, colorOut, layerOrder, nullptr);

	for(auto maskDesc: masks)
	{
	    auto mask = image->GetChannel<float>(maskDesc.maskChannel);
	    if(mask == nullptr)
		continue;

	    auto maskOut = getLayer(layerName, maskDesc.maskName, image->width, image->height, false);
	    if(maskDesc.maskType == MaskDesc::MaskType_Composited)
    		DeepImageUtil::SeparateLayer(image, collapsedId, layerDesc.objectId, maskOut, layerOrder, mask);
	    else
	    {
		bool useAlpha = maskDesc.maskType == MaskDesc::MaskType_Alpha;
		auto A = image->GetAlphaChannel();
		DeepImageUtil::ExtractMask(useAlpha, false, mask, A, collapsedId, layerDesc.objectId, maskOut);
	    }
	}
    }

    // Write the layers.
    for(const auto &layer: layers)
    {
	// Don't write this layer if it's completely empty.
	if(layer.image->IsEmpty())
	    continue;

	printf("Writing %s\n", layer.filename.c_str());
        SimpleImage::WriteEXR(layer.filename, layer.image);
    }
}

// Do simple substitutions on the output filename.
string EXROperation_WriteLayers::MakeOutputFilename(const Layer &layer) const
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
	    maskChannel = args[1].c_str();
	else if(args[0] == "name" && args.size() > 1)
	    maskName = args[1].c_str();
	else if(args[0] == "grey")
	    maskType = MaskType_Greyscale;
	else if(args[0] == "alpha")
	    maskType = MaskType_Alpha;
	else if(args[0] == "rgb")
	    maskType = MaskType_Composited;
    }
}

