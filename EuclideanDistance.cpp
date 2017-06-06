#include "EuclideanDistance.h"

#include <math.h>
#include <vector>

using namespace std;
using namespace Imf;

/*
 * edtaa3()
 *
 * Sweep-and-update Euclidean distance transform of an
 * image. Positive pixels are treated as object pixels,
 * zero or negative pixels are treated as background.
 * An attempt is made to treat antialiased edges correctly.
 * The input image must have pixels in the range [0,1],
 * and the antialiased image should be a box-filter
 * sampling of the ideal, crisp edge.
 * If the antialias region is more than 1 pixel wide,
 * the result from this transform will be inaccurate.
 *
 * By Stefan Gustavson (stefan.gustavson@gmail.com).
 *
 * Originally written in 1994, based on a verbal
 * description of the SSED8 algorithm published in the
 * PhD dissertation of Ingemar Ragnemalm. This is his
 * algorithm, I only implemented it in C.
 *
 * Updated in 2004 to treat border pixels correctly,
 * and cleaned up the code to improve readability.
 *
 * Updated in 2009 to handle anti-aliased edges.
 *
 * Updated in 2011 to avoid a corner case infinite loop.
 *
 * Updated 2012 to change license from LGPL to MIT.
 *
 * Updated 2014 to fix a bug in the gradient computations. Ahem.
 *
 */


/*
 * Compute the local gradient at edge pixels using convolution filters.
 * The gradient is computed only at edge pixels. At other places in the
 * image, it is never used, and it's mostly zero anyway.
 */
static void computegradient(const Array2D<float> &mask,
           int w, int h, float *gx, float *gy)
{
    int i,j,k;
    float glength;
#define SQRT2 1.4142136f
    for(j = 1; j < h-1; j++) {
	for(i = 1; i < w-1; i++) { // Avoid edges where the kernels would spill over
            k = j*w + i;
            if((mask[j][i] > 0.0) && (mask[j][i]<1.0)) { // Compute gradient for edge pixels only
                gx[k] =
                    - mask[j-1][i-1]
                    - SQRT2*
                      mask[j][i-1]
                    - mask[j+1][i-1]
                    + mask[j-1][i+1]
                    + SQRT2*
                      mask[j][i+1]
                    + mask[j+1][i+1];
                gy[k] =
                    - mask[j-1][i-1]
                    - SQRT2*
                      mask[j-1][i]
                    - mask[j-1][i+1]
                    + mask[j+1][i-1]
                    + SQRT2*
                      mask[j+1][i]
                    + mask[j+1][i+1];
                glength = gx[k]*gx[k] + gy[k]*gy[k];
                if(glength > 0.0) { // Avoid division by zero
                    glength = sqrt(glength);
                    gx[k]=gx[k]/glength;
                    gy[k]=gy[k]/glength;
                }
            }
        }
    }
    // TODO: Compute reasonable values for gx, gy also around the image edges.
    // (These are zero now, which reduces the accuracy for a 1-pixel wide region
	// around the image edge.) 2x2 kernels would be suitable for this.
}

/*
 * A somewhat tricky function to approximate the distance to an edge in a
 * certain pixel, with consideration to either the local gradient (gx,gy)
 * or the direction to the pixel (dx,dy) and the pixel greyscale value a.
 * The latter alternative, using (dx,dy), is the metric used by edtaa2().
 * Using a local estimate of the edge gradient (gx,gy) yields much better
 * accuracy at and near edges, and reduces the error even at distant pixels
 * provided that the gradient direction is accurately estimated.
 */
static float edgedf(float gx, float gy, float a)
{
    if (gx == 0 || gy == 0) // Either A) gu or gv are zero, or B) both
        return 0.5f-a;  // Linear approximation is A) correct or B) a fair guess

    float glength = sqrtf(gx*gx + gy*gy);
    if(glength>0) {
        gx = gx/glength;
        gy = gy/glength;
    }

    /* Everything is symmetric wrt sign and transposition,
     * so move to first octant (gx>=0, gy>=0, gx>=gy) to
     * avoid handling all possible edge directions. */
    gx = fabsf(gx);
    gy = fabsf(gy);
    if(gx<gy)
        swap(gx, gy);

    float a1 = 0.5f*gy/gx;

    if (a < a1) { // 0 <= a < a1
        return 0.5f*(gx + gy) - sqrt(2.0f*gx*gy*a);
    } else if (a < (1.0-a1)) { // a1 <= a <= 1-a1
        return (0.5f-a)*gx;
    } else { // 1-a1 < a <= 1
        return -0.5f*(gx + gy) + sqrt(2.0f*gx*gy*(1.0f-a));
    }
}

