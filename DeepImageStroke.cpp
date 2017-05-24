#include "DeepImageStroke.h"

#include <functional>
#include <vector>
#include <algorithm>

#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImathVec.h>

using namespace std;
using namespace Imf;
using namespace Imath;

#include "DeepImageUtil.h"
#include "SimpleImage.h"

namespace EuclideanMetric
{
    inline float f(float x_i, float gi)
    {
	return (x_i*x_i)+(gi*gi);
    }

    inline float sep(float i, float u, float gi, float gu)
    {
	return (u*u - i*i + gu*gu - gi*gi) / (2*(u-i));
    }
};

void DeepImageStroke::CalculateDistance(
    int width, int height,
    function<float(int x, int y)> GetMask,
    function<void(int x, int y, int sx, int sy, float distance)> PutResult)
{
    // g[y][x] is the number of pixels away the nearest "inside" value is on the Y
    // axis.  If it's upwards (move by -g[y][x]), g_up[y][x] is true, otherwise false.
    Array2D<float> g(height, width);
    Array2D<bool> g_up(height, width);

    const float inf = float(width + height);
    for(int x = 0; x < width; ++x)
	for(int y = 0; y < height; ++y)
	    g_up[y][x] = true;

    // phase 1: find the distance to the closest pixel vertically
    for(int x = 0; x < width; ++x)
    {
	{
	    const int y = 0;
	    const int idx = x+y*width;
	    g[y][x] = GetMask(x, y);
	    if(g[y][x] <= 0.0001f)       g[y][x] = 0;
	    else if(g[y][x] >= 0.9999f)  g[y][x] = inf; // no pixel above
	}
	// scan 1
	for(int y = 1; y < height; ++y)
	{
	    g[y][x] = GetMask(x, y);
	    if(g[y][x] <= 0.0001f)       g[y][x] = 0;
	    else if(g[y][x] >= 0.9999f)  g[y][x] = g[y-1][x] + 1; // 1 + g for the pixel above
	}

	// scan 2
	for(int y = height-2; y >= 0; --y)
	{
	    const float d = g[y+1][x] + 1; // 1 + g for the pixel below
	    if(d < g[y][x])
	    {
		g[y][x] = d;
		g_up[y][x] = false;
	    }
	    //g[y][x] = min(g[y][x], d);
	}
    }
    /*
    for(int y = 0; y < min(height, 10); ++y)
    {
	for(int x = 0; x < min(width, 10); ++x)
	    printf("%.0f ", GetMask(x, y));
	printf("\n");
    }

    for(int y = 0; y < min(height, 10); ++y)
    {
	for(int x = 0; x < min(width, 10); ++x)
	    printf("%3.0f ", g[y][x] * (g_up[y][x]?-1:+1));
	printf("\n");
    }
    */
    // phase 2
    for(int y = 0; y < height; ++y)
    {
	vector<int> s(max(width, height));
	vector<float> t(max(width, height)); // scaled

	int q = 0;
	s[0] = 0;
	t[0] = 0;

	// scan 3
	for(int x = 1; x < width; ++x)
	{
	    while(q >= 0 &&
		EuclideanMetric::f(floorf(t[q]) - s[q], g[y][s[q]]) >
		EuclideanMetric::f(floorf(t[q]) -    x, g[y][   x]))
	    {
		q--;
	    }

	    if(q < 0)
	    {
		q = 0;
		s[q] = x;
		continue;
	    }

	    const float w = 1 + EuclideanMetric::sep((float) s[q], (float) x, g[y][s[q]], g[y][x]);
	    if(w < width)
	    {
		++q;
		s[q] = x;
		t[q] = w;
	    }
	}

	// scan 4
	for(int x = width-1; x >= 0; --x)
	{
	    int read_x = s[q];

	    // sx and sy are the distance on X and Y:
	    float sx = float(x-read_x);
	    float sy = g[y][read_x];
	    const float distanceSquared = EuclideanMetric::f(sx, sy);
	    float distance = sqrtf(distanceSquared);

	    // These are the X and Y coordinates of the nearest pixel:
	    int src_x = s[q];
	    int src_y = y + int(floor(g[y][read_x]) * (g_up[y][read_x]?-1:+1));

	    PutResult(x, y, src_x, src_y, distance);
	    if(x == floorf(t[q]))
		--q;
	}
    }
}

