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


struct Layer
{
    string layerName;
    string layerType;
    int order = 0;
    shared_ptr<SimpleImage> image;

    Layer(int width, int height)
    {
        image = make_shared<SimpleImage>(width, height);
    }
};

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
void SeparateLayer(
    int objectId,
    shared_ptr<SimpleImage> layer,
    shared_ptr<const DeepImage> image,
    const map<int,int> &layerOrder,
    shared_ptr<const TypedDeepImageChannel<float>> mask)
{
    auto rgba = image->GetChannel<V4f>("rgba");
    auto id = image->GetChannel<uint32_t>("id");

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

		if(mask != nullptr)
		{
		    // If we have a mask, we'll multiply rgba by it to get a masked version.  However,
		    // the mask will be premultiplied by rgba[3] too.  To get the real mask value, we
		    // need to un-premultiply it.
		    float alpha = sampleColor[3];
		    float maskValue = mask->Get(x, y, s);
		    maskValue = alpha > 0.0001f? (maskValue / alpha):0;
		    sampleColor *= maskValue;
		}

		color += sampleColor;
	    }

	    // Save the result.
	    layer->GetRGBA(x,y) = color;
	}
    }
}

struct Config
{
    vector<string> inputFilenames;
    string outputPath = "";
    string outputPattern = "<inputname> <ordername> <layer>.exr";

    struct LayerDesc
    {
	string layerName;
	int objectId;
    };
    vector<LayerDesc> layers;

    struct MaskDesc
    {
	string maskChannel;
	string maskName;
    };
    vector<MaskDesc> masks;

    vector<DeepImageStroke::Config> strokes;

    // A list of (dst, src) pairs to combine layers before writing them.
    vector<pair<int,int>> combines;
};

bool ParseConfig(Config &config, string opt, string value)
{
    if(opt == "layers")
    {
	vector<string> layerParts;
	split(value, ";", layerParts);

	for(string layerDesc: layerParts)
	{
	    // id=name
	    vector<string> descParts;
	    split(layerDesc, "=", descParts);
	    if(descParts.size() != 2)
	    {
		printf("Warning: ignored part of layer desc \"%s\"\n", layerDesc.c_str());
		continue;
	    }

	    Config::LayerDesc layer;
	    layer.objectId = atoi(descParts[0].c_str());
	    layer.layerName = descParts[1];
	    config.layers.push_back(layer);
	}
	return true;
    }
    else if(opt == "masks")
    {
	vector<string> maskParts;
	split(value, ";", maskParts);

	for(string maskDesc: maskParts)
	{
	    // channel=mask name
	    vector<string> descParts;
	    split(maskDesc, "=", descParts);
	    if(descParts.size() != 2)
	    {
		printf("Warning: ignored part of mask desc \"%s\"\n", maskDesc.c_str());
		continue;
	    }

	    Config::MaskDesc mask;
	    mask.maskName = descParts[1];
	    mask.maskChannel = descParts[0];
	    config.masks.push_back(mask);
	}
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
	config.combines.push_back(make_pair(dst, src));
    }
    else if(opt == "stroke")
    {
	config.strokes.emplace_back();
	config.strokes.back().ParseOptionsString(optarg);
    }

    return false;
}

void ReadConfigFromHeader(const Header &header, Config &config)
{
    for(auto it = header.begin(); it != header.end(); ++it)
    {
	string name = it.name();
	if(name.substr(0, 8) != "flatten/")
	    continue;
	string argName = name.substr(8);

	auto attr = dynamic_cast<const StringAttribute *>(&it.attribute());
	if(attr == nullptr)
	    continue;

	ParseConfig(config, argName, attr->value());
    }
}

namespace FlattenFiles
{
    bool flatten(Config config);
    string GetFrameNumberFromFilename(string s);
    string MakeOutputFilename(const Config &config, const Layer &layer);
};


// Given a filename like "abcdef.1234.exr", return "1234".
string FlattenFiles::GetFrameNumberFromFilename(string s)
{
    // abcdef.1234.exr -> abcdef.1234
    s = setExtension(s, "");

    auto pos = s.rfind(".");
    if(pos == string::npos)
	return "";

    string frameString = s.substr(pos+1);
    return frameString;
}