static float distaa3(
	const Array2D<float> &mask,
        const float *gximg, const float *gyimg, int w, int c, int xc, int yc, int xi, int yi)
{
  float di, df, dx, dy, gx, gy, a;
  
  int closest = c-xc-yc*w; // Index to the edge pixel pointed to from c

  int closestX = closest % w; // Index to the edge pixel pointed to from c
  int closestY = closest / w; // Index to the edge pixel pointed to from c
  a = mask[closestY][closestX];    // Grayscale value at the edge pixel
  gx = gximg[closest]; // X gradient component at the edge pixel
  gy = gyimg[closest]; // Y gradient component at the edge pixel
  
  if(a > 1.0) a = 1.0;
  if(a < 0.0) a = 0.0; // Clip grayscale values outside the range [0,1]
  if(a == 0.0) return 1000000.0; // Not an object pixel, return "very far" ("don't know yet")

  dx = (float)xi;
  dy = (float)yi;
  di = sqrt(dx*dx + dy*dy); // Length of integer vector, like a traditional EDT
  if(di==0) { // Use local gradient only at edges
      // Estimate based on local gradient only
      df = edgedf(gx, gy, a);
  } else {
      // Estimate gradient based on direction to edge (accurate for large di)
      df = edgedf(dx, dy, a);
  }
  return di + df; // Same metric as edtaa2, except at edges (where di=0)
}

// Shorthand macro: add ubiquitous parameters dist, gx, gy, img and w and call distaa3()
#define DISTAA(c,xc,yc,xi,yi) (distaa3(mask, gx, gy, w, c, xc, yc, xi, yi))

static void edtaa3(
    const Array2D<float> &mask,
    const float *gx, const float *gy, int w, int h, short *distx, short *disty, float *dist)
{
    int x, y, i, c;
    int offset_u, offset_ur, offset_r, offset_rd,
        offset_d, offset_dl, offset_l, offset_lu;
    float olddist, newdist;
    int cdistx, cdisty, newdistx, newdisty;
    int changed;
    float epsilon = 1e-3f;

    /* Initialize index offsets for the current image width */
    offset_u = -w;
    offset_ur = -w+1;
    offset_r = 1;
    offset_rd = w+1;
    offset_d = w;
    offset_dl = w-1;
    offset_l = -1;
    offset_lu = -w-1;

    /* Initialize the distance images */
    for(x = 0; x < w; ++x) {
        for(y = 0; y < h; ++y) {
            i = y*w + x;
            distx[i] = 0; // At first, all pixels point to
            disty[i] = 0; // themselves as the closest known.
            float value = mask[y][x];
            if(value <= 0.0)
            {
                dist[i]= 1000000.0; // Big value, means "not set yet"
            }
            else { // if (value < 1.0) {
                dist[i] = edgedf(gx[i], gy[i], value); // Gradient-assisted estimate
            }
            //else {
                //dist[i]= 0.0; // Inside the object
            //}
        }
    }

    /* Perform the transformation */
    do
    {
        changed = 0;

        /* Scan rows, except first row */
        for(y=1; y<h; y++)
        {

            /* move index to leftmost pixel of current row */
            i = y*w;

            /* scan right, propagate distances from above & left */

            /* Leftmost pixel is special, has no left neighbors */
            olddist = dist[i];
            if(olddist > 0) // If non-zero distance or not set yet
            {
                c = i + offset_u; // Index of candidate for testing
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx;
                newdisty = cdisty+1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_ur;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx-1;
                newdisty = cdisty+1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    changed = 1;
                }
            }
            i++;

            /* Middle pixels have all neighbors */
            for(x=1; x<w-1; x++, i++)
            {
                olddist = dist[i];
                if(olddist <= 0) continue; // No need to update further

                c = i+offset_l;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx+1;
                newdisty = cdisty;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_lu;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx+1;
                newdisty = cdisty+1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_u;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx;
                newdisty = cdisty+1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_ur;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx-1;
                newdisty = cdisty+1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    changed = 1;
                }
            }

            /* Rightmost pixel of row is special, has no right neighbors */
            olddist = dist[i];
            if(olddist > 0) // If not already zero distance
            {
                c = i+offset_l;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx+1;
                newdisty = cdisty;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_lu;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx+1;
                newdisty = cdisty+1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_u;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx;
                newdisty = cdisty+1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    changed = 1;
                }
            }

            /* Move index to second rightmost pixel of current row. */
            /* Rightmost pixel is skipped, it has no right neighbor. */
            i = y*w + w-2;

            /* scan left, propagate distance from right */
            for(x=w-2; x>=0; x--, i--)
            {
                olddist = dist[i];
                if(olddist <= 0) continue; // Already zero distance

                c = i+offset_r;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx-1;
                newdisty = cdisty;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    changed = 1;
                }
            }
        }

        /* Scan rows in reverse order, except last row */
        for(y=h-2; y>=0; y--)
        {
            /* move index to rightmost pixel of current row */
            i = y*w + w-1;

            /* Scan left, propagate distances from below & right */

            /* Rightmost pixel is special, has no right neighbors */
            olddist = dist[i];
            if(olddist > 0) // If not already zero distance
            {
                c = i+offset_d;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx;
                newdisty = cdisty-1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_dl;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx+1;
                newdisty = cdisty-1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    changed = 1;
                }
            }
            i--;

            /* Middle pixels have all neighbors */
            for(x=w-2; x>0; x--, i--)
            {
                olddist = dist[i];
                if(olddist <= 0) continue; // Already zero distance

                c = i+offset_r;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx-1;
                newdisty = cdisty;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_rd;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx-1;
                newdisty = cdisty-1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_d;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx;
                newdisty = cdisty-1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_dl;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx+1;
                newdisty = cdisty-1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    changed = 1;
                }
            }
            /* Leftmost pixel is special, has no left neighbors */
            olddist = dist[i];
            if(olddist > 0) // If not already zero distance
            {
                c = i+offset_r;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx-1;
                newdisty = cdisty;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_rd;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx-1;
                newdisty = cdisty-1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    olddist=newdist;
                    changed = 1;
                }

                c = i+offset_d;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx;
                newdisty = cdisty-1;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    changed = 1;
                }
            }

            /* Move index to second leftmost pixel of current row. */
            /* Leftmost pixel is skipped, it has no left neighbor. */
            i = y*w + 1;
            for(x=1; x<w; x++, i++)
            {
                /* scan right, propagate distance from left */
                olddist = dist[i];
                if(olddist <= 0) continue; // Already zero distance

                c = i+offset_l;
                cdistx = distx[c];
                cdisty = disty[c];
                newdistx = cdistx+1;
                newdisty = cdisty;
                newdist = DISTAA(c, cdistx, cdisty, newdistx, newdisty);
                if(newdist < olddist-epsilon)
                {
                    distx[i]=newdistx;
                    disty[i]=newdisty;
                    dist[i]=newdist;
                    changed = 1;
                }
            }
        }
    }
    while(changed); // Sweep until no more updates are made
}

