#define TEST_X 304
#define TEST_Y 572

#include "DeepImageStroke.h"

#include <functional>
#include <vector>
#include <algorithm>
#include <math.h>

#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImathVec.h>
#include <OpenEXR/ImathMatrix.h>
#include <OpenEXR/ImfMatrixAttribute.h>

#include "EuclideanDistance.h"
#include "DeepImageUtil.h"
#include "SimpleImage.h"
#include "Helpers.h"

using namespace std;
using namespace Imf;
using namespace Imath;

float DeepImageStroke::DistanceAndRadiusToAlpha(float distance, const Config &config)
{
    // At 0, we're completely inside the shape.  Don't draw the stroke at all.
    if(distance <= 0.00001f)
        return 0;

    // Note that we don't fade the inside edge of the stroke.  That's handled by comping
    // the stroke underneath the shape, so the antialiasing of the shape blends on top
    // of the stroke.
    return scale_clamp(distance, config.radius, config.radius+config.fade, 1.0f, 0.0f);
}

void DeepImageStroke::ApplyStrokeUsingMask(const DeepImageStroke::Config &config, const SharedConfig &sharedConfig,
    shared_ptr<const DeepImage> image, shared_ptr<DeepImage> outputImage, shared_ptr<SimpleImage> mask)
{
    auto rgba = image->GetChannel<V4f>("rgba");
    auto id = image->GetChannel<uint32_t>(sharedConfig.idChannel);
    auto Z = image->GetChannel<float>("Z");

    // Find closest sample (for our object ID) to the camera for each point.
    Array2D<int> NearestSample;
    NearestSample.resizeErase(image->height, image->width);

    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            int &nearest = NearestSample[y][x];
            nearest = -1;

            for(int s = 0; s < image->NumSamples(x,y); ++s)
            {
                if(config.objectIds.find(id->Get(x,y,s)) == config.objectIds.end())
                    continue;

                if(nearest != -1)
                {
                    if(Z->Get(x,y,s) > Z->Get(x,y,nearest))
                        continue;
                }

                nearest = s;
            }
        }
    }

    // Calculate a stroke for the flattened image, and insert the stroke as deep samples, so
    // it'll get composited at the correct depth, allowing it to be obscured.
    Array2D<float> greyscale(mask->height, mask->width);
    for(int y = 0; y < mask->height; ++y)
    {
        for(int x = 0; x < mask->width; ++x)
        {
            float alpha = mask->GetRGBA(x, y)[3];
            alpha = ::clamp(alpha, 0.0f, 1.0f);
            if(alpha < 0.001f)
                alpha = 0;
            if(alpha >= 0.999f)
                alpha = 1;
            greyscale[y][x] = alpha;
        }
    }

    shared_ptr<Array2D<EuclideanDistance::DistanceResult>> EuclideanDistance = EuclideanDistance::Calculate(mask->width, mask->height, greyscale);

    for(int y = 0; y < mask->height; ++y)
    {
        for(int x = 0; x < mask->width; ++x)
        {
            float distance = (*EuclideanDistance)[y][x].distance;
            int sx = (*EuclideanDistance)[y][x].sx;
            int sy = (*EuclideanDistance)[y][x].sy;

            float alpha = DistanceAndRadiusToAlpha(distance + 0.5f, config);
            //if(x == TEST_X && y == TEST_Y)
            //    printf("-> distance %.13f, alpha %.13f, source %ix%i\n", distance, alpha, sx, sy);
#if 0
            image->AddSample(x, y);
            rgba->GetLast(x,y) = V4f(distance, distance, distance, 1);
            Z->GetLast(x,y) = 1;
            ZBack->GetLast(x,y) = 1;
            id->GetLast(x,y) = config.outputObjectId;
            continue;
#endif

            // Don't add an empty sample.
            if(alpha <= 0.00001f)
                continue;

            // sx/sy might be out of bounds.  This normally only happens if the layer is completely
            // empty and alpha will be 0 so we won't get here, but check to be safe.
            if(sx < 0 || sy < 0 || sx >= NearestSample.width() || sy >= NearestSample.height())
                continue;

            // SourceSample is the nearest visible pixel to this stroke, which we treat as the
            // "source" of the stroke.  StrokeSample is the sample underneath the stroke itself,
            // if any.
            int SourceSample = NearestSample[sy][sx];
            int StrokeSample = NearestSample[y][x];

            // For samples that lie outside the mask, StrokeSample.zNear won't be set, and we'll
            // use the Z distance from the source sample.  For samples that lie within the mask,
            // eg. because there's antialiasing, use whichever is nearer, the sample under the stroke
            // or the sample the stroke came from.  In this case, the sample under the stroke might
            // be closer to the camera than the source sample, so if we don't do this the stroke will
            // end up being behind the shape.
            //
            // Note that either of these may not actually have a sample, in which case the index will
            // be -1 and we'll use the default.
            float SourceSampleDistance = Z->GetWithDefault(sx, sy, SourceSample, 10000000);
            float StrokeSampleDistance = Z->GetWithDefault(x, y, StrokeSample, 10000000);
            float zDistance = min(SourceSampleDistance, StrokeSampleDistance);

            // Bias the distance closer to the camera.  We need to subtract at least a small amount to
            // make sure the stroke is on top of the source shape.  Subtracting more helps avoid aliasing
            // where two stroked objects are overlapping, but too much will cause strokes to be on top
            // of objects they shouldn't.
            zDistance -= config.pushTowardsCamera;
            // zDistance = 0;

            /*
             * An outer stroke is logically blended underneath the shape, and only antialiased on
             * the outer edge of the stroke.  The inner edge where the stroke meets the shape isn't
             * antialiased.  Instead, the antialiasing of the shape on top of it is what gives the
             * smooth blending from the stroke to the shape.  For intersection lines, the stroke
             * is between the source sample and samples below it.
             *
             * However, we want to put the stroke over the shape, not underneath it, so it can go over
             * other stroked objects.  Deal with this by mixing the existing color over the stroke color.
             */
            V4f topColor(0,0,0,0);
            for(int s = 0; s < image->NumSamples(x, y); ++s)
            {
                float depth = Z->Get(x,y,s);
                if(depth > SourceSampleDistance + 0.0001f + config.pushTowardsCamera)
                    continue;

                V4f c = rgba->Get(x,y,s);
                topColor = topColor*(1-c[3]);

                if(config.objectIds.find(id->Get(x,y,s)) != config.objectIds.end() ||
                    id->Get(x,y,s) == config.outputObjectId)
                    topColor += c;
            }

            // If the top color is completely opaque the stroke can't be seen at all, so
            // don't output a sample for it.
            if(topColor[3] >= 0.999f)
                continue;

            V4f strokeColor = config.strokeColor * alpha;
            V4f mixedColor = topColor + strokeColor * (1-topColor[3]);

            // Don't add an empty sample.
            if(mixedColor[3] <= 0.00001f)
                continue;

            // Add a sample for the stroke.
            outputImage->AddSample(x, y);

            auto rgbaOut = outputImage->GetChannel<V4f>("rgba");
            auto idOut = outputImage->GetChannel<uint32_t>(sharedConfig.idChannel);
            auto ZBackOut = outputImage->GetChannel<float>("ZBack");
            auto ZOut = outputImage->GetChannel<float>("Z");

            rgbaOut->GetLast(x,y) = mixedColor;
            ZOut->GetLast(x,y) = zDistance;
            ZBackOut->GetLast(x,y) = zDistance;
            idOut->GetLast(x,y) = config.outputObjectId;
        }
    }
}

