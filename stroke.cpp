#include "stroke.h"

#include <functional>
#include <vector>
#include <algorithm>

#include <OpenEXR/ImfArray.h>

using namespace std;
using namespace Imf;

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

void Stroke::CalculateDistance(
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

float Stroke::DistanceAndRadiusToAlpha(float distance, float radius)
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
