#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>
#include <limits.h>
#include "getopt.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <vector>
#include <unordered_map>

// Too fine-grained:
#include <OpenEXR/ImfCRgbaFile.h>
#include <OpenEXR/ImathBox.h>
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfPreviewImage.h>
#include <OpenEXR/ImfAttribute.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfMatrixAttribute.h>
#include <OpenEXR/Iex.h>

#include "DeepImage.h"
#include "DeepImageMasks.h"
#include "DeepImageStroke.h"
#include "DeepImageUtil.h"
#include "helpers.h"
#include "SimpleImage.h"

using namespace std;
using namespace Imf;
using namespace Imath;
using namespace Iex;

// This currently processes all object IDs at once, which means we need enough memory to hold all
// output buffers at once.  We could make a separate pass for each object ID to reduce memory usage,
// so we only need to hold one at a time.
//
// Not currently supported/tested:
// - data window is untested
// - tiled images
// - volumes (samples with non-zero depth)
// - arbitrary channel mappings, including layers (we assume "R", "G", "B", "A", "Z", "ZBack", "id")
// - separate per-color alpha (RA, GA, BA)
// - (and lots of other stuff, EXR is "too general")


#include "EXROperation.h"

// Configuration settings shared by multiple EXROperations.  Note that this is
// stored by value, so operations have a snapshot of the configuration at the time
// they were created.
struct SharedConfig
{
    string outputPath;
    vector<string> inputFilenames;
};

class Operation_SaveFlattenedImage: public EXROperation
{
public:
    Operation_SaveFlattenedImage(SharedConfig sharedConfig, string args)
    {
	filename = sharedConfig.outputPath;
	if(!filename.empty())
	    filename += "/";
	filename += args;
    }

    void Run(shared_ptr<DeepImage> image)
    {
	auto flat = DeepImageUtil::CollapseEXR(image);

	printf("Writing %s\n", filename.c_str());
	flat->WriteEXR(filename);
    }

private:
    string filename;
};

class Operation_Stroke: public EXROperation
{
public:
    Operation_Stroke(string args)
    {
	strokeDesc.ParseOptionsString(args);
    }

    void Run(shared_ptr<DeepImage> image)
    {
	DeepImageStroke::AddStroke(strokeDesc, image);

	// Re-sort samples, since new samples may have been added.
	DeepImageUtil::SortSamplesByDepth(image);
    }

    void AddChannels(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const
    {
	if(!strokeDesc.strokeMaskChannel.empty())
	    image->AddChannelToFramebuffer<float>(strokeDesc.strokeMaskChannel, { strokeDesc.strokeMaskChannel }, frameBuffer, true);
	if(!strokeDesc.intersectionMaskChannel.empty())
	    image->AddChannelToFramebuffer<float>(strokeDesc.intersectionMaskChannel, { strokeDesc.intersectionMaskChannel }, frameBuffer, true);
    }

private:
    DeepImageStroke::Config strokeDesc;
};

class Operation_WriteLayers: public EXROperation
{
public:
    SharedConfig sharedConfig;
    string outputPattern = "<inputname> <ordername> <layer>.exr";

    Operation_WriteLayers(SharedConfig sharedConfig_)
    {
	sharedConfig = sharedConfig_;
    }

    struct Layer
    {
	string filename;
	string layerName;
	string layerType;
	int order = 0;
	shared_ptr<SimpleImage> image;

	Layer(int width, int height)
	{
	    image = make_shared<SimpleImage>(width, height);
	}
    };

    struct LayerDesc
    {
	string layerName;
	int objectId;
    };
    vector<LayerDesc> layerDescs;

    struct MaskDesc
    {
	void ParseOptionsString(string optionsString);

	enum MaskType
	{
	    // The mask value will be output on the RGB channels.
	    MaskType_Greyscale,

	    // The mask value will be output on the alpha channel.
	    MaskType_Alpha,

	    // The mask will be composited with the color channel and output as a pre-masked
	    // RGBA image.
	    MaskType_Composited,
	};
	MaskType maskType = MaskType_Greyscale;
	string maskChannel;
	string maskName;
    };
    vector<MaskDesc> masks;

    // A list of (dst, src) pairs to combine layers before writing them.
    vector<pair<int,int>> combines;

    void Run(shared_ptr<DeepImage> image);

    string MakeOutputFilename(const Layer &layer);
    string GetFrameNumberFromFilename(string s);