// Return the number of pixels crossed when moving one pixel to the right, at a
// depth of 1.
static float CalculateDepthScale(const DeepImageStroke::Config &config, shared_ptr<const DeepImage> image)
{
    auto *worldToNDCAttr = image->header.findTypedAttribute<M44fAttribute>("worldToNDC");
    if(worldToNDCAttr == nullptr)
        throw exception("Can't create stroke intersections because worldToNDC matrix attribute is missing");

    auto *worldToCameraAttr = image->header.findTypedAttribute<M44fAttribute>("worldToCamera");
    if(worldToCameraAttr == nullptr)
        throw exception("Can't create stroke intersections because worldToNDC matrix attribute is missing");

    // Note that the OpenEXR ImfStandardAttributes.h header has a completely wrong
    // description of worldToNDC that could never work.  It's actually clip space,
    // with the origin in the center of the window, positive coordinates going
    // up-right, and requires perspective divide.
    M44f worldToNDC = worldToNDCAttr->value();
    M44f worldToCamera = worldToCameraAttr->value();
    M44f cameraToWorld = worldToCamera.inverse();

    // One point directly in front of the camera, and a second one unit up-right.
    V3f cameraSpaceReferencePos1(0,0,1);
    V3f cameraSpaceReferencePos2 = cameraSpaceReferencePos1 + V3f(1,1,0);

    // Convert to world space.
    V3f worldSpaceReferencePos1 = cameraSpaceReferencePos1 * cameraToWorld;
    V3f worldSpaceReferencePos2 = cameraSpaceReferencePos2 * cameraToWorld;

    // Convert from world space to NDC.
    V3f ndcReferencePos1, ndcReferencePos2;
    worldToNDC.multVecMatrix(worldSpaceReferencePos1, ndcReferencePos1);
    worldToNDC.multVecMatrix(worldSpaceReferencePos2, ndcReferencePos2);

    // Convert both positions to screen space.
    Box2i displayWindow = image->header.displayWindow();
    V2f screenSpace1(
        scale(ndcReferencePos1[0], -1.0f, +1.0f, float(displayWindow.min.x), float(displayWindow.max.x)),
        scale(ndcReferencePos1[1], -1.0f, +1.0f, float(displayWindow.max.y), float(displayWindow.min.y)));
    V2f screenSpace2(
        scale(ndcReferencePos2[0], -1.0f, +1.0f, float(displayWindow.min.x), float(displayWindow.max.x)),
        scale(ndcReferencePos2[1], -1.0f, +1.0f, float(displayWindow.max.y), float(displayWindow.min.y)));

    // The distance between these positions is the number of pixels one world space unit covers at
    // a distance of referenceDistance.
    V2f screenSpaceDistance = screenSpace2 - screenSpace1;

    /* printf("world1 %.1f %.1f %.1f\nworld2 %.1f %.1f %.1f\nndc1 %.3f %.3f %.3f\nndc2 %.3f %.3f %.3f\ndistance %f %f\n",
        worldSpaceReferencePos1.x, worldSpaceReferencePos1.y, worldSpaceReferencePos1.z,
        worldSpaceReferencePos2.x, worldSpaceReferencePos2.y, worldSpaceReferencePos2.z,
        ndcReferencePos1[0], ndcReferencePos1[1], ndcReferencePos1[2],
        ndcReferencePos2[0], ndcReferencePos2[1], ndcReferencePos2[2],
        screenSpaceDistance[0], screenSpaceDistance[1]
    ); */

    // Return the distance on X covered by one unit in camera space.
    return screenSpaceDistance[0];
}

