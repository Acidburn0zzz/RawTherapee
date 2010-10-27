/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 * 
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RAWIMAGESOURCE_I_H_INCLUDED
#define RAWIMAGESOURCE_I_H_INCLUDED

#include <rawimagesource.h>

#include <curves.h>

#undef MAXVAL
#undef CLIP

#define MAXVAL  0xffff
#define CLIP(a) ((a)>0?((a)<MAXVAL?(a):MAXVAL):0)

namespace rtengine {

inline void RawImageSource::convert_row_to_YIQ (unsigned short* r, unsigned short* g, unsigned short* b, int* Y, int* I, int* Q, int W) {
  for (int j=0; j<W; j++) {
    Y[j] = 299 * r[j] + 587 * g[j] + 114 * b[j];
    I[j] = 596 * r[j] - 275 * g[j] - 321 * b[j];
    Q[j] = 212 * r[j] - 523 * g[j] + 311 * b[j];
  }
}

inline void RawImageSource::convert_row_to_RGB (unsigned short* r, unsigned short* g, unsigned short* b, int* Y, int* I, int* Q, int W) {
  for (int j=1; j<W-1; j++) {
    int ir = Y[j]/1000 + 0.956*I[j]/1000 + 0.621*Q[j]/1000;
    int ig = Y[j]/1000 - 0.272*I[j]/1000 - 0.647*Q[j]/1000;
    int ib = Y[j]/1000 - 1.105*I[j]/1000 + 1.702*Q[j]/1000;
    r[j] = CLIP(ir);
    g[j] = CLIP(ig);
    b[j] = CLIP(ib);
  }
}

inline void RawImageSource::convert_to_cielab_row (unsigned short* ar, unsigned short* ag, unsigned short* ab, short* oL, short* oa, short* ob) {

  for (int j=0; j<W; j++) {
    double r = ar[j];
    double g = ag[j];
    double b = ab[j];
  
    double x = lc00 * r + lc01 * g + lc02 * b;
    double y = lc10 * r + lc11 * g + lc12 * b;
    double z = lc20 * r + lc21 * g + lc22 * b;

	if (y>threshold)
      oL[j] = 300.0*cache[(int)y];
    else
      oL[j] = 300.0 * 903.3 * y / CMAXVAL;

    oa[j] = 32.0 * 500.0 * ((x>threshold ? cache[(int)x] : 7.787*x/CMAXVAL+16.0/116.0) - (y>threshold ? cache[(int)y] : 7.787*y/CMAXVAL+16.0/116.0));
    ob[j] = 32.0 * 200.0 * ((y>threshold ? cache[(int)y] : 7.787*y/CMAXVAL+16.0/116.0) - (z>threshold ? cache[(int)z] : 7.787*z/CMAXVAL+16.0/116.0));
  }
}

inline void RawImageSource::interpolate_row_g (unsigned short* agh, unsigned short* agv, int i) {

  for (int j=0; j<W; j++) {
    if (ISGREEN(ri,i,j)) {
      agh[j] = ri->data[i][j];
      agv[j] = ri->data[i][j];
    }
    else {
      int gh=0;
      int gv=0;
      if (j>1 && j<W-2) {
        gh = (-ri->data[i][j-2] + 2*ri->data[i][j-1] + 2*ri->data[i][j] + 2*ri->data[i][j+1] -ri->data[i][j+2]) / 4;
        int maxgh = MAX(ri->data[i][j-1], ri->data[i][j+1]);
        int mingh = MIN(ri->data[i][j-1], ri->data[i][j+1]);
        if (gh>maxgh)
            gh = maxgh;
        else if (gh<mingh)
            gh = mingh;
      }
      else if (j==0)
        gh = ri->data[i][1];
      else if (j==1)
        gh = (ri->data[i][0] + ri->data[i][2]) / 2;
      else if (j==W-1)
        gh = ri->data[i][W-2];
      else if (j==W-2)
        gh = (ri->data[i][W-1] + ri->data[i][W-3]) / 2;
 
     if (i>1 && i<H-2) {
        gv = (-ri->data[i-2][j] + 2*ri->data[i-1][j] + 2*ri->data[i][j] + 2*ri->data[i+1][j] - ri->data[i+2][j]) / 4;
        int maxgv = MAX(ri->data[i-1][j], ri->data[i+1][j]);
        int mingv = MIN(ri->data[i-1][j], ri->data[i+1][j]);
        if (gv>maxgv)
          gv = maxgv;
        else if (gv<mingv)
          gv = mingv;
      }
      else if (i==0)
        gv = ri->data[1][j];
      else if (i==1)
        gv = (ri->data[0][j] + ri->data[2][j]) / 2;
      else if (i==H-1)
        gv = ri->data[H-2][j];
      else if (i==H-2)
        gv = (ri->data[H-1][j] + ri->data[H-3][j]) / 2;

      agh[j] = CLIP(gh);
      agv[j] = CLIP(gv);
    }
  }
}

inline void RawImageSource::interpolate_row_rb (unsigned short* ar, unsigned short* ab, unsigned short* pg, unsigned short* cg, unsigned short* ng, int i) {
  if (ISRED(ri,i,0) || ISRED(ri,i,1)) {
    // RGRGR or GRGRGR line
    for (int j=0; j<W; j++) {
      if (ISRED(ri,i,j)) {
         // red is simple
         ar[j] = ri->data[i][j];
         // blue: cross interpolation
         int b = 0;
         int n = 0;
         if (i>0 && j>0) {
           b += ri->data[i-1][j-1] - pg[j-1];
           n++;
         }
         if (i>0 && j<W-1) {
           b += ri->data[i-1][j+1] - pg[j+1];
           n++;
         }
         if (i<H-1 && j>0) {
           b += ri->data[i+1][j-1] - ng[j-1];
           n++;
         }
         if (i<H-1 && j<W-1) {
           b += ri->data[i+1][j+1] - ng[j+1];
           n++;
         }
         b = cg[j] + b / n;
         ab[j] = CLIP(b);
      }
      else {
        // linear R-G interp. horizontally
        int r;
        if (j==0)
          r = cg[0] + ri->data[i][1] - cg[1];
        else if (j==W-1)
          r = cg[W-1] + ri->data[i][W-2] - cg[W-2];
        else
          r = cg[j] + (ri->data[i][j-1] - cg[j-1] + ri->data[i][j+1] - cg[j+1]) / 2;
        ar[j] = CLIP(r);
        // linear B-G interp. vertically
        int b;
        if (i==0)
          b = ng[j] + ri->data[1][j] - cg[j];
        else if (i==H-1)
          b = pg[j] + ri->data[H-2][j] - cg[j];
        else
          b = cg[j] + (ri->data[i-1][j] - pg[j] + ri->data[i+1][j] - ng[j]) / 2;
        ab[j] = CLIP(b);
      }
    }
  }
  else {
    // BGBGB or GBGBGB line
    for (int j=0; j<W; j++) {
      if (ISBLUE(ri,i,j)) {
         // red is simple
         ab[j] = ri->data[i][j];
         // blue: cross interpolation
         int r = 0;
         int n = 0;
         if (i>0 && j>0) {
           r += ri->data[i-1][j-1] - pg[j-1];
           n++;
         }
         if (i>0 && j<W-1) {
           r += ri->data[i-1][j+1] - pg[j+1];
           n++;
         }
         if (i<H-1 && j>0) {
           r += ri->data[i+1][j-1] - ng[j-1];
           n++;
         }
         if (i<H-1 && j<W-1) {
           r += ri->data[i+1][j+1] - ng[j+1];
           n++;
         }
         r = cg[j] + r / n;

         ar[j] = CLIP(r);
      }
      else {
        // linear B-G interp. horizontally
        int b;
        if (j==0)
          b = cg[0] + ri->data[i][1] - cg[1];
        else if (j==W-1)
          b = cg[W-1] + ri->data[i][W-2] - cg[W-2];
        else
          b = cg[j] + (ri->data[i][j-1] - cg[j-1] + ri->data[i][j+1] - cg[j+1]) / 2;
        ab[j] = CLIP(b);
        // linear R-G interp. vertically
        int r;
        if (i==0)
          r = ng[j] + ri->data[1][j] - cg[j];
        else if (i==H-1)
          r = pg[j] + ri->data[H-2][j] - cg[j];
        else
          r = cg[j] + (ri->data[i-1][j] - pg[j] + ri->data[i+1][j] - ng[j]) / 2;
        ar[j] = CLIP(r);
      }
    }
  }
}

inline void RawImageSource::interpolate_row_rb_mul_pp (unsigned short* ar, unsigned short* ab, unsigned short* pg, unsigned short* cg, unsigned short* ng, int i, double r_mul, double g_mul, double b_mul, int x1, int width, int skip) {

  if (ISRED(ri,i,0) || ISRED(ri,i,1)) {
    // RGRGR or GRGRGR line
    for (int j=x1, jx=0; jx<width; j+=skip, jx++) {
      if (ISRED(ri,i,j)) {
         // red is simple
         ar[jx] = CLIP(r_mul * ri->data[i][j]);
         // blue: cross interpolation
         int b = 0;
         int n = 0;
         if (i>0 && j>0) {
           b += b_mul*ri->data[i-1][j-1] - g_mul*pg[j-1];
           n++;
         }
         if (i>0 && j<W-1) {
           b += b_mul*ri->data[i-1][j+1] - g_mul*pg[j+1];
           n++;
         }
         if (i<H-1 && j>0) {
           b += b_mul*ri->data[i+1][j-1] - g_mul*ng[j-1];
           n++;
         }
         if (i<H-1 && j<W-1) {
           b += b_mul*ri->data[i+1][j+1] - g_mul*ng[j+1];
           n++;
         }
         b = g_mul*cg[j] + b / n;
         ab[jx] = CLIP(b);
      }
      else {
        // linear R-G interp. horizontally
        int r;
        if (j==0)
          r = g_mul*cg[0] + r_mul*ri->data[i][1] - g_mul*cg[1];
        else if (j==W-1)
          r = g_mul*cg[W-1] + r_mul*ri->data[i][W-2] - g_mul*cg[W-2];
        else
          r = g_mul*cg[j] + (r_mul*ri->data[i][j-1] - g_mul*cg[j-1] + r_mul*ri->data[i][j+1] - g_mul*cg[j+1]) / 2;
        ar[jx] = CLIP(r);
        // linear B-G interp. vertically
        int b;
        if (i==0)
          b = g_mul*ng[j] + b_mul*ri->data[1][j] - g_mul*cg[j];
        else if (i==H-1)
          b = g_mul*pg[j] + b_mul*ri->data[H-2][j] - g_mul*cg[j];
        else
          b = g_mul*cg[j] + (b_mul*ri->data[i-1][j] - g_mul*pg[j] + b_mul*ri->data[i+1][j] - g_mul*ng[j]) / 2;
        ab[jx] = CLIP(b);
      }
    }
  }
  else {
    // BGBGB or GBGBGB line
    for (int j=x1, jx=0; jx<width; j+=skip, jx++) {
      if (ISBLUE(ri,i,j)) {
         // red is simple
         ab[jx] = CLIP(b_mul*ri->data[i][j]);
         // blue: cross interpolation
         int r = 0;
         int n = 0;
         if (i>0 && j>0) {
           r += r_mul*ri->data[i-1][j-1] - g_mul*pg[j-1];
           n++;
         }
         if (i>0 && j<W-1) {
           r += r_mul*ri->data[i-1][j+1] - g_mul*pg[j+1];
           n++;
         }
         if (i<H-1 && j>0) {
           r += r_mul*ri->data[i+1][j-1] - g_mul*ng[j-1];
           n++;
         }
         if (i<H-1 && j<W-1) {
           r += r_mul*ri->data[i+1][j+1] - g_mul*ng[j+1];
           n++;
         }
         r = g_mul*cg[j] + r / n;

         ar[jx] = CLIP(r);
      }
      else {
        // linear B-G interp. horizontally
        int b;
        if (j==0)
          b = g_mul*cg[0] + b_mul*ri->data[i][1] - g_mul*cg[1];
        else if (j==W-1)
          b = g_mul*cg[W-1] + b_mul*ri->data[i][W-2] - g_mul*cg[W-2];
        else
          b = g_mul*cg[j] + (b_mul*ri->data[i][j-1] - g_mul*cg[j-1] + b_mul*ri->data[i][j+1] - g_mul*cg[j+1]) / 2;
        ab[jx] = CLIP(b);
        // linear R-G interp. vertically
        int r;
        if (i==0)
          r = g_mul*ng[j] + r_mul*ri->data[1][j] - g_mul*cg[j];
        else if (i==H-1)
          r = g_mul*pg[j] + r_mul*ri->data[H-2][j] - g_mul*cg[j];
        else
          r = g_mul*cg[j] + (r_mul*ri->data[i-1][j] - g_mul*pg[j] + r_mul*ri->data[i+1][j] - g_mul*ng[j]) / 2;
        ar[jx] = CLIP(r);
      }
    }
  }
}

};

#endif