    void AddChannels(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const
    {
	// Add channels used by masks.
	for(auto maskDesc: masks)
	    image->AddChannelToFramebuffer<float>(maskDesc.maskChannel, { maskDesc.maskChannel }, frameBuffer, true);
    }

    bool AddArgument(string opt, string value)
    {
	if(opt == "filename")
	{
	    outputPattern = value;
	    return true;
	}
	else if(opt == "layer")
	{
	    // id=name
	    vector<string> descParts;
	    split(value, "=", descParts);
	    if(descParts.size() != 2)
	    {
		printf("Warning: ignored part of layer desc \"%s\"\n", value.c_str());
		return true;
	    }

	    LayerDesc layer;
	    layer.objectId = atoi(descParts[0].c_str());
	    layer.layerName = descParts[1];
	    layerDescs.push_back(layer);
	    return true;
	}
	else if(opt == "save-mask")
	{
	    MaskDesc mask;
	    mask.ParseOptionsString(value);
	    masks.push_back(mask);
	    return true;
	}
	else if(opt == "combine")
	{
	    const char *split = strchr(optarg, ',');
	    if(split == NULL)
	    {
		printf("Invalid --combine (ignored)\n");
		return true;
	    }

	    int dst = atoi(optarg);
	    int src = atoi(split+1);
	    combines.push_back(make_pair(dst, src));
	    return true;
	}

	return false;
    }

};


void Operation_WriteLayers::Run(shared_ptr<DeepImage> image)
{
    vector<Layer> layers;

    // If no layer was specified for the default object ID, add one at the beginning.
    {
	bool hasDefaultObjectId = false;
	for(auto layer: layerDescs)
	    if(layer.objectId == DeepImageUtil::NO_OBJECT_ID)
		hasDefaultObjectId = true;

	if(!hasDefaultObjectId)
	{
	    LayerDesc layerDesc;
	    layerDesc.objectId = 0;
	    layerDesc.layerName = "default";
	    // XXX: do locally, make this const
	    layerDescs.insert(layerDescs.begin(), layerDesc);
	}
    }

    // Create the layer ordering.  This just maps each layer's object ID to its position in
    // the layer list.
    map<int,int> layerOrder;
    {
	int next = 0;
	for(auto layerDesc: layerDescs)
	    layerOrder[layerDesc.objectId] = next++;
    }

    // Collapse any object IDs that aren't associated with layers into the default layer
    // to use with layer separation.
    shared_ptr<TypedDeepImageChannel<uint32_t>> collapsedId(image->GetChannel<uint32_t>("id")->Clone());
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

    // Combine layers.  This just changes the object IDs of samples, so we don't need to re-sort.
    for(auto combine: combines)
	DeepImageUtil::CombineObjectId(collapsedId, combine.second, combine.first);

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

    for(auto layerDesc: layerDescs)
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
		auto rgba = image->GetChannel<V4f>("rgba");
		DeepImageUtil::ExtractMask(useAlpha, false, mask, rgba, collapsedId, layerDesc.objectId, maskOut);
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
	layer.image->WriteEXR(layer.filename);
    }
}

// Do simple substitutions on the output filename.
string Operation_WriteLayers::MakeOutputFilename(const Layer &layer)
{
    string outputName = outputPattern;
    if(!sharedConfig.outputPath.empty())
	outputName = sharedConfig.outputPath + "/" + outputName;

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

    return outputName;
}

// Given a filename like "abcdef.1234.exr", return "1234".
string Operation_WriteLayers::GetFrameNumberFromFilename(string s)
{
    // abcdef.1234.exr -> abcdef.1234
    s = setExtension(s, "");

    auto pos = s.rfind(".");
    if(pos == string::npos)
	return "";

    string frameString = s.substr(pos+1);
    return frameString;
}

void Operation_WriteLayers::MaskDesc::ParseOptionsString(string optionsString)
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

class Operation_CreateMask: public EXROperation
{
public:
    Operation_CreateMask(string args)
    {
	createMask.ParseOptionsString(args);
    }

    void Run(shared_ptr<DeepImage> image)
    {
	createMask.Create(image);
    }

    void AddChannels(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const
    {
	createMask.AddLayers(image, frameBuffer);
    }

private:
    CreateMask createMask;
};

struct Config
{
    bool ParseOption(string opt, string value, vector<pair<string,string>> &accumulatedOptions);

    SharedConfig sharedConfig;