// Create an intersection pattern that can be used to create a stroke.  This generates a mask
// which is set for pixels that neighbor pixels further away.  What we're really looking
// for is mesh discontinuities: neighboring pixels which are from two different places
// and not a continuous object.
//
// If imageMask is non-null, it's a mask to apply to the layer we're creating a mask for.
//
// Note that to make comments easier to follow, this pretends world space units are in cm,
// like Maya.  "1cm" really just means one world space unit.
shared_ptr<SimpleImage> DeepImageStroke::CreateIntersectionPattern(
    const DeepImageStroke::Config &config,
    const SharedConfig &sharedConfig,
    shared_ptr<const DeepImage> image,
    shared_ptr<const TypedDeepImageChannel<float>> strokeMask,
    shared_ptr<const TypedDeepImageChannel<float>> intersectionMask)
{
    shared_ptr<SimpleImage> pattern = make_shared<SimpleImage>(image->width, image->height);

    // Create a mask using simple edge detection.
    auto id = image->GetChannel<uint32_t>(sharedConfig.idChannel);
    auto Z = image->GetChannel<float>("Z");
    auto A = image->GetAlphaChannel();

    // P and/or N will be NULL if intersectionsUseDistance or intersectionsUseNormals are false.
    auto P = image->GetChannel<V3f>("P");
    auto N = image->GetChannel<V3f>("N");

    if(config.intersectionsUseDistance && P == nullptr)
    {
        printf("Warning: No P layer is present, so stroke intersections can only use normals.  If this is\n");
        printf("what you meant, the --intersection-ignore-distance argument will suppress this message.\n");
    }

    if(config.intersectionsUseNormals && N == nullptr)
    {
        printf("Warning: No N channel is present, so stroke intersections can only use positions.  If this is\n");
        printf("what you meant, the --intersection-ignore-normals argument will suppress this message.\n");
    }

    if(P == nullptr && N == nullptr)
    {
        printf("Error: No P or N channel is active, so stroke intersections can't be created.\n");
        return nullptr;
    }

    Array2D<vector<float>> SampleVisibilities;
    DeepImageUtil::GetSampleVisibilities(image, SampleVisibilities);

    // The number of pixels per 1cm, at a distance of 1cm from the camera.
    float pixelsPerCm = CalculateDepthScale(config, image);

    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            if(!image->NumSamples(x,y))
                continue;

            float maxDistance = 0;

            static const vector<pair<int,int>> directions = {
                {  0, -1 },
                { -1,  0 },
                { +1,  0 },
                {  0, +1 },

                // We can test against diagonals, and again other samples in the same
                // pixel, but this generally doesn't seem to make much difference.
#if 0
                { -1, -1 },
                { +1, -1 },
                { -1, +1 },
                { +1, +1 },
                {  0,  0 },
#endif
            };

            // Compare this pixel to each of the bordering pixels.
            for(const auto &dir: directions)
            {
                int x2 = x + dir.first;
                int y2 = y + dir.second;
                if(x2 < 0 || y2 < 0 || x2 >= image->width || y2 >= image->height)
                    continue;

                // Compare the depth of each sample in (x,y) to each sample in (x2,y2).
                float totalDifference = 0;
                for(int s1 = 0; s1 < image->NumSamples(x,y); ++s1)
                {
                    if(config.objectIds.find(id->Get(x,y,s1)) == config.objectIds.end())
                        continue;

                    // Skip this sample if it's completely occluded.
                    float sampleVisibility1 = SampleVisibilities[y][x][s1] * A->Get(x,y,s1);
                    if(sampleVisibility1 < 0.001f)
                        continue;

                    float depth1 = Z->Get(x, y, s1);
                    V3f world1 = P? P->Get(x, y, s1):V3f(0,0,0);
                    V3f normal1 = N? N->Get(x, y, s1).normalized():V3f(1,0,0);

                    // We're looking for sudden changes in depth from one pixel to the next to find
                    // edges.  However, we need to adjust the threshold based on pixel density.  If
                    // we're twice as far from the camera, we'll have half as many pixels, which makes
                    // changes in depth look twice as sudden.  If we don't have enough pixels to
                    // sample, any two neighboring pixels might look far apart.
                    //
                    // config.minPixelsPerCm is the minimum number of pixels that we're allowed to cross in
                    // 1cm of world space.  If we're crossing less than that, the object is far away or the
                    // image is low resolution, and we'll begin scaling intersectionMinDistance up, so it takes
                    // a bigger distance before we detect an edge.

                    // pixelsPerCm is at a depth of 1.  pixelsPerCm / depth is the number of pixels at depth.
                    float pixelsPerCmAtThisDepth = pixelsPerCm / depth1;

                    // If pixelsPerCmAtThisDepth >= minPixelsPerCm, then we have enough pixels and don't
                    // need to scale, so depthScale is 1.
                    //
                    // If pixelsPerCmAtThisDepth is half minPixelsPerCm, then we're crossing half as many
                    // pixels per cm as minPixelsPerCm.  depthScale is 2, so we'll double the threshold.
                    float depthScale = max(1.0f, config.minPixelsPerCm / pixelsPerCmAtThisDepth);

                    /*if(x == TEST_X && y == TEST_Y)
                    {
                        printf("%ix%i depth %f, pixelsPerCmAtThisDepth %f, depthScale %f\n",
                            x, y, depth1, pixelsPerCmAtThisDepth, depthScale);
                    }*/

                    // config.intersectionMinDistance is the distance between pixels where we start to
                    // add intersection lines, assuming the number of units per pixel is expectedPixelsPerCm.

                    for(int s2 = 0; s2 < image->NumSamples(x2,y2); ++s2)
                    {
                        if(config.objectIds.find(id->Get(x2,y2,s2)) == config.objectIds.end())
                            continue;

                        // Skip this sample if it's completely occluded.
                        float sampleVisibility2 = SampleVisibilities[y2][x2][s2] * A->Get(x2,y2,s2);
                        if(sampleVisibility2 < 0.001f)
                            continue;

                        // Don't clear this pixel if it's further away than the source, so we clear
                        // pixels within the nearer object and not the farther one.
                        float depth2 = Z->Get(x2, y2, s2);
                        if(depth2 < depth1)
                            continue;

                        V3f world2 = P? P->Get(x2, y2, s2):V3f(0,0,0);
                        V3f normal2 = N? N->Get(x2, y2, s2).normalized():V3f(1,0,0);
                        float angle = acosf(normal1.dot(normal2)) * 180 / float(M_PI);

                        // Find the world space distance between these two samples.
                        float distance = (world2 - world1).length();

                        /* if(x == TEST_X && y == TEST_Y)
                        {
                            printf("distance (%+ix%+i) between %ix%i sample %i (depth %.1f, vis %.2f) and %ix%i sample %i (vis %.2f): depth %.1f, distance %f\n",
                                dir.first, dir.second,
                                x, y, s1, depth1, sampleVisibility1,
                                x2, y2, s2, sampleVisibility2,
                                depth2-depth1, distance);
                        } */

                        // Scale depth and normals to 0-1.
                        float result = 1;
                        if(config.intersectionsUseNormals && N)
                            result *= scale_clamp(angle,
                                config.intersectionAngleThreshold,
                                config.intersectionAngleThreshold + config.intersectionAngleFade,
                                0.0f, 1.0f);
                        if(config.intersectionsUseDistance && P)
                            result *= scale_clamp(distance,
                                 config.intersectionMinDistance*depthScale,
                                (config.intersectionMinDistance+config.intersectionFade) * depthScale, 0.0f, 1.0f);

                        // Scale by the visibility of the pixels we're testing.
                        result *= sampleVisibility1 * sampleVisibility2;

                        // If we have a mask, apply it now like visibility.
                        //
                        // If the object ID is the same then this is an object crossing over itself, so
                        // use the intersection mask.  If the ID is different then it's one object on top
                        // of another, so use the stroke mask.
                        shared_ptr<const TypedDeepImageChannel<float>> &mask =
                            id->Get(x,y,s1) == id->Get(x2,y2,s2)? intersectionMask:strokeMask;
                        if(mask)
                            result *= ::clamp(mask->Get(x,y,s1), 0.0f, 1.0f);

                        totalDifference += result;
                    }
                }

                // If this is a corner sample, reduce its effect based on the distance to the
                // pixel we're testing.
                float screenDistance = (V2f((float) x, (float) y) - V2f((float) x2, (float) y2)).length();
                if(screenDistance >= 1)
                    totalDifference *= 1/screenDistance;

                maxDistance = max(maxDistance, totalDifference);
            }

            pattern->GetRGBA(x,y) = V4f(1,1,1,1) * maxDistance;
        }
    }

    return pattern;
}

