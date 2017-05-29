#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>
#include <limits.h>
#include "getopt.h"

#include <algorithm>
#include <functional>
#include <vector>

// Too fine-grained:
#include <OpenEXR/ImathBox.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/Iex.h>

#include "DeepImage.h"
#include "DeepImageMasks.h"
#include "DeepImageStroke.h"
#include "DeepImageUtil.h"
#include "helpers.h"

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
#include "EXROperation_WriteLayers.h"

// Collapse the image to a flat file, and save a non-deep EXR.
class EXROperation_SaveFlattenedImage: public EXROperation
{
public:
    EXROperation_SaveFlattenedImage(const SharedConfig &sharedConfig_, string args):
	sharedConfig(sharedConfig_)
    {
	filename = args;
    }

    void Run(shared_ptr<DeepImage> image) const
    {
	auto flat = DeepImageUtil::CollapseEXR(image);

	string f = GetFilename();
	printf("Writing %s\n", f.c_str());
	flat->WriteEXR(f);
    }

private:
    string GetFilename() const
    {
	string result = sharedConfig.outputPath;
	if(!result.empty())
	    result += "/";
	result += filename;
	return result;
    }

    string filename;
    const SharedConfig &sharedConfig;
};

// Use DeepImageStroke to add a stroke.
class EXROperation_Stroke: public EXROperation
{
public:
    EXROperation_Stroke(string args)
    {
	strokeDesc.ParseOptionsString(args);
    }

    void Run(shared_ptr<DeepImage> image) const
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

// Use CreateMask to create a mask and add it as an EXR channel.
class EXROperation_CreateMask: public EXROperation
{
public:
    EXROperation_CreateMask(string args)
    {
	createMask.ParseOptionsString(args);
    }

    void Run(shared_ptr<DeepImage> image) const
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
    bool ParseOption(string opt, string value);
    void Run() const;

    SharedConfig sharedConfig;
    vector<shared_ptr<EXROperation>> operations;
};

bool Config::ParseOption(string opt, string value)
{
    if(opt == "save-layers")
    {
	operations.push_back(make_shared<EXROperation_WriteLayers>(sharedConfig));
	return true;
    }
    else if(opt == "create-mask")
    {
	operations.push_back(make_shared<EXROperation_CreateMask>(value));
	return true;
    }
    else if(opt == "stroke")
    {
	operations.push_back(make_shared<EXROperation_Stroke>(value));
	return true;
    }
    else if(opt == "save-flattened")
    {
	operations.push_back(make_shared<EXROperation_SaveFlattenedImage>(sharedConfig, value));
	return true;
    }

    if(operations.empty())
	return false;

    // We don't know what this option is.  See if it's an option for the most
    // recent operation.
    shared_ptr<EXROperation> op = operations.back();
    return op->AddArgument(opt, value);
}

void Config::Run() const
{
    vector<shared_ptr<DeepImage>> images;
    for(string inputFilename: sharedConfig.inputFilenames)
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

	for(auto op: operations)
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

    for(auto op: operations)
	op->Run(image);
}

int main(int argc, char **argv)
{
    // XXX: This getopt-style option list doesn't scale.
    option opts[] = {
	{"input", required_argument, NULL, 'i'},
	{"output", required_argument, NULL, 'o'},
	{"stroke", required_argument, NULL, 0},
	{"combine", required_argument, NULL, 0},
	{"filename-pattern", required_argument, NULL, 0},
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
	    int c = getopt_long(argc, argv, "i:o:", opts, &index);
	    if( c == -1 )
		    break;
	    switch( c )
	    {
	    case 0:
		if(!config.ParseOption(opts[index].name, optarg? optarg:""))
		    printf("Unrecognized argument: %s\n", opts[index].name);

		break;
	    case 'i':
		config.sharedConfig.inputFilenames.push_back(optarg);
		break;
	    case 'o':
		config.sharedConfig.outputPath = optarg;
		break;
	    }
    }

    if(config.sharedConfig.inputFilenames.empty()) {
	fprintf(stderr, "No input files were specified.\n");
	return 1;
    }

    try {
	config.Run();
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

