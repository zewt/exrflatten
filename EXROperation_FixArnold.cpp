#include "EXROperation_FixArnold.h"
#include "DeepImage.h"
#include "DeepImageUtil.h"
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfMatrixAttribute.h>

using namespace std;
using namespace Imf;
using namespace Imath;

bool EXROperation_FixArnold::IsArnold(shared_ptr<DeepImage> image) const
{
    return image->header.findTypedAttribute<StringAttribute>("arnold/version") != NULL;
};

void EXROperation_FixArnold::AddChannels(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const
{
    // Don't add P here.  We only want to fix P if it's added by another operation,
    // not add it and fix it if nobody needs it.
}

void EXROperation_FixArnold::Run(shared_ptr<EXROperationState> state) const
{
    shared_ptr<DeepImage> image = state->image;
    if(!IsArnold(image))
        return;

    // If there's no P channel, we don't need to do this.
    auto P = image->GetChannel<V3f>("P");
    if(P == nullptr)
        return;

    auto *worldToNDCAttr = image->header.findTypedAttribute<M44fAttribute>("worldToNDC");
    if(worldToNDCAttr == nullptr)
        throw exception("Can't work around Arnold problems because the worldToNDC matrix attribute is missing");

    auto *worldToCameraAttr = image->header.findTypedAttribute<M44fAttribute>("worldToCamera");
    if(worldToCameraAttr == nullptr)
        throw exception("Can't work around Arnold problems because the worldToNDC matrix attribute is missing");

    M44f worldToNDC = worldToNDCAttr->value();

    Box2i displayWindow = image->header.displayWindow();
    auto convertWorldToScreen = [&worldToNDC, &displayWindow](V3f world)
    {
        V3f ndc;
        worldToNDC.multVecMatrix(world, ndc);

        V2f screenSpace(
            scale(ndc[0], -1.0f, +1.0f, float(displayWindow.min.x), float(displayWindow.max.x)),
            scale(ndc[1], -1.0f, +1.0f, float(displayWindow.max.y), float(displayWindow.min.y)));

        return screenSpace;
    };

    auto A = image->GetAlphaChannel();

    float errorCountDirect = 0;
    float errorCountUnpremultiplied = 0;
    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            for(int s = 0; s < image->NumSamples(x,y); ++s)
            {
                float alpha = A->Get(x,y,s);
                V3f world = P->Get(x,y,s);
                V3f worldUnpremultiplied = world / alpha;
                    
                V2f expectedPos((float) x, (float) y);
                float error = (convertWorldToScreen(world) - expectedPos).length();
                float errorUnpremultiplied = (convertWorldToScreen(worldUnpremultiplied) - expectedPos).length();

                // Ignore world space positions at the origin.  This is what Arnold outputs for
                // the background when it's opaque.
                if(world.length() < 0.01f)
                    continue;
                    //printf("%f %f %f %f\n", alpha, world[0], world[1], world[2]);
                errorCountDirect += error;
                errorCountUnpremultiplied += errorUnpremultiplied;
            }
        }
    }

    bool isMultipliedByAlpha = false;
    if(errorCountDirect >= errorCountUnpremultiplied*10)
    {
        // We have much less position error when dividing by alpha than without, so it looks
        // like this image is multiplied by alpha.
        isMultipliedByAlpha = true;
        printf("Working around corrupted Arnold positional data\n");
    }
    else if(errorCountUnpremultiplied >= errorCountDirect*10)
    {
        // If the above didn't happen, then we should have much less error without dividing by
        // alpha.
        //return;
    }
    else
    {
        // If we get here, we have similar amounts of error in both, which means something unexpected
        // is happening.
        printf("Warning: can't determine whether we have bad Arnold data or not (%f, %f)\n",
            errorCountDirect, errorCountUnpremultiplied);
        return;
    }

    // Unpremultiply P.
    P->UnpremultiplyChannel(image->GetAlphaChannel());
}