void EXROperation_Stroke::Run(shared_ptr<EXROperationState> state) const
{
    // Output stroke samples to an output image that we'll combine later, and not
    // directly into the image.  If multiple strokes are added, we don't want later
    // strokes to be affected by the strokes of earlier images.
    AddStroke(strokeDesc, state->image, state->GetOutputImage());
}

void EXROperation_Stroke::AddStroke(const DeepImageStroke::Config &config, shared_ptr<const DeepImage> image, shared_ptr<DeepImage> outputImage) const
{
    // The user masks that control where we apply strokes and intersection lines:
    shared_ptr<const TypedDeepImageChannel<float>> strokeVisibilityMask;
    if(!config.strokeMaskChannel.empty())
        strokeVisibilityMask = image->GetChannel<float>(config.strokeMaskChannel);

    shared_ptr<const TypedDeepImageChannel<float>> intersectionVisibilityMask;
    if(!config.intersectionMaskChannel.empty())
        intersectionVisibilityMask = image->GetChannel<float>(config.intersectionMaskChannel);

    // Flatten the image.  We'll use this as the mask to create the stroke.  Don't
    // actually apply the stroke until we deal with intersections, so we don't apply
    // intersection strokes to other strokes.
    shared_ptr<SimpleImage> strokeMask;
    if(config.strokeOutline)
        strokeMask = DeepImageUtil::CollapseEXR(image,
            image->GetChannel<uint32_t>(sharedConfig.idChannel),
            image->GetChannel<V4f>("rgba"),
            strokeVisibilityMask, config.objectIds,
            DeepImageUtil::CollapseMode_Visibility);

    // Create the intersection mask.  It's important that we do this before applying the stroke.
    shared_ptr<SimpleImage> intersectionPattern;
    if(config.strokeIntersections)
    {
        intersectionPattern = CreateIntersectionPattern(config, sharedConfig, image, strokeVisibilityMask, intersectionVisibilityMask);

        // This is just for diagnostics.
        if(intersectionPattern && !config.saveIntersectionPattern.empty())
            SimpleImage::WriteEXR(config.saveIntersectionPattern, { SimpleImage::EXRLayersToWrite(intersectionPattern) });
    }

    // Apply the regular stroke and the intersection stroke.
    if(config.strokeOutline)
        ApplyStrokeUsingMask(config, sharedConfig, image, outputImage, strokeMask);
    if(config.strokeIntersections && intersectionPattern)
        ApplyStrokeUsingMask(config, sharedConfig, image, outputImage, intersectionPattern);

    // Make sure the output image is sorted.
    DeepImageUtil::SortSamplesByDepth(outputImage);
}

