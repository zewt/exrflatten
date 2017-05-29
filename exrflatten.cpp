#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>
#include <limits.h>

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

#include "EXROperation.h"
#include "EXROperation_WriteLayers.h"

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

struct Config
{
    bool ParseOption(string opt, string value);
    void Run() const;

    SharedConfig sharedConfig;
    vector<shared_ptr<EXROperation>> operations;
};

bool Config::ParseOption(string opt, string value)
{
    if(opt == "input")
    {
	sharedConfig.inputFilenames.push_back(value);
	return true;
    }
    else if(opt == "output")
    {
	sharedConfig.outputPath = value;
	return true;
    }
    else if(opt == "save-layers")
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

vector<pair<string,string>> GetArgs(int argc, char **argv)
{
    vector<pair<string,string>> results;
    for(int i = 1; i < argc; ++i)
    {
	string option = argv[i];
	if(option.substr(0, 2) != "--")
	{
	    printf("Warning: unrecognized argument %s\n", option.c_str());
	    continue;
	}
	option = option.substr(2);

	string argument;
	int pos = option.find('=');
	if(pos != string::npos)
	{
	    argument = option.substr(pos+1);
	    option = option.substr(0, pos);
	}

	results.push_back(make_pair(option, argument));
    }

    return results;
}

int main(int argc, char **argv)
{
    Config config;
    for(auto opt: GetArgs(argc, argv))
    {
	if(!config.ParseOption(opt.first, opt.second))
	    printf("Unrecognized argument: %s\n", opt.first.c_str());
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