    vector<shared_ptr<EXROperation>> operations;
};

bool Config::ParseOption(string opt, string value, vector<pair<string,string>> &accumulatedOptions)
{
    shared_ptr<EXROperation> newOperation;
    if(opt == "save-layers")
    {
	newOperation = make_shared<Operation_WriteLayers>(sharedConfig);
    }
    else if(opt == "create-mask")
    {
	newOperation = make_shared<Operation_CreateMask>(value);
    }
    else if(opt == "stroke")
    {
	newOperation = make_shared<Operation_Stroke>(value);
    }
    else if(opt == "save-flattened")
    {
	newOperation = make_shared<Operation_SaveFlattenedImage>(sharedConfig, value);
    }
    else
    {
	// We don't know what this option is, so it's probably an option for an upcoming
	// operation.  Save it, and we'll hand it to the op once it's created.
	accumulatedOptions.emplace_back(opt, value);
    }

    if(newOperation == nullptr)
	return false;

    // Store the new operation.
    operations.push_back(newOperation);

    // Send any arguments that we didn't recognize to the new operation.
    for(auto it: accumulatedOptions)
    {
	if(!newOperation->AddArgument(it.first, it.second))
	    printf("Unrecognized argument: %s\n", it.first.c_str());
    }
    accumulatedOptions.clear();

    return true;
}

namespace FlattenFiles
{
    bool flatten(Config config);
};

bool FlattenFiles::flatten(Config config)
{
    vector<shared_ptr<DeepImage>> images;
    for(string inputFilename: config.sharedConfig.inputFilenames)
    {
	DeepImageReader reader;
	shared_ptr<DeepImage> image = reader.Open(inputFilename);

	// Set up the channels we're interested in.
	DeepFrameBuffer frameBuffer;
	image->AddSampleCountSliceToFramebuffer(frameBuffer);
	image->AddChannelToFramebuffer<V4f>("rgba", {"R", "G", "B", "A"}, frameBuffer, false);
	image->AddChannelToFramebuffer<uint32_t>("id", {"id"}, frameBuffer, false);
	image->AddChannelToFramebuffer<float>("Z", { "Z" }, frameBuffer, false);
	image->AddChannelToFramebuffer<float>("ZBack", {"ZBack"}, frameBuffer, false);
	image->AddChannelToFramebuffer<V3f>("P", { "P.X", "P.Y", "P.Z" }, frameBuffer, true);
	image->AddChannelToFramebuffer<V3f>("N", { "N.X", "N.Y", "N.Z" }, frameBuffer, true);

	for(auto op: config.operations)
	    op->AddChannels(image, frameBuffer);

	reader.Read(frameBuffer);
	images.push_back(image);

	// Work around bad Arnold channels: non-color channels get multiplied by alpha.
	auto rgba = image->GetChannel<V4f>("rgba");
	for(auto it: image->channels)
	{
	    shared_ptr<DeepImageChannel> channel = it.second;
	    if(channel->needsAlphaDivide)
		channel->UnpremultiplyChannel(rgba);
	}
    }

    // Combine the images.
    shared_ptr<DeepImage> image;
    if(images.size() == 1)
	image = images[0];
    else
	image = DeepImageUtil::CombineImages(images);

    // Sort all samples by depth.  If we want to support volumes, this is where we'd do the rest
    // of "tidying", splitting samples where they overlap using splitVolumeSample.
    DeepImageUtil::SortSamplesByDepth(image);

    for(auto op: config.operations)
	op->Run(image);

    return true;
}

int main(int argc, char **argv)
{
    option opts[] = {
	{"stroke", required_argument, NULL, 0},
	{"combine", required_argument, NULL, 0},
	{"input", required_argument, NULL, 'i'},
	{"output", required_argument, NULL, 'o'},
	{"filename", required_argument, NULL, 'f'},
	{"layer", required_argument, NULL, 0},
	{"layer-mask", required_argument, NULL, 0},
	{"create-mask", required_argument, NULL, 0},
	{"save-flattened", required_argument, NULL, 0},
	{"save-layers", no_argument, NULL, 0},
	{0},
    };

    Config config;
    vector<pair<string,string>> accumulatedOptions;
    while(1)
    {
	    int index = -1;
	    int c = getopt_long(argc, argv, "i:o:f:", opts, &index);
	    if( c == -1 )
		    break;
	    switch( c )
	    {
	    case 0:
		config.ParseOption(opts[index].name, optarg? optarg:"", accumulatedOptions);
		break;
	    case 'i':
		config.sharedConfig.inputFilenames.push_back(optarg);
		break;
	    case 'o':
		config.sharedConfig.outputPath = optarg;
		break;
	    case 'f':
		config.ParseOption("filename", optarg? optarg:"", accumulatedOptions);
		break;
	    }
    }

    if(config.sharedConfig.inputFilenames.empty()) {
	fprintf(stderr, "Usage: exrflatten [--combine src,dst ...] -i input.exr [-i input2.exr ...] [-o output path] [-f filename pattern.exr]\n");
	return 1;
    }

    // If there are any accumulatedOptions left that weren't used by an operation, warn
    // about it.
    for(auto it: accumulatedOptions)
	printf("Warning: unused option %s\n", it.first.c_str());

    try {
	if(!FlattenFiles::flatten(config))
	    return 1;
    }
    catch(const exception &e)
    {
	fprintf(stderr, "%s\n", e.what());
	return 1;
    }


//    char buf[1024];
//    fgets(buf, 1000, stdin);
    return 0;
}

