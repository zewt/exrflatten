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
    string name;
    int objectId = -1;
    string layerName;
    shared_ptr<SimpleImage> image;

    Layer(string name_, int width, int height)
    {
        name = name_;
        image = make_shared<SimpleImage>(width, height);
    }
};

struct OutputFilenames
{
    string main;
    string mask;
};

/*
 * Create a set of images, one per object ID in the input image, which can be combined with
 * additive blending to get the original image.  This is useful because the layers can be
 * edited simply on a per-object basis and composited in a simpler application like Photoshop.
 *
 * Normally, samples are composited with "over" compositing.  The order of samples is important:
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
void SeparateIntoAdditiveLayers(vector<Layer> &layers, shared_ptr<const DeepImage> image, const unordered_map<int,OutputFilenames> &objectIdNames)
{
    auto getLayerName = [objectIdNames](int objectId) {
        auto it = objectIdNames.find(objectId);
        if(it == objectIdNames.end())
            return OutputFilenames();
        return it->second;
    };

    unordered_map<string,shared_ptr<SimpleImage>> imagesPerObjectIdName;

    auto rgba = image->GetChannel<V4f>("rgba");
    auto id = image->GetChannel<uint32_t>("id");
    auto Z = image->GetChannel<float>("Z");
    auto mask = image->GetChannel<float>("mask");

    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            struct AccumulatedSample {
		V4f rgba = V4f(0,0,0,0);
		V4f masked_rgba = V4f(0,0,0,0);
		float mask = 0;
                int objectId = 0;
                float zNear = 0;
            };

            vector<AccumulatedSample> sampleLayers;
	    for(int s = 0; s < image->NumSamples(x, y); ++s)
	    {
                AccumulatedSample new_sample;
                new_sample.objectId = id->Get(x, y, s);
                new_sample.zNear = Z->Get(x, y, s);

		// If we have a mask, we'll multiply rgba by it to get a masked version.  However,
		// the mask will be premultiplied by rgba[3] too.  To get the real mask value, we
		// need to un-premultiply it.
		float alpha = rgba->Get(x, y, s)[3];
		float maskValue = alpha > 0.0001f? (mask->Get(x, y, s) / alpha):0;

		new_sample.rgba = new_sample.masked_rgba = rgba->Get(x,y,s);
		new_sample.masked_rgba = new_sample.rgba * maskValue;

                // new_sample.mask = mask->Get(x, y, s);

                // Apply the alpha term to each sample underneath this one.
                for(AccumulatedSample &sample: sampleLayers)
                {
		    sample.rgba *= 1-alpha;
		    sample.masked_rgba *= 1-alpha;
                    // sample.mask *= 1-alpha;
                }

                // Add the new sample.
                sampleLayers.push_back(new_sample);
            }

            // Combine samples by object ID, creating a layer for each.
            //
            // We could do this in one pass instead of two, but debugging is easier in two passes.
            const int pixelIdx = x + y*image->width;
/*            for(const AccumulatedSample &sample: sampleLayers)
            {
                image->data[pixelIdx].rgba += sample.rgba;
            } */

            auto getLayer = [&imagesPerObjectIdName, &layers](string layerName, int objectId, int width, int height)
            {
                auto it = imagesPerObjectIdName.find(layerName);
                if(it != imagesPerObjectIdName.end())
                    return it->second;

                layers.push_back(Layer("color", width, height));
                Layer &layer = layers.back();
                layer.objectId = objectId;
                layer.layerName = layerName;
                imagesPerObjectIdName[layerName] = layer.image;
                return layer.image;
            };

            for(const AccumulatedSample &sample: sampleLayers)
            {
                int objectId = sample.objectId;

                string layerName = getLayerName(objectId).main;
                if(layerName != "")
                {
                    auto out = getLayer(layerName, objectId, image->width, image->height);
                    for(int i = 0; i < 4; ++i)
			out->data[pixelIdx].rgba[i] += sample.rgba[i];
                }

                string maskName = getLayerName(objectId).mask;
                if(maskName != "")
                {
                    auto out = getLayer(maskName, objectId, image->width, image->height);
                    for(int i = 0; i < 4; ++i)
			out->data[pixelIdx].rgba[i] += sample.masked_rgba[i];
                }
                //                image->data[pixelIdx].mask += sample.mask;
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

    vector<DeepImageStroke::Config> strokes;

    // A list of (dst, src) pairs to combine layers before writing them.
    vector<pair<int,int>> combines;
};

namespace FlattenFiles
{
    bool flatten(const Config &config);
    string GetFrameNumberFromFilename(string s);
    string MakeOutputFilename(const Config &config, string output, const Layer &layer, string name);
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
string FlattenFiles::MakeOutputFilename(const Config &config, string output, const Layer &layer, string name)
{
    string outputName = output;

    // <name>: the name of the object ID that we got from the EXR file, or "#100" if we
    // only have a number.
    outputName = subst(outputName, "<name>", name);

    // <layer>: the output layer that we generated.  This is currently always "color".
    outputName = subst(outputName, "<layer>", layer.name);

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

    // Combine layers.  This just changes the object IDs of samples, so we don't need to re-sort.
    for(auto combine: config.combines)
	DeepImageUtil::CombineObjectId(image, combine.second, combine.first);

    // Separate the image into layers.
    vector<Layer> layers;
    SeparateIntoAdditiveLayers(layers, image, objectIdNames);

    shared_ptr<SimpleImage> flat = DeepImageUtil::CollapseEXR(image);
    layers.push_back(Layer("color", image->width, image->height));
    layers.back().layerName = "main";
    layers.back().image = flat;

    // Write the layers.
    for(const auto &layer: layers)
    {
        // Copy all image attributes, except for built-in EXR headers that we shouldn't set.
	DeepImageUtil::CopyLayerAttributes(image->header, layer.image->header);

        string outputName = MakeOutputFilename(config, config.outputPattern, layer, layer.layerName);

        try {
            layer.image->WriteEXR(outputName);
        }
        catch(const BaseExc &e)
        {
            fprintf(stderr, "%s\n", e.what());
            return false;
        }
        /*
        if(filenames.mask != "")
        {
            // layer.image->ApplyMask();

            string outputName = MakeOutputFilename(config.outputPattern, layer, filenames.mask);
            try {
                layer.image->WriteEXR(outputName);
            }
            catch(const BaseExc &e)
            {
                fprintf(stderr, "%s\n", e.what());
                return false;
            }
        }
        */
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
    V3f strokeColor(0,0,0);
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
		    if(opt == "stroke")
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

