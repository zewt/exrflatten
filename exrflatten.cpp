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
    shared_ptr<SimpleImage> image;

    Layer(int width, int height)
    {
        image = make_shared<SimpleImage>(width, height);
    }
};

struct OutputFilenames
{
    string main;
    string mask;
};

/*
 * Each pixel in an OpenEXR image can have multiple samples, and each sample can be tagged
 * with a different object ID.  Normally to composite a deep EXR image into a regular image
 * software needs to understand deep samples, to composite each sample, which makes them hard
 * to use in traditional tools like Photoshop.  When you import the image, you just get a flat
 * image and can't manipulate individual objects because the importer has to discard the deep
 * data.
 *
 * Deep samples are normally composited with "over" compositing.  Transform the samples to an
 * equivalent set of samples that can be composited with "add" instead.  This has a useful property:
 * the order the layers are composited together no longer matters.  This means we can output
 * samples to arbitrary layers, and if you add all the layers together you'll get the original
 * image.  That's useful since we can now import each layer into a simple editing tool, set it to
 * additive blending, and you can make adjustments to the individual layers without the tool
 * actually needing to have native deep compositing support.
 *
 * Note: this originally exported as additive layers, and the data flow still converts to additive
 * layers, but the resulting layers are now actually converted back to "over" using a separate
 * algorithm.  It still gives the same results, but is easier to work with.  This algorithm is
 * still defined in terms of converting to additive layers since that's how it was created.
 * 
 * With "over" compositing, the order of samples is important:
 * 
 * r = 0 // current color
 * r = r0*a0 + r*(1-a0); // overlay r0/a0, and reduce everything under it
 * r = r1*a1 + r*(1-a1); // overlay r1/a1, and reduce everything under it
 * r = r2*a2 + r*(1-a2); // overlay r2/a2, and reduce everything under it
 * ...
 * 
 * which iteratively adds each sample on top to the output, and reduces the contribution
 * of everything underneath it in the process.  This is repeated for each RGBA channel.
 * 
 * We can convert each sample to an additive sample by determining the final contribution of
 * the sample to the pixel.  For example, if we have three "over" samples:
 *
 * 1.0
 * 0.5
 * 0.5
 *
 * the second sample (0.5) reduces the first by 0.5, giving 0.5 0.5, and the third sample reduces
 * the first two by 0.5, giving 0.25 0.25 0.5.  This means that the first sample contributes 25%
 * towards the final result.
 *
 * We want to know the final contribution of each sample at the end, so instead we do:
 * 
 * r[0] = r0*a0         // r*(1-a0) is zero since this is the first layer
 * 
 * r[1]= r1*a1          // overlay r1/a1
 * r[0] *= (1-a1)       // and reduce everything under it
 * 
 * r[2] = r2*a2         // overlay r2/a2
 * r[1] *= (1-a2)       // and reduce everything under it
 * r[0] *= (1-a2)       // and reduce everything under it
 * 
 * This tracks each term separately: instead of applying the r*(1-a0) term accumulatively,
 * we track its effect on each individual sample underneath it.  Adding r[0]+r[1]+r[2] 
 * would be equivalent to the normal compositing formula.
 *
 * Since this is simple addition, the order of samples no longer matters.  We can then
 * combine samples by object ID, and the resulting images can be added together with simple
 * additive blending.
 */