// Do simple substitutions on the output filename.
string FlattenFiles::MakeOutputFilename(const Config &config, const Layer &layer)
{
    string outputName = config.outputPattern;
    if(!config.outputPath.empty())
	outputName = config.outputPath + "/" + outputName;

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
    string inputName = config.inputFilenames[0];
    inputName = basename(inputName);
    inputName = setExtension(inputName, "");
    outputName = subst(outputName, "<inputname>", inputName);

    // <frame>: the input filename's frame number, given a "abcdef.1234.exr" filename.
    // It would be nice if there was an EXR attribute contained the frame number.
    outputName = subst(outputName, "<frame>", GetFrameNumberFromFilename(config.inputFilenames[0]));

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

bool FlattenFiles::flatten(Config config)
{
    vector<shared_ptr<DeepImage>> images;
    for(string inputFilename: config.inputFilenames)
    {
	try {
	    shared_ptr<DeepImage> image;

	    DeepImageReader reader;
	    image = reader.Open(inputFilename);

	    // Read any parts of the configuration that are stored in the header.
	    if(images.empty())
		ReadConfigFromHeader(image->header, config);

	    // Set up the channels we're interested in.
	    DeepFrameBuffer frameBuffer;
	    image->AddSampleCountSliceToFramebuffer(frameBuffer);
	    image->AddChannelToFramebuffer<V4f>("rgba", {"R", "G", "B", "A"}, frameBuffer);
	    image->AddChannelToFramebuffer<uint32_t>("id", {"id"}, frameBuffer);
	    image->AddChannelToFramebuffer<float>("Z", { "Z" }, frameBuffer);
	    image->AddChannelToFramebuffer<float>("ZBack", {"ZBack"}, frameBuffer);
	    image->AddChannelToFramebuffer<V3f>("P", { "P.X", "P.Y", "P.Z" }, frameBuffer);
	    image->AddChannelToFramebuffer<V3f>("N", { "N.X", "N.Y", "N.Z" }, frameBuffer);

	    // Also read channels used by masks.
	    for(auto maskDesc: config.masks)
		image->AddChannelToFramebuffer<float>(maskDesc.maskChannel, { maskDesc.maskChannel }, frameBuffer);

	    reader.Read(frameBuffer);
	    images.push_back(image);
	}
	catch(const BaseExc &e)
	{
	    // We don't include the filename here because OpenEXR's exceptions include the filename.
	    // (Unfortunately, the errors are also formatted awkwardly...)
	    fprintf(stderr, "%s\n", e.what());
	    return false;
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

    // If no layer was specified for the default object ID, add one at the beginning.
    bool hasDefaultObjectId = false;
    for(auto layer: config.layers)
	if(layer.objectId == DeepImageUtil::NO_OBJECT_ID)
	    hasDefaultObjectId = true;
    if(!hasDefaultObjectId)
    {
	Config::LayerDesc layerDesc;
	layerDesc.objectId = 0;
	layerDesc.layerName = "default";
	config.layers.insert(config.layers.begin(), layerDesc);
    }
    
/*    if(!config.layers.empty())
    {
        printf("Object ID names:\n");
        for(auto layer: config.layers)
            printf("Id %i: %s\n", layer.objectId, layer.layerName.c_str());
        printf("\n");
    }
*/
    // Apply strokes to layers.
    for(auto stroke: config.strokes)
	DeepImageStroke::AddStroke(stroke, image);

    // If we stroked any objects, re-sort samples, since new samples may have been added.
    if(!config.strokes.empty())
	DeepImageUtil::SortSamplesByDepth(image);

    map<int,int> layerOrder;
    int next = 0;
    for(auto layer: config.layers)
	layerOrder[layer.objectId] = next++;

    // Combine layers.  This just changes the object IDs of samples, so we don't need to re-sort.
    for(auto combine: config.combines)
    {
	DeepImageUtil::CombineObjectId(image, combine.second, combine.first);

	// Remove the layer we merged, so we don't waste time trying to create a layer for it.
	// Note that we do this after creating layerOrder.  We want to avoid creating the layer,
	// but leave the empty space in the layer ordering so filenames don't change if combining
	// is changed.
	layerOrder.erase(combine.second);
    }

    // Collapse any object IDs that aren't associated with layers into the default layer.
    {
	auto id = image->GetChannel<uint32_t>("id");
	for(int y = 0; y < image->height; y++)
	{
	    for(int x = 0; x < image->width; x++)
	    {
		for(int s = 0; s < image->NumSamples(x, y); ++s)
		{
		    uint32_t value = id->Get(x,y,s);
		    if(layerOrder.find(value) == layerOrder.end())
			id->Get(x,y,s) = DeepImageUtil::NO_OBJECT_ID;
		}
	    }
	}
    }

    // Separate the image into layers.
    vector<Layer> layers;
    int nextOrder = 1;
    auto getLayer = [&layers, &nextOrder](string layerName, string layerType, int width, int height, bool ordered)
    {
	layers.push_back(Layer(width, height));
	Layer &layer = layers.back();
	layer.layerName = layerName;
	layer.layerType = layerType;
	if(ordered)
	    layer.order = nextOrder++;
	return layer.image;
    };

    for(auto layerDesc: config.layers)
    {
	// Skip this layer if we've removed it from layerOrder.
	if(layerOrder.find(layerDesc.objectId) == layerOrder.end())
	    continue;

	string layerName = layerDesc.layerName;

	auto colorOut = getLayer(layerName, "color", image->width, image->height, true);
	SeparateLayer(layerDesc.objectId, colorOut, image, layerOrder, nullptr);

	for(auto maskDesc: config.masks)
	{
	    auto mask = image->GetChannel<float>(maskDesc.maskChannel);
	    if(mask)
	    {
		auto maskOut = getLayer(layerName, maskDesc.maskName, image->width, image->height, false);
		SeparateLayer(layerDesc.objectId, maskOut, image, layerOrder, mask);
    	    }
	}
    }

    for(const auto &layer: layers)
	printf("Layer: %s, %s\n", layer.layerName.c_str(), layer.layerType.c_str());

    shared_ptr<SimpleImage> flat = DeepImageUtil::CollapseEXR(image);
    layers.push_back(Layer(image->width, image->height));
    layers.back().layerName = "main";
    layers.back().layerType = "color";
    layers.back().image = flat;

    // Write the layers.
    for(const auto &layer: layers)
    {
	// Don't write this layer if it's completely empty.
	if(layer.image->IsEmpty())
	    continue;
	
	// Copy all image attributes, except for built-in EXR headers that we shouldn't set.
	DeepImageUtil::CopyLayerAttributes(image->header, layer.image->header);

        string outputName = MakeOutputFilename(config, layer);
	printf("Writing %s\n", outputName.c_str());

        try {
            layer.image->WriteEXR(outputName);
        }
        catch(const BaseExc &e)
        {
            fprintf(stderr, "%s\n", e.what());
            return false;
        }
    }
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
	{"layers", required_argument, NULL, 0},
	{"masks", required_argument, NULL, 0},
	{0},
    };

    Config config;
    while(1)
    {
	    int index = -1;
	    int c = getopt_long(argc, argv, "i:o:f:", opts, &index);
	    if( c == -1 )
		    break;
	    switch( c )
	    {
	    case 0:
		ParseConfig(config, opts[index].name, optarg? optarg:"");
		break;
	    case 'i':
		config.inputFilenames.push_back(optarg);
		break;
	    case 'o':
		config.outputPath = optarg;
		break;
	    case 'f':
		config.outputPattern = optarg;
		break;
	    }
    }

    if(config.inputFilenames.empty()) {
	fprintf(stderr, "Usage: exrflatten [--combine src,dst ...] -i input.exr [-i input2.exr ...] [-o output path] [-f filename pattern.exr]\n");
	return 1;
    }

    if(!FlattenFiles::flatten(config))
        return 1;

//    char buf[1024];
//    fgets(buf, 1000, stdin);
    return 0;
}

