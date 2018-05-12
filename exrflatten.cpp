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
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/Iex.h>

#include "DeepImage.h"
#include "DeepImageUtil.h"
#include "helpers.h"

#include "EXROperation.h"
#include "EXROperation_CreateMask.h"
#include "EXROperation_WriteLayers.h"
#include "EXROperation_FixArnold.h"
#include "EXROperation_Stroke.h"

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
    EXROperation_SaveFlattenedImage(const SharedConfig &sharedConfig_, string opt, vector<pair<string,string>> args):
        sharedConfig(sharedConfig_)
    {
        filename = opt;

        for(auto it: args)
        {
            string arg = it.first;
            string value = it.second;

            if(arg == "object-id")
                objectIds.insert(atoi(value.c_str()));
            else if(arg == "channel")
                channel = value;
        }
    }

    void AddChannels(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const
    {
        image->AddChannelToFramebuffer<uint32_t>(sharedConfig.GetIdChannel(image->header), frameBuffer);
        image->AddChannelToFramebuffer<V4f>(channel, frameBuffer);
    }

    void Run(shared_ptr<EXROperationState> state) const
    {
        string f = sharedConfig.GetFilename(filename);
        printf("Writing %s\n", f.c_str());

        auto flat = DeepImageUtil::CollapseEXR(state->image,
            state->image->GetChannel<uint32_t>(sharedConfig.GetIdChannel(state->image->header)),
            state->image->GetChannel<V4f>(channel),
            nullptr,
            objectIds);

        // Add the main RGBA layer.
        vector<SimpleImage::EXRLayersToWrite> layers;
        layers.push_back(SimpleImage::EXRLayersToWrite(flat));
        SimpleImage::WriteImages(f, layers);
    }

private:
    string filename;
    const SharedConfig &sharedConfig;
    set<int> objectIds;
    string channel = "rgba";
};

class EXROperation_Stats: public EXROperation
{
public:
    EXROperation_Stats(const SharedConfig &sharedConfig_, string opt, vector<pair<string,string>> args):
        sharedConfig(sharedConfig_)
    {
        filename = opt;

        for(auto it: args)
        {
            string arg = it.first;
            string value = it.second;

            if(arg == "object-id")
                objectIds.insert(atoi(value.c_str()));
        }
    }

    void AddChannels(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const
    {
        image->AddChannelToFramebuffer<uint32_t>(sharedConfig.GetIdChannel(image->header), frameBuffer);
    }

    void Run(shared_ptr<EXROperationState> state) const
    {
        const auto rgba = state->image->GetChannel<V4f>("rgba");

        int totalSamples = 0;
        int totalEmptyPixels = 0;
        int totalVisiblePixels = 0;
        for(int y = 0; y < state->image->height; y++)
        {
            for(int x = 0; x < state->image->width; x++)
            {
                int samples = state->image->NumSamples(x, y);
                totalSamples += samples;
                if(samples == 0)
                    totalEmptyPixels++;
                else
                    totalVisiblePixels++;
            }
        }

        printf("Average samples per pixel: %f\n", double(totalSamples) / totalVisiblePixels );
        printf("Visible pixels: %0f%%\n", 100*(double(totalVisiblePixels) / (totalVisiblePixels+totalEmptyPixels)) );
    }

private:
    string filename;
    const SharedConfig &sharedConfig;
    set<int> objectIds;
};

struct Config
{
    void ParseOptions(const vector<pair<string,string>> &options);
    void Run() const;
    typedef function<shared_ptr<EXROperation>(const SharedConfig &sharedConfig, string opt, vector<pair<string,string>> arguments)> CreateFunc;