void SeparateIntoAdditiveLayer(
    int objectId,
    shared_ptr<SimpleImage> layer,
    shared_ptr<const DeepImage> image,
    shared_ptr<const TypedDeepImageChannel<float>> mask)
{
    auto rgba = image->GetChannel<V4f>("rgba");
    auto id = image->GetChannel<uint32_t>("id");

    for(int y = 0; y < image->height; y++)
    {
	for(int x = 0; x < image->width; x++)
	{
	    auto rgba = image->GetChannel<V4f>("rgba");

	    // Figure out the total additive contribution of objectId for this pixel.
	    V4f color(0,0,0,0);
	    for(int s = 0; s < image->NumSamples(x, y); ++s)
	    {
		V4f sampleColor = rgba->Get(x,y,s);

		// If this sample is part of this layer, composite it in.  If it's not, it still causes this
		// color to become less visible, so apply alpha, but don't add color.
		color = color*(1-sampleColor[3]);

		if(id->Get(x, y, s) != objectId)
		    continue;

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

void FixOpacity(
    int objectId,
    shared_ptr<SimpleImage> layer,
    shared_ptr<const DeepImage> image,
    const vector<int> &layerObjectIds)
{
    map<int,int> layerOrder;
    for(int i = 0; i < layerObjectIds.size(); ++i)
	layerOrder[layerObjectIds[i]] = i;

    auto rgba = image->GetChannel<V4f>("rgba");
    auto id = image->GetChannel<uint32_t>("id");

    for(int y = 0; y < image->height; y++)
    {
	for(int x = 0; x < image->width; x++)
	{
	    vector<float> SampleVisibilities = DeepImageUtil::GetSampleVisibility(image, x, y);

	    // If true, this is the bottommost layer for this pixel that has any influence.
	    // If false, an earlier layer is visible underneath this one.
	    bool bottomLayer = true;

	    for(int s = 0; s < image->NumSamples(x, y); ++s)
	    {
		int actualObjectId = id->Get(x, y, s);

		if(actualObjectId != objectId)
		{
		    // This is a different layer, so skip it.
		    //
		    // If this layer is below the one we're processing, and it has any visibility, then our layer isn't
		    // the bottommost visible layer.
		    if(layerOrder.at(actualObjectId) < layerOrder.at(objectId))
		    {
			float visibility = SampleVisibilities[s];
			if(visibility > 0.00001f)
			    bottomLayer = false;
		    }

		    // This sample isn't for the object ID we're processing.
		    continue;
		}
	    }

	    if(x == 555 && y == 288)
		printf("bottomLayer %i\n", bottomLayer);

	    if(!bottomLayer)
		continue;

	    // We've set the RGBA color for this layer that will give a correct result if the image is
	    // composited on a black background.  However, it'll be wrong if there's no background.
	    // For example, if we have two layers with samples:
	    //
	    // #FF0000 alpha = 1.0
	    // #00FF00 alpha = 0.5
	    //
	    // then we've created additive layers that look like this:
	    //
	    // #FF0000 alpha = 0.5
	    // #00FF00 alpha = 0.5
	    //
	    // This gives a correct result when there's a black layer underneath, but without the black
	    // layer the image is transparent when it should be opaque.  This makes it impossible to put
	    // the layers on top of a different background.
	    //
	    // Correct this by making the bottommost visible layer more opaque.  Figure out how opaque the
	    // image should be at this layer.
	    // In the above case, 
	    // since we want the whole image to be opaque
	    // end up back at:
	    //
	    // #FF0000 alpha = 1.0
	    // #00FF00 alpha = 0.5


	    // For over compositing, we need to be careful to get the correct final alpha value.
	    // If the whole image is opaque, the bottommost visible layer for each pixel needs to
	    // be opaque.  This may not be the first layer.  If we have three layers, and the bottom
	    // layer is completely covered by the other two layers somewhere, we want the second layer
	    // to be opaque instead of the first.  That way, we'll still get correct results if the
	    // bottom layer is hidden.  If we only made the bottom layer opaque, and the bottom layer
	    // was hidden, the image would end up being transparent.

	    // First, figure out how transparent the image should be if we look only at this layer and
	    // the layers on top of it.
	    float alphaThisLayerAndUp = 0;
	    for(int s = 0; s < image->NumSamples(x, y); ++s)
	    {
		if(layerOrder.at(id->Get(x, y, s)) < layerOrder.at(objectId))
		    continue;

		float alpha = rgba->Get(x,y,s)[3];
		alphaThisLayerAndUp = alphaThisLayerAndUp*(1-alpha) + alpha;
	    }
	    if(x == 301 && y == 228)
	    {
		printf("id %i, alpha from here: %f\n", objectId, alphaThisLayerAndUp);
	    }

	    V4f &color = layer->GetRGBA(x,y);
	    float alpha = color[3];

	    if(x == 301 && y == 228)
		printf("from: %f %f %f %f\n",
		    color[0], color[1], color[2], color[3]);

	    if(alpha > 0.00001f)
	    {
//		color /= alpha;
//		color *= alphaThisLayerAndUp;

		color[3] /= alpha;
		color[3] *= alphaThisLayerAndUp;
		if(x == 301 && y == 228)
		    printf("result: %f %f %f %f\n",
			color[0], color[1], color[2], color[3]);
	    }
	}
    }
}

void readObjectIdNames(const Header &header, unordered_map<int,OutputFilenames> &objectIdNames)
{
    for(auto it = header.begin(); it != header.end(); ++it)
    {
        auto &attr = it.attribute();
        if(strcmp(attr.typeName(), "string"))
            continue;

        string headerName = it.name();
        if(headerName.substr(0, 9) == "ObjectId/")
        {
            string idString = headerName.substr(9);
            int id = atoi(idString.c_str());
            const StringAttribute &value = dynamic_cast<const StringAttribute &>(attr);
            objectIdNames[id].main = value.value();
        }
        else if(headerName.substr(0, 5) == "Mask/")
        {
            string idString = headerName.substr(5);
            int id = atoi(idString.c_str());

            const StringAttribute &value = dynamic_cast<const StringAttribute &>(attr);
            objectIdNames[id].mask = value.value();
        }
    }
}

struct Config
{
    string inputFilename;
    string outputPattern;
    bool outputAdditiveLayers = false;

    vector<DeepImageStroke::Config> strokes;

    // A list of (dst, src) pairs to combine layers before writing them.
    vector<pair<int,int>> combines;
};

namespace FlattenFiles
{
    bool flatten(const Config &config);
    string GetFrameNumberFromFilename(string s);
    string MakeOutputFilename(const Config &config, string output, const Layer &layer);
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
string FlattenFiles::MakeOutputFilename(const Config &config, string output, const Layer &layer)
{
    string outputName = output;

    // <name>: the name of the object ID that we got from the EXR file, or "#100" if we
    // only have a number.
    outputName = subst(outputName, "<name>", layer.layerName);

    // <layer>: the output layer that we generated.  This is currently always "color".
    outputName = subst(outputName, "<layer>", layer.layerType);

    // <inputname>: the input filename, with the directory and ".exr" removed.
    string inputName = config.inputFilename;
    inputName = basename(inputName);
    inputName = setExtension(inputName, "");
    outputName = subst(outputName, "<inputname>", inputName);

    // <frame>: the input filename's frame number, given a "abcdef.1234.exr" filename.
    // It would be nice if there was an EXR attribute contained the frame number.
    outputName = subst(outputName, "<frame>", GetFrameNumberFromFilename(config.inputFilename));

    static bool warned = false;
    if(!warned && outputName == output)
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

bool FlattenFiles::flatten(const Config &config)
{
    unordered_map<int,OutputFilenames> objectIdNames;

    shared_ptr<DeepImage> image;
    try {
	DeepImageReader reader;
	image = reader.Open(config.inputFilename);

	// Set up the channels we're interested in.
	DeepFrameBuffer frameBuffer;
	image->AddSampleCountSliceToFramebuffer(frameBuffer);
	image->AddChannelToFramebuffer<V4f>("rgba", {"R", "G", "B", "A"}, frameBuffer);
	image->AddChannelToFramebuffer<uint32_t>("id", {"id"}, frameBuffer);
	image->AddChannelToFramebuffer<float>("Z", { "Z" }, frameBuffer);
	image->AddChannelToFramebuffer<float>("ZBack", {"ZBack"}, frameBuffer);
	image->AddChannelToFramebuffer<float>("mask", { "mask" }, frameBuffer);
	image->AddChannelToFramebuffer<V3f>("P", { "P.X", "P.Y", "P.Z" }, frameBuffer);
	image->AddChannelToFramebuffer<V3f>("N", { "N.X", "N.Y", "N.Z" }, frameBuffer);

	reader.Read(frameBuffer);
    }
    catch(const BaseExc &e)
    {
        // We don't include the filename here because OpenEXR's exceptions include the filename.
        // (Unfortunately, the errors are also formatted awkwardly...)
        fprintf(stderr, "%s\n", e.what());
        return false;
    }

    DeepImageUtil::ReplaceHighObjectIds(image);

    //const ChannelList &channels = image->header.channels();
    //for(auto i = channels.begin(); i != channels.end(); ++i)
    //    printf("Channel %s has type %i\n", i.name(), i.channel().type);

#if 0
    set<string> layerNames;
    channels.layers(layerNames);
    for(const string &layerName: layerNames)
    {
        printf("layer: %s", layerName.c_str());
        ChannelList::ConstIterator layerBegin, layerEnd;
        channels.channelsInLayer(layerName, layerBegin, layerEnd);
        for(auto j = layerBegin; j != layerEnd; ++j)
        {
            cout << "\tchannel " << j.name() << endl;
        }
    }
#endif

    // Sort all samples by depth.  If we want to support volumes, this is where we'd do the rest
    // of "tidying", splitting samples where they overlap using splitVolumeSample.
    DeepImageUtil::SortSamplesByDepth(image);

    // If this file has names for object IDs, read them.
    readObjectIdNames(image->header, objectIdNames);

    // Set the layer with the object ID 0 to "default", unless a name for that ID
    // was specified explicitly.
    if(objectIdNames.find(DeepImageUtil::NO_OBJECT_ID) == objectIdNames.end())
	objectIdNames[DeepImageUtil::NO_OBJECT_ID].main = "default";

    
/*    if(!objectIdNames.empty())
    {
        printf("Object ID names:\n");
        for(auto it: objectIdNames)
            printf("Id %i: %s\n", it.first, it.second.c_str());
        printf("\n");
    } */

    // Apply strokes to layers.
    for(auto stroke: config.strokes)
	DeepImageStroke::AddStroke(stroke, image);

    // If we stroked any objects, re-sort samples, since new samples may have been added.
    if(!config.strokes.empty())
	DeepImageUtil::SortSamplesByDepth(image);

    // XXX: need a way to specify the layer order
    vector<int> layerObjectIds = {0, 1000, 1001};

    // Combine layers.  This just changes the object IDs of samples, so we don't need to re-sort.
    for(auto combine: config.combines)
    {
	DeepImageUtil::CombineObjectId(image, combine.second, combine.first);

	// Remove the layer we merged from layerObjectIds, so we don't waste time trying to create
	// a layer for it.
	auto it = find(layerObjectIds.begin(), layerObjectIds.end(), combine.second);
	if(it != layerObjectIds.end())
	    layerObjectIds.erase(it, it+1);
    }

    // Separate the image into layers.
    vector<Layer> layers;
    for(int objectId: layerObjectIds)
    {
	auto getLayer = [&layers](string layerName, string layerType, int width, int height)
	{
	    layers.push_back(Layer(width, height));
	    Layer &layer = layers.back();
	    layer.layerName = layerName;
	    layer.layerType = layerType;
	    return layer.image;
	};

	OutputFilenames filenames = map_get(objectIdNames, objectId, OutputFilenames());
	if(filenames.main != "")
	{
	    string layerName = filenames.main;
	    auto colorOut = getLayer(layerName, "color", image->width, image->height);
	    SeparateIntoAdditiveLayer(objectId, colorOut, image, nullptr);
	    FixOpacity(objectId, colorOut, image, layerObjectIds);
	}

	auto mask = image->GetChannel<float>("mask");
	if(filenames.mask != "" && mask)
	{
	    auto maskOut = getLayer(filenames.mask, "mask", image->width, image->height);
	    SeparateIntoAdditiveLayer(objectId, maskOut, image, mask);
    	}
    }

    for(const auto &layer: layers)
	printf("Layer: %s, %s\n", layer.layerName.c_str(), layer.layerType.c_str());

    if(!config.outputAdditiveLayers)
    {
	// Convert the additive layers to equivalent over layers.
	vector<shared_ptr<SimpleImage>> blendedLayers;
	for(auto &layer: layers)
	{
	    if(layer.layerType == "color")
		blendedLayers.push_back(layer.image);
	}
	SimpleImage::ConvertAdditiveLayersToOver(blendedLayers);
    }

    shared_ptr<SimpleImage> flat = DeepImageUtil::CollapseEXR(image);
    layers.push_back(Layer(image->width, image->height));
    layers.back().layerName = "main";
    layers.back().layerType = "color";
    layers.back().image = flat;

    // Write the layers.
    for(const auto &layer: layers)
    {
        // Copy all image attributes, except for built-in EXR headers that we shouldn't set.
	DeepImageUtil::CopyLayerAttributes(image->header, layer.image->header);

        string outputName = MakeOutputFilename(config, config.outputPattern, layer);
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
	{"stroke-radius", required_argument, NULL, 0},
	{"stroke", required_argument, NULL, 0},
	{"combine", required_argument, NULL, 0},
	{0},
    };

    Config config;
    float strokeRadius = 1.0f;
    V4f strokeColor(0,0,0,1);
    while(1)
    {
	    int index = -1;
	    int c = getopt_long(argc, argv, "", opts, &index);
	    if( c == -1 )
		    break;
	    switch( c )
	    {
	    case 0:
	    {
		    string opt = opts[index].name;
		    if(opt == "add")
			config.outputAdditiveLayers = true;
		    else if(opt == "stroke")
		    {
			config.strokes.emplace_back();
			config.strokes.back().objectId = atoi(optarg);
			config.strokes.back().radius = strokeRadius;
			config.strokes.back().strokeColor = strokeColor;
		    }
		    else if(opt == "stroke-radius")
		    {
			strokeRadius = (float) atof(optarg);
		    }
		    else if(opt == "combine")
		    {
			const char *split = strchr(optarg, ',');
			if(split == NULL)
			{
			    printf("Invalid --combine (ignored)\n");
			    break;
			}

			int dst = atoi(optarg);
			int src = atoi(split+1);
			config.combines.push_back(make_pair(dst, src));
		    }
	    }
	    }
    }

    if(argc < optind + 1) {
	    fprintf(stderr, "Usage: exrflatten [--combine src,dst ...] input.exr output\n");
	    return 1;
    }

    config.inputFilename = argv[optind];
    config.outputPattern = argv[optind+1];

    if(!FlattenFiles::flatten(config))
        return 1;

//    char buf[1024];
//    fgets(buf, 1000, stdin);
    return 0;
}