float DeepImageStroke::DistanceAndRadiusToAlpha(float distance, float radius)
{
    // At 0, we're completely inside the shape.  Don't draw the stroke at all.
    if(distance <= 0)
	return 0;

    // We're fully visible up to the radius.  Note that we don't fade the inside edge
    // of the stroke.  That's handled by comping the stroke underneath the shape, so
    // the antialiasing of the shape blends on top of the stroke.
    if(distance < radius)
	return 1;

    // Fade off for one pixel.  XXX: make this configurable
    if(distance < radius+1)
    {
	float outsideFade = distance - radius;
	return 1 - outsideFade;
    }

    // We're completely outside the stroke.
    return 0;
}

void DeepImageStroke::AddStroke(const DeepImageStroke::Config &config, shared_ptr<DeepImage> image)
{
    // Flatten the image.  We'll use this as the mask to create the stroke.
    shared_ptr<SimpleImage> mask = DeepImageUtil::CollapseEXR(image, { config.objectId });

    // Find closest sample (for our object ID) to the camera for each point.
    Array2D<int> NearestSample;
    NearestSample.resizeErase(image->height, image->width);

    auto rgba = image->GetChannel<V4f>("rgba");
    auto id = image->GetChannel<uint32_t>("id");
    auto ZBack = image->GetChannel<float>("ZBack");
    auto Z = image->GetChannel<float>("Z");

    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
	    int &nearest = NearestSample[y][x];
	    nearest = -1;

	    for(int s = 0; s < image->NumSamples(x,y); ++s)
            {
		if(id->Get(x,y,s) != config.objectId)
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
    CalculateDistance(mask->width, mask->height,
    [&](int x, int y) {
	float result = mask->GetRGBA(x, y)[3];
	result = max(0.0f, result);
	result = min(1.0f, result);

	// Skip this line for an inner stroke instead of an outer stroke:
	result = 1.0f - result;
	
	return result;
    }, [&](int x, int y, int sx, int sy, float distance) {
	float alpha = DistanceAndRadiusToAlpha(distance, config.radius);

	// Don't add an empty sample.
	if(alpha <= 0.00001f)
	    return;

	// sx/sy might be out of bounds.  This normally only happens if the layer is completely
	// empty and alpha will be 0 so we won't get here, but check to be safe.
	if(sx < 0 || sy < 0 || sx >= NearestSample.width() || sy >= NearestSample.height())
	    return;

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
	float zDistance = min(Z->GetWithDefault(sx, sy, SourceSample, 10000000),
	                      Z->GetWithDefault(x, y, StrokeSample, 10000000));

	// Bias the distance closer to the camera.  We need to subtract at least a small amount to
	// make sure the stroke is on top of the source shape.  Subtracting more helps avoid aliasing
	// where two stroked objects are overlapping, but too much will cause strokes to be on top
	// of objects they shouldn't.
	zDistance -= config.pushTowardsCamera;
	// zDistance = 0;

	/*
	 * An outer stroke is normally blended underneath the shape, and only antialiased on
	 * the outer edge of the stroke.  The inner edge where the stroke meets the shape isn't
	 * antialiased.  Instead, the antialiasing of the shape on top of it is what gives the
	 * smooth blending from the stroke to the shape.
	 *
	 * However, we want to put the stroke over the shape, not underneath it, so it can go over
	 * other stroked objects.  Deal with this by mixing the existing color over the stroke color.
	 */
	V4f strokeColor = config.strokeColor * alpha;
	V4f topColor = mask->GetRGBA(x, y);
	V4f mixedColor = topColor + strokeColor * (1-topColor[3]);

	// Don't add an empty sample.
	if(mixedColor[3] <= 0.00001f)
	    return;

	// Add a sample for the stroke.
	image->AddSample(x, y);

	rgba->GetLast(x,y) = mixedColor;
	Z->GetLast(x,y) = zDistance;
	ZBack->GetLast(x,y) = zDistance;
	id->GetLast(x,y) = config.objectId;
    });
}

/*
 * Based on https://github.com/vinniefalco/LayerEffects/blo
 *
 * Copyright (c) 2012 Vinnie Falco
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