    SharedConfig sharedConfig;
    vector<shared_ptr<EXROperation>> operations;
};

template<typename T>
shared_ptr<T> CreateOp(const SharedConfig &sharedConfig, string opt, vector<pair<string,string>> arguments)
{
    return make_shared<T>(sharedConfig, opt, arguments);
}

static map<string, Config::CreateFunc> Operations = {
    { "save-layers", CreateOp<EXROperation_WriteLayers> },
    { "create-mask", CreateOp<EXROperation_CreateMask> },
    { "stroke", CreateOp<EXROperation_Stroke> },
    { "save-flattened", CreateOp<EXROperation_SaveFlattenedImage> },
    { "stats", CreateOp<EXROperation_Stats> },
};

void Config::ParseOptions(const vector<pair<string,string>> &options)
{
    string currentOp;
    vector<pair<string,string>> accumulatedOptions;

    auto finalizeOp = [&] {
        if(currentOp.empty())
            return;

        string firstOption = accumulatedOptions[0].second;
        vector<pair<string,string>> options(accumulatedOptions.begin()+1, accumulatedOptions.end());
        auto op = Operations.at(currentOp)(sharedConfig, firstOption, options);
        operations.push_back(op);

        accumulatedOptions.clear();
        currentOp.clear();
    };

    for(auto it: options)
    {
        string opt = it.first;
        string value = it.second;

        // See if this is a global option.
        if(sharedConfig.ParseOption(opt, value))
        {
            // There are too many confusing situations if global operations can come in between
            // operations, so require that they come first.
            //
            // For example, if we allow specifying --output we can allow a different output directory
            // for each operation, but if you say
            // --output=output --save-layers --output=output2 --save-layers
            //
            // it's unclear whether the second --output is meant to affect the first --save-layers or
            // not, since normally options for an operation come after the operation, but global options
            // typically come before it.  This isn't useful enough for the complication.
            if(!currentOp.empty() || !operations.empty())
                throw StringException("Global options must precede operations: --" + opt);
            continue;
        }

        // See if this is an option to create a new operation, eg. --stroke.
        if(Operations.find(opt) != Operations.end())
        {
            // This is a new operation.  Finish the previous one, creating it and passing it
            // any options we saw since the operation command.
            finalizeOp();

            // Save the function to create the operation.  We'll create it after we've collected
            // its arguments.
            currentOp = opt;

            // Save the operation argument itself.  The option will be the argument when creating
            // the operation, eg. the 1 in --stroke=1.
            accumulatedOptions.emplace_back(opt, value);
            continue;
        }

        // We don't know what this option is.  Add it to accumulatedOptions, so we send it
        // with the current operation's arguments.  
        if(currentOp.empty())
            printf("Unrecognized argument: %s\n", opt.c_str());
        else
            accumulatedOptions.emplace_back(opt, value);
    }

    // Finish creating the last op.
    finalizeOp();

    if(sharedConfig.inputFilenames.empty())
        throw StringException("No input files were specified.");
    if(operations.empty())
        throw StringException("No operations were specified.");
}

void Config::Run() const
{
    if(sharedConfig.inputFilenames.empty())
        throw StringException("No input files");

    vector<shared_ptr<DeepImage>> images;
    for(string inputFilename: sharedConfig.inputFilenames)
    {
        DeepImageReader reader;
        shared_ptr<DeepImage> image = reader.Open(inputFilename);

        // Set up the channels we're interested in.
        DeepFrameBuffer frameBuffer;
        image->AddSampleCountSliceToFramebuffer(frameBuffer);
        image->AddChannelToFramebuffer<V4f>("rgba", frameBuffer);
        image->AddChannelToFramebuffer<float>("Z", frameBuffer);

        // We don't actually need this right now, and it's not available for shallow renders.
        // It'd be needed for handling volumes in deep images.
        // image->AddChannelToFramebuffer<float>("ZBack", frameBuffer);

        for(auto op: operations)
            op->AddChannels(image, frameBuffer);

        // If any channel/layer was required above that isn't in the image, print
        // an error and stop.
        string missing = "";
        for(auto channel: image->missingChannels)
        {
            if(!missing.empty())
                missing += ", ";
            missing += channel;
        }
        if(!missing.empty())
            throw StringException(ssprintf("%s: Missing input channels: %s", inputFilename.c_str(), missing.c_str()));

        reader.Read(frameBuffer);
        images.push_back(image);

        // Handle unpremultiplication.
        if(image->header.findTypedAttribute<StringAttribute>("arnold/version") != NULL)
        {
            auto A = image->GetAlphaChannel();
            for(auto it: image->channels)
            {
                shared_ptr<DeepImageChannel> channel = it.second;
                if(channel->needsUnpremultiply)
                    channel->UnpremultiplyChannel(A);
            }
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

    auto state = make_shared<EXROperationState>();
    state->image = image;
    shared_ptr<EXROperation> prevOp;
    for(auto op: operations)
    {
        // If this op is a different type than the previous, and we have new images waiting to be
        // merged into the main one, do so now.
        if(prevOp && typeid(*prevOp.get()) != typeid(*op.get()) && !state->waitingImages.empty())
        {
            // printf("Merging images\n");
            state->CombineWaitingImages();
        }

        op->Run(state);
        prevOp = op;
    }
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

void CompositeOver(SimpleImage &image, shared_ptr<const SimpleImage> over)
{
    for(int y = 0; y < image.height; y++)
    {
        for(int x = 0; x < image.width; x++)
        {
            V4f c2 = over->GetRGBA(x, y);

            V4f &c1 = image.GetRGBA(x, y);
            c1 = (c1 * (1-c2.w)) + c2;
        }
    }
}

int main(int argc, char **argv)
{
    try {
        Config config;
        config.ParseOptions(GetArgs(argc, argv));
        config.operations.insert(config.operations.begin(), make_shared<EXROperation_FixArnold>());
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