shared_ptr<Array2D<EuclideanDistance::DistanceResult>> EuclideanDistance::Calculate(int width, int height,
    const Array2D<float> &mask)
{
    shared_ptr<Array2D<DistanceResult>> result = make_shared<Array2D<DistanceResult>>(height, width);

    vector<float> gx(height*width), gy(height*width);
    computegradient(mask, width, height, gx.data(), gy.data());
    
    vector<float> Dout(width*height);
    vector<short> xdist(height*width); // local data
    vector<short> ydist(height*width);
    edtaa3(mask, gx.data(), gy.data(), width, height, xdist.data(), ydist.data(), Dout.data());

    for(int y=0; y<height; y++)
    {
	for(int x=0; x<width; x++)
	{
	    int i = y*width + x;
	    float distance = Dout[y*width+x];

	    // Coordinates of the closest pixel:
	    int srcX = x - xdist[i];
	    int srcY = y - ydist[i];

	    auto &r = (*result)[y][x];
	    r.sx = srcX;
	    r.sy = srcY;
	    r.distance = distance;
	}
    }

    return result;
}


#if 0
int main()
{
    int width = 3;
    int height = 3;
    vector<float> test = {
        0, 0, 0,
        0, .75, 0,
        0, 0, 0,
    };
    test.resize(width*height);
    vector<float> out;
    vector<int> idx;
    EuclideanDistance::Calculate(
        [&](int x, int y) {
            return test[y*width+x];
        },
            test, out, idx, width, height);

    for(int y = 0; y < height; ++y)
    {
        for(int x = 0; x < width; ++x)
        {
            float f = out[y*width+x];
//            f = (f*2);
//            printf("%i ", idx[y*2+x]);
            printf("%.3f ", f);
        }
        printf("\n");
    }

    return 0;
}
#endif

#if 0
void test()
{
    vector<vector<float>> test = {
	{ 0.0f, 0.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 0.0, 0.0f },
	{ 0.0f, 0.0f, 1.0, 0.0f },
	{ 0.0f, 0.0f, 0.0, 0.0f },
    };
    Array2D<float> out;
    out.resizeErase(test.size(), test[0].size());

    EuclideanDistance::Calculate(test[0].size(), test.size(),
	[&](int x, int y) {
	return test[y][x];
    }, [&](int x, int y, int sx, int sy, float distance) {
	out[y][x] = distance;
    });

    for(int y = 0; y < test.size(); ++y)
    {
	for(int x = 0; x < test[y].size(); ++x)
	{
	    printf("%.4f ", out[y][x]);
	}
	printf("\n");
    }
}
#endif