static V4f ParseColor(const string &str)
{
    int ir=255, ib=255, ig=255, ia=255;
    int result = sscanf( str.c_str(), "#%2x%2x%2x%2x", &ir, &ig, &ib, &ia );
    if(result < 3)
        return V4f(1,1,1,1);

    V4f rgba;
    rgba[0] = (float) ir; rgba[1] = (float) ig; rgba[2] = (float) ib;
    if( result == 4 )
        rgba[3] = (float) ia;
    else
        rgba[3] = 255;
    rgba /= 255;
    return rgba;
}

// --stroke=1000
EXROperation_Stroke::EXROperation_Stroke(const SharedConfig &sharedConfig_, string opt, vector<pair<string,string>> arguments):
    sharedConfig(sharedConfig_)
{
    // Adjust worldSpaceScale to world space units.  This only affects defaults, not what the user specifies directly.
    strokeDesc.minPixelsPerCm *= sharedConfig.worldSpaceScale;
    strokeDesc.intersectionMinDistance *= sharedConfig.worldSpaceScale;
    strokeDesc.intersectionFade *= sharedConfig.worldSpaceScale;
    strokeDesc.pushTowardsCamera *= sharedConfig.worldSpaceScale;

    vector<string> ids;
    split(opt, ",", ids);
    for(string id: ids)
        strokeDesc.objectIds.insert(atoi(id.c_str()));
    strokeDesc.outputObjectId = atoi(ids[0].c_str());

    for(auto it: arguments)
    {
        string arg = it.first;
        string value = it.second;
        if(arg == "output-id")
            strokeDesc.outputObjectId = atoi(value.c_str());
        else if(arg == "radius")
            strokeDesc.radius = (float) atof(value.c_str());
        else if(arg == "fade")
            strokeDesc.fade = (float) atof(value.c_str());
        else if(arg == "color")
            strokeDesc.strokeColor = ParseColor(value);
        else if(arg == "stroke-mask")
            strokeDesc.strokeMaskChannel = value;
        else if(arg == "intersection-mask")
            strokeDesc.intersectionMaskChannel = value;
        else if(arg == "intersections-only")
        {
            strokeDesc.strokeIntersections = true;
            strokeDesc.strokeOutline = false;
        }
        else if(arg == "intersections")
            strokeDesc.strokeIntersections = true;
        else if(arg == "intersection-min-distance")
            strokeDesc.intersectionMinDistance = (float) atof(value.c_str());
        else if(arg == "intersection-fade")
            strokeDesc.intersectionFade = (float) atof(value.c_str());
        else if(arg == "intersection-min-angle")
            strokeDesc.intersectionAngleThreshold = (float) atof(value.c_str());
        else if(arg == "intersection-angle-fade")
            strokeDesc.intersectionAngleFade = (float) atof(value.c_str());
        else if(arg == "intersection-save-pattern")
            strokeDesc.saveIntersectionPattern = sharedConfig.GetFilename(value);
        else if(arg == "intersection-ignore-distance")
            strokeDesc.intersectionsUseDistance = false;
        else if(arg == "intersection-ignore-normals")
            strokeDesc.intersectionsUseNormals = false;
        else
            throw StringException("Unknown stroke option: " + arg);
    }

    // Make sure at least one of these is on.
    if(!strokeDesc.intersectionsUseDistance && !strokeDesc.intersectionsUseNormals)
        throw StringException("Intersections can't ignore both distance and normals");
}

void EXROperation_Stroke::AddChannels(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const
{
    image->AddChannelToFramebuffer<uint32_t>(sharedConfig.idChannel, frameBuffer, false);

    if(strokeDesc.strokeIntersections)
    {
        if(strokeDesc.intersectionsUseDistance)
            image->AddChannelToFramebuffer<V3f>("P", frameBuffer, false);
        if(strokeDesc.intersectionsUseNormals)
            image->AddChannelToFramebuffer<V3f>("N", frameBuffer, false);
    }
    if(!strokeDesc.strokeMaskChannel.empty())
        image->AddChannelToFramebuffer<float>(strokeDesc.strokeMaskChannel, frameBuffer, true);
    if(!strokeDesc.intersectionMaskChannel.empty())
        image->AddChannelToFramebuffer<float>(strokeDesc.intersectionMaskChannel, frameBuffer, true);
}


/*
 * Based on http://weber.itn.liu.se/~stegu/aadist/
 *
 * Copyright (C) 2009-2012 Stefan Gustavson (stefan.gustavson@gmail.com)
 *
 * This software is distributed under the permissive "MIT License":
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
