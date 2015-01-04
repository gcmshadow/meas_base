// -*- lsst-c++ -*-
/*
 * LSST Data Management System
 * Copyright 2008-2013 LSST Corporation.
 *
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the LSST License Statement and
 * the GNU General Public License along with this program.  If not,
 * see <http://www.lsstcorp.org/LegalNotices/>.
 */

#include "lsst/utils/ieee.h"
#include "lsst/utils/PowFast.h"

#include "boost/tuple/tuple.hpp"
#include "Eigen/LU"
#include "lsst/pex/logging/Trace.h"
#include "lsst/afw/image.h"
#include "lsst/afw/detection/Psf.h"
#include "lsst/afw/geom/Angle.h"
#include "lsst/afw/geom/ellipses.h"
#include "lsst/afw/table/Source.h"

#include "lsst/meas/base/exceptions.h"
#include "lsst/meas/base/SdssShape.h"
#include "lsst/meas/base/detail/SdssShapeImpl.h"

namespace pexPolicy = lsst::pex::policy;
namespace pexExceptions = lsst::pex::exceptions;
namespace pexLogging = lsst::pex::logging;
namespace afwDet = lsst::afw::detection;
namespace afwImage = lsst::afw::image;
namespace afwGeom = lsst::afw::geom;

namespace lsst {
/*
 * The exponential function that we use, which may be only an approximation to the true value of e^x
 */
#define USE_APPROXIMATE_EXP 1
#if USE_APPROXIMATE_EXP
    lsst::utils::PowFast const& powFast = lsst::utils::getPowFast<11>();
#endif
    
inline float
approxExp(float x)
{
#if USE_APPROXIMATE_EXP
    return powFast.exp(x);
#else
    return std::exp(x);
#endif
}

namespace meas {
namespace base {

namespace {  // anonymous

lsst::afw::geom::BoxI set_amom_bbox(int width, int height, float xcen, float ycen, 
                                    double sigma11_w, double , double sigma22_w, float maxRad);
    
/*****************************************************************************/
/*
 * Error analysis, courtesy of David Johnston, University of Chicago
 */
/*
 * This function takes the 4 Gaussian parameters A, sigmaXXW and the
 * sky variance and fills in the Fisher matrix from the least squares fit.
 *
 * Following "Numerical Recipes in C" section 15.5, it ignores the 2nd
 * derivative parts and so the fisher matrix is just a function of these
 * best fit model parameters. The components are calculated analytically.
 */
detail::SdssShapeImpl::Matrix4
calc_fisher(detail::SdssShapeImpl const& shape, // the Shape that we want the the Fisher matrix for
            float bkgd_var              // background variance level for object
           )
{
    float const A = shape.getI0();     // amplitude
    float const sigma11W = shape.getIxx();
    float const sigma12W = shape.getIxy();
    float const sigma22W = shape.getIyy();
    
    double const D = sigma11W*sigma22W - sigma12W*sigma12W;
   
    if (D <= std::numeric_limits<double>::epsilon()) {
        throw LSST_EXCEPT(lsst::pex::exceptions::DomainError,
                          "Determinant is too small calculating Fisher matrix");
    }
/*
 * a normalization factor
 */
    if (bkgd_var <= 0.0) {
        throw LSST_EXCEPT(lsst::pex::exceptions::DomainError,
                          (boost::format("Background variance must be positive (saw %g)") % bkgd_var).str());
    }
    double const F = afwGeom::PI*sqrt(D)/bkgd_var;
/*
 * Calculate the 10 independent elements of the 4x4 Fisher matrix 
 */
    detail::SdssShapeImpl::Matrix4 fisher;

    double fac = F*A/(4.0*D);
    fisher(0, 0) =  F;
    fisher(0, 1) =  fac*sigma22W;
    fisher(1, 0) =  fisher(0, 1);
    fisher(0, 2) =  fac*sigma11W;                      
    fisher(2, 0) =  fisher(0, 2);
    fisher(0, 3) = -fac*2*sigma12W;    
    fisher(3, 0) =  fisher(0, 3);
    
    fac = 3.0*F*A*A/(16.0*D*D);
    fisher(1, 1) =  fac*sigma22W*sigma22W;
    fisher(2, 2) =  fac*sigma11W*sigma11W;
    fisher(3, 3) =  fac*4.0*(sigma12W*sigma12W + D/3.0);
    
    fisher(1, 2) =  fisher(3, 3)/4.0;
    fisher(2, 1) =  fisher(1, 2);
    fisher(1, 3) =  fac*(-2*sigma22W*sigma12W);
    fisher(3, 1) =  fisher(1, 3);
    fisher(2, 3) =  fac*(-2*sigma11W*sigma12W);
    fisher(3, 2) =  fisher(2, 3);
    
    return fisher;
}
//
// Here's a class to allow us to get the Image and variance from an Image or MaskedImage
//
template<typename ImageT>               // general case
struct ImageAdaptor {
    typedef ImageT Image;

    Image const& getImage(ImageT const& image) const {
        return image;
    }

    double getVariance(ImageT const&, int, int) {
        return std::numeric_limits<double>::quiet_NaN();
    }
};
    
template<typename T>                    // specialise to a MaskedImage
struct ImageAdaptor<afwImage::MaskedImage<T> > {
    typedef typename afwImage::MaskedImage<T>::Image Image;

    Image const& getImage(afwImage::MaskedImage<T> const& mimage) const {
        return *mimage.getImage();
    }

    double getVariance(afwImage::MaskedImage<T> const& mimage, int ix, int iy) {
        return mimage.at(ix, iy).variance();
    }
};

/// Calculate weights from moments
boost::tuple<std::pair<bool, double>, double, double, double>
getWeights(double sigma11, double sigma12, double sigma22, ///< Moments
           bool careful=true                               ///< Deal carefully with singular moments matrices?
    ) {
    double const NaN = std::numeric_limits<double>::quiet_NaN();
    if (lsst::utils::isnan(sigma11) || lsst::utils::isnan(sigma12) || lsst::utils::isnan(sigma22)) {
        return boost::make_tuple(std::make_pair(false, NaN), NaN, NaN, NaN);
    }
    double const det = sigma11*sigma22 - sigma12*sigma12; // determinant of sigmaXX matrix
    if (lsst::utils::isnan(det) || det < std::numeric_limits<float>::epsilon()) {
        double const nan = std::numeric_limits<double>::quiet_NaN();
        return boost::make_tuple(std::make_pair(false, nan), nan, nan, nan);
    }
    
    if (lsst::utils::isnan(det) || det < std::numeric_limits<float>::epsilon()) { // a suitably small number

        /*
         * We have to be a little careful here.  For some degenerate cases (e.g. an object that it zero
         * except on a line) the moments matrix can be singular.  We deal with this by adding 1/12 in
         * quadrature to the principal axes.
         *
         * Why bother?  Because we use the shape code for e.g. 2nd moment based star selection, and it
         * needs to be robust.
         */
        double const iMin = 1/12.0;                                          // 2nd moment of single pixel

        if (!careful) {
            // Don't want to be careful --- report bad determinant
            return boost::make_tuple(std::make_pair(false, det), NaN, NaN, NaN);
        }

        lsst::afw::geom::ellipses::Quadrupole const q(sigma11, sigma22, sigma12); // Ixx, Iyy, Ixy
        lsst::afw::geom::ellipses::Axes axes(q);                                  // convert to (a, b, theta)
        
        axes.setA(::sqrt(::pow(axes.getA(), 2) + iMin));
        axes.setB(::sqrt(::pow(axes.getB(), 2) + iMin));
            
        lsst::afw::geom::ellipses::Quadrupole const q2(axes); // back to Ixx etc.
        
        lsst::afw::geom::ellipses::Quadrupole::Matrix const mat = q2.getMatrix().inverse();
        
        return boost::make_tuple(std::make_pair(true, q2.getDeterminant()), mat(0, 0), mat(1, 0), mat(1, 1));
    }

    assert(sigma11*sigma22 >= sigma12*sigma12 - std::numeric_limits<float>::epsilon());

    return boost::make_tuple(std::make_pair(true, det), sigma22/det, -sigma12/det, sigma11/det);
}

/// Should we be interpolating?
bool shouldInterp(double sigma11, double sigma22, double det) {
    float const xinterp = 0.25; // I.e. 0.5*0.5
    return (sigma11 < xinterp || sigma22 < xinterp || det < xinterp*xinterp);
}


/************************************************************************************************************/
/*
 * Decide on the bounding box for the region to examine while calculating
 * the adaptive moments
 */
lsst::afw::geom::BoxI set_amom_bbox(int width, int height, // size of region
                                     float xcen, float ycen,        // centre of object
                                     double sigma11_w,              // quadratic moments of the
                                     double ,                       //         weighting function
                                     double sigma22_w,              //                    xx, xy, and yy
                                     float maxRad = 1000              // Maximum radius of area to use
                                    )
{
    float rad = 4*sqrt(((sigma11_w > sigma22_w) ? sigma11_w : sigma22_w));
        
    if (rad > maxRad) {
        rad = maxRad;
    }
        
    int ix0 = static_cast<int>(xcen - rad - 0.5);
    ix0 = (ix0 < 0) ? 0 : ix0;
    int iy0 = static_cast<int>(ycen - rad - 0.5);
    iy0 = (iy0 < 0) ? 0 : iy0;
    lsst::afw::geom::Point2I llc(ix0, iy0); // Desired lower left corner
        
    int ix1 = static_cast<int>(xcen + rad + 0.5);
    if (ix1 >= width) {
        ix1 = width - 1;
    }
    int iy1 = static_cast<int>(ycen + rad + 0.5);
    if (iy1 >= height) {
        iy1 = height - 1;
    }
    lsst::afw::geom::Point2I urc(ix1, iy1); // Desired upper right corner
        
    return lsst::afw::geom::BoxI(llc, urc);
}   

/*****************************************************************************/
/*
 * Calculate weighted moments of an object up to 2nd order
 */
template<bool fluxOnly, typename ImageT>
static int
calcmom(ImageT const& image,            // the image data
        float xcen, float ycen,         // centre of object
        lsst::afw::geom::BoxI bbox,    // bounding box to consider
        float bkgd,                     // data's background level
        bool interpflag,                // interpolate within pixels?
        double w11, double w12, double w22, // weights
        double *pI0,                        // amplitude of fit
        double *psum,                       // sum w*I (if !NULL)
        double *psumx, double *psumy,       // sum [xy]*w*I (if !fluxOnly)
        double *psumxx, double *psumxy, double *psumyy, // sum [xy]^2*w*I (if !fluxOnly)
        double *psums4                                  // sum w*I*weight^2 (if !fluxOnly && !NULL)
       )
{
    
    float tmod, ymod;
    float X, Y;                          // sub-pixel interpolated [xy]
    float weight;
    float tmp;
    double sum, sumx, sumy, sumxx, sumyy, sumxy, sums4;
#define RECALC_W 0                      // estimate sigmaXX_w within BBox?
#if RECALC_W
    double wsum, wsumxx, wsumxy, wsumyy;

    wsum = wsumxx = wsumxy = wsumyy = 0;
#endif

    assert(w11 >= 0);                   // i.e. it was set
    if (fabs(w11) > 1e6 || fabs(w12) > 1e6 || fabs(w22) > 1e6) {
        return(-1);
    }

    sum = sumx = sumy = sumxx = sumxy = sumyy = sums4 = 0;

    int const ix0 = bbox.getMinX();       // corners of the box being analyzed
    int const ix1 = bbox.getMaxX();
    int const iy0 = bbox.getMinY();       // corners of the box being analyzed
    int const iy1 = bbox.getMaxY();

    if (ix0 < 0 || ix1 >= image.getWidth() || iy0 < 0 || iy1 >= image.getHeight()) {
        return -1;
    }

    for (int i = iy0; i <= iy1; ++i) {
        typename ImageT::x_iterator ptr = image.x_at(ix0, i);
        float const y = i - ycen;
        float const y2 = y*y;
        float const yl = y - 0.375;
        float const yh = y + 0.375;
        for (int j = ix0; j <= ix1; ++j, ++ptr) {
            float x = j - xcen;
            if (interpflag) {
                float const xl = x - 0.375;
                float const xh = x + 0.375;
               
                float expon = xl*xl*w11 + yl*yl*w22 + 2.0*xl*yl*w12;
                tmp = xh*xh*w11 + yh*yh*w22 + 2.0*xh*yh*w12;
                expon = (expon > tmp) ? expon : tmp;
                tmp = xl*xl*w11 + yh*yh*w22 + 2.0*xl*yh*w12;
                expon = (expon > tmp) ? expon : tmp;
                tmp = xh*xh*w11 + yl*yl*w22 + 2.0*xh*yl*w12;
                expon = (expon > tmp) ? expon : tmp;
               
                if (expon <= 9.0) {
                    tmod = *ptr - bkgd;
                    for (Y = yl; Y <= yh; Y += 0.25) {
                        double const interpY2 = Y*Y;
                        for (X = xl; X <= xh; X += 0.25) {
                            double const interpX2 = X*X;
                            double const interpXy = X*Y;
                            expon = interpX2*w11 + 2*interpXy*w12 + interpY2*w22;
                            weight = approxExp(-0.5*expon);
                           
                            ymod = tmod*weight;
                            sum += ymod;
                            if (!fluxOnly) {
                                sumx += ymod*(X + xcen);
                                sumy += ymod*(Y + ycen);
#if RECALC_W
                                wsum += weight;
                           
                                tmp = interpX2*weight;
                                wsumxx += tmp;
                                sumxx += tmod*tmp;
                           
                                tmp = interpXy*weight;
                                wsumxy += tmp;
                                sumxy += tmod*tmp;
                           
                                tmp = interpY2*weight;
                                wsumyy += tmp;
                                sumyy += tmod*tmp;
#else
                                sumxx += interpX2*ymod;
                                sumxy += interpXy*ymod;
                                sumyy += interpY2*ymod;
#endif
                                sums4 += expon*expon*ymod;
                            }
                        }
                    }
                }
            } else {
                float x2 = x*x;
                float xy = x*y;
                float expon = x2*w11 + 2*xy*w12 + y2*w22;
               
                if (expon <= 14.0) {
                    weight = approxExp(-0.5*expon);
                    tmod = *ptr - bkgd;
                    ymod = tmod*weight;
                    sum += ymod;
                    if (!fluxOnly) {
                        sumx += ymod*j;
                        sumy += ymod*i;
#if RECALC_W
                        wsum += weight;
                   
                        tmp = x2*weight;
                        wsumxx += tmp;
                        sumxx += tmod*tmp;
                   
                        tmp = xy*weight;
                        wsumxy += tmp;
                        sumxy += tmod*tmp;
                   
                        tmp = y2*weight;
                        wsumyy += tmp;
                        sumyy += tmod*tmp;
#else
                        sumxx += x2*ymod;
                        sumxy += xy*ymod;
                        sumyy += y2*ymod;
#endif
                        sums4 += expon*expon*ymod;
                    }
                }
            }
        }
    }
   

    boost::tuple<std::pair<bool, double>, double, double, double> const weights = getWeights(w11, w12, w22);
    double const detW = weights.get<1>()*weights.get<3>() - std::pow(weights.get<2>(), 2);
    *pI0 = sum/(afwGeom::PI*sqrt(detW));
    if (psum) {
        *psum = sum;
    }
    if (!fluxOnly) {
        *psumx = sumx;
        *psumy = sumy;
        *psumxx = sumxx;
        *psumxy = sumxy;
        *psumyy = sumyy;
        if (psums4 != NULL) {
            *psums4 = sums4;
        }
    }

#if RECALC_W
    if (wsum > 0 && !fluxOnly) {
        double det = w11*w22 - w12*w12;
        wsumxx /= wsum;
        wsumxy /= wsum;
        wsumyy /= wsum;
        printf("%g %g %g  %g %g %g\n", w22/det, -w12/det, w11/det, wsumxx, wsumxy, wsumyy);
    }
#endif

    return (fluxOnly || (sum > 0 && sumxx > 0 && sumyy > 0)) ? 0 : -1;
}

} // anonymous namespace

/************************************************************************************************************/

namespace detail {
/**
 * Workhorse for adaptive moments
 */
template<typename ImageT>
bool getAdaptiveMoments(ImageT const& mimage, double bkgd, double xcen, double ycen, double shiftmax,
                        SdssShapeImpl *shape, int maxIter, float tol1, float tol2)
{
    double I0 = 0;                      // amplitude of best-fit Gaussian
    double sum;                         // sum of intensity*weight
    double sumx, sumy;                  // sum ((int)[xy])*intensity*weight
    double sumxx, sumxy, sumyy;         // sum {x^2,xy,y^2}*intensity*weight
    double sums4;                       // sum intensity*weight*exponent^2
    float const xcen0 = xcen;           // initial centre
    float const ycen0 = ycen;           //                of object

    double sigma11W = 1.5;              // quadratic moments of the
    double sigma12W = 0.0;              //     weighting fcn;
    double sigma22W = 1.5;              //               xx, xy, and yy

    double w11 = -1, w12 = -1, w22 = -1;        // current weights for moments; always set when iter == 0
    float e1_old = 1e6, e2_old = 1e6;           // old values of shape parameters e1 and e2
    float sigma11_ow_old = 1e6;                 // previous version of sigma11_ow
    
    typename ImageAdaptor<ImageT>::Image const &image = ImageAdaptor<ImageT>().getImage(mimage);

    if (lsst::utils::isnan(xcen) || lsst::utils::isnan(ycen)) {
        // Can't do anything
        shape->setFlag(SdssShapeImpl::UNWEIGHTED_BAD);
        return false;
    }

    bool interpflag = false;            // interpolate finer than a pixel?
    lsst::afw::geom::BoxI bbox;
    int iter = 0;                       // iteration number
    for (; iter < maxIter; iter++) {
        bbox = set_amom_bbox(image.getWidth(), image.getHeight(), xcen, ycen, sigma11W, sigma12W, sigma22W);
        boost::tuple<std::pair<bool, double>, double, double, double> weights = 
            getWeights(sigma11W, sigma12W, sigma22W);
        if (!weights.get<0>().first) {
            shape->setFlag(SdssShapeImpl::UNWEIGHTED);
            break;
        }

        double const detW = weights.get<0>().second;
        
#if 0                                   // this form was numerically unstable on my G4 powerbook
        assert(detW >= 0.0);
#else
        assert(sigma11W*sigma22W >= sigma12W*sigma12W - std::numeric_limits<float>::epsilon());
#endif

        {
            const double ow11 = w11;    // old
            const double ow12 = w12;    //     values
            const double ow22 = w22;    //            of w11, w12, w22

            w11 = weights.get<1>();
            w12 = weights.get<2>();
            w22 = weights.get<3>();

            if (shouldInterp(sigma11W, sigma22W, detW)) {
                if (!interpflag) {
                    interpflag = true;       // N.b.: stays set for this object
                    if (iter > 0) {
                        sigma11_ow_old = 1.e6; // force at least one more iteration
                        w11 = ow11;
                        w12 = ow12;
                        w22 = ow22;
                        iter--;         // we didn't update wXX
                    }
                }
            }
        }

        if (calcmom<false>(image, xcen, ycen, bbox, bkgd, interpflag, w11, w12, w22,
                           &I0, &sum, &sumx, &sumy, &sumxx, &sumxy, &sumyy, &sums4) < 0) {
            shape->setFlag(SdssShapeImpl::UNWEIGHTED);
            break;
        }
#if 0
/*
 * Find new centre
 *
 * This is only needed if we update the centre; if we use the input position we've already done the work
 */
        xcen = sumx/sum;
        ycen = sumy/sum;
#endif
        shape->setX(sumx/sum); // update centroid.  N.b. we're not setting errors here
        shape->setY(sumy/sum);

        if (fabs(shape->getX() - xcen0) > shiftmax || fabs(shape->getY() - ycen0) > shiftmax) {
            shape->setFlag(SdssShapeImpl::SHIFT);
        }
/*
 * OK, we have the centre. Proceed to find the second moments.
 */
        float const sigma11_ow = sumxx/sum; // quadratic moments of
        float const sigma22_ow = sumyy/sum; //          weight*object
        float const sigma12_ow = sumxy/sum; //                 xx, xy, and yy 

        if (sigma11_ow <= 0 || sigma22_ow <= 0) {
            shape->setFlag(SdssShapeImpl::UNWEIGHTED);
            break;
        }

        float const d = sigma11_ow + sigma22_ow; // current values of shape parameters
        float const e1 = (sigma11_ow - sigma22_ow)/d;
        float const e2 = 2.0*sigma12_ow/d;
/*
 * Did we converge?
 */
        if (iter > 0 &&
           fabs(e1 - e1_old) < tol1 && fabs(e2 - e2_old) < tol1 &&
           fabs(sigma11_ow/sigma11_ow_old - 1.0) < tol2 ) {
            break;                              // yes; we converged
        }

        e1_old = e1;
        e2_old = e2;
        sigma11_ow_old = sigma11_ow;
/*
 * Didn't converge, calculate new values for weighting function
 *
 * The product of two Gaussians is a Gaussian:
 * <x^2 exp(-a x^2 - 2bxy - cy^2) exp(-Ax^2 - 2Bxy - Cy^2)> = 
 *                            <x^2 exp(-(a + A) x^2 - 2(b + B)xy - (c + C)y^2)>
 * i.e. the inverses of the covariances matrices add.
 *
 * We know sigmaXX_ow and sigmaXXW, the covariances of the weighted object
 * and of the weights themselves.  We can estimate the object's covariance as
 *   sigmaXX_ow^-1 - sigmaXXW^-1
 * and, as we want to find a set of weights with the _same_ covariance as the
 * object we take this to be the an estimate of our correct weights.
 *
 * N.b. This assumes that the object is roughly Gaussian.
 * Consider the object:
 *   O == delta(x + p) + delta(x - p)
 * the covariance of the weighted object is equal to that of the unweighted
 * object, and this prescription fails badly.  If we detect this, we set
 * the UNWEIGHTED flag, and calculate the UNweighted moments
 * instead.
 */
        {
            float n11, n12, n22;                // elements of inverse of next guess at weighting function
            float ow11, ow12, ow22;             // elements of inverse of sigmaXX_ow

            boost::tuple<std::pair<bool, double>, double, double, double> weights = 
                getWeights(sigma11_ow, sigma12_ow, sigma22_ow);
            if (!weights.get<0>().first) {
                shape->setFlag(SdssShapeImpl::UNWEIGHTED);
                break;
            }
         
            ow11 = weights.get<1>();
            ow12 = weights.get<2>();
            ow22 = weights.get<3>();

            n11 = ow11 - w11;
            n12 = ow12 - w12;
            n22 = ow22 - w22;

            weights = getWeights(n11, n12, n22, false);
            if (!weights.get<0>().first) {
                // product-of-Gaussians assumption failed
                shape->setFlag(SdssShapeImpl::UNWEIGHTED);
                break;
            }
      
            sigma11W = weights.get<1>();
            sigma12W = weights.get<2>();
            sigma22W = weights.get<3>();
        }

        if (sigma11W <= 0 || sigma22W <= 0) {
            shape->setFlag(SdssShapeImpl::UNWEIGHTED);
            break;
        }
    }

    if (iter == maxIter) {
        shape->setFlag(SdssShapeImpl::UNWEIGHTED);
        shape->setFlag(SdssShapeImpl::MAXITER);
    }

    if (sumxx + sumyy == 0.0) {
        shape->setFlag(SdssShapeImpl::UNWEIGHTED);
    }
/*
 * Problems; try calculating the un-weighted moments
 */
    if (shape->getFlag(SdssShapeImpl::UNWEIGHTED)) {
        w11 = w22 = w12 = 0;
        if (calcmom<false>(image, xcen, ycen, bbox, bkgd, interpflag, w11, w12, w22,
                           &I0, &sum, &sumx, &sumy, &sumxx, &sumxy, &sumyy, NULL) < 0 || sum <= 0) {
            shape->resetFlag(SdssShapeImpl::UNWEIGHTED);
            shape->setFlag(SdssShapeImpl::UNWEIGHTED_BAD);

            if (sum > 0) {
                shape->setIxx(1/12.0);      // a single pixel
                shape->setIxy(0.0);
                shape->setIyy(1/12.0);
            }
            
            return false;
        }

        sigma11W = sumxx/sum;          // estimate of object moments
        sigma12W = sumxy/sum;          //   usually, object == weight
        sigma22W = sumyy/sum;          //      at this point
    }

    shape->setI0(I0);
    shape->setIxx(sigma11W);
    shape->setIxy(sigma12W);
    shape->setIyy(sigma22W);
    shape->setIxy4(sums4/sum);

    if (shape->getIxx() + shape->getIyy() != 0.0) {
        int const ix = lsst::afw::image::positionToIndex(xcen);
        int const iy = lsst::afw::image::positionToIndex(ycen);
        
        if (ix >= 0 && ix < mimage.getWidth() && iy >= 0 && iy < mimage.getHeight()) {
            float const bkgd_var =
                ImageAdaptor<ImageT>().getVariance(mimage, ix, iy); // XXX Overestimate as it includes object

            if (bkgd_var > 0.0) {                                   // NaN is not > 0.0
                if (!(shape->getFlag(SdssShapeImpl::UNWEIGHTED))) {
                    SdssShapeImpl::Matrix4 fisher = calc_fisher(*shape, bkgd_var); // Fisher matrix 
                    shape->setCovar(fisher.inverse());
                }
            }
        }
    }

    return true;
}

/**
 * \brief Return the flux of an object, using the aperture described by the SdssShape object
 *
 * The SdssShape algorithm calculates an elliptical Gaussian fit to an object, so the "aperture" is
 * an elliptical Gaussian
 *
 * \returns A std::pair of the flux and its error
 */
template<typename ImageT>
std::pair<double, double>
getFixedMomentsFlux(ImageT const& image,               ///< the data to process
                    double bkgd,                       ///< background level
                    double xcen,                       ///< x-centre of object (PARENT coordinates)
                    double ycen,                       ///< y-centre of object (PARENT coordinates)
                    SdssShapeImpl const& shape_ ///< The SdssShape of the object
    )
{
    SdssShapeImpl shape = shape_; // we need a mutable copy

    afwGeom::BoxI const& bbox = set_amom_bbox(image.getWidth(), image.getHeight(), xcen, ycen,
                                              shape.getIxx(), shape.getIxy(), shape.getIyy());

    boost::tuple<std::pair<bool, double>, double, double, double> weights =
        getWeights(shape.getIxx(), shape.getIxy(), shape.getIyy());
    double const NaN = std::numeric_limits<double>::quiet_NaN();
    if (!weights.get<0>().first) {
        return std::make_pair(NaN, NaN);
    }

    double const w11 = weights.get<1>();
    double const w12 = weights.get<2>();
    double const w22 = weights.get<3>();
    bool const interp = shouldInterp(shape.getIxx(), shape.getIyy(), weights.get<0>().second);

    double I0 = 0;                      // amplitude of Gaussian
    calcmom<true>(ImageAdaptor<ImageT>().getImage(image), xcen - image.getX0(), ycen - image.getY0(),
                  bbox, bkgd, interp, w11, w12, w22,
                  &I0, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    /*
     * We have enerything we need, but it isn't quite packaged right; we need an initialised SdssShapeImpl
     */
    shape.setI0(I0);

    {
        int ix = static_cast<int>(xcen - image.getX0());
        int iy = static_cast<int>(ycen - image.getY0());
        float bkgd_var = 
            ImageAdaptor<ImageT>().getVariance(image, ix, iy); // XXX Overestimate as it includes object

        SdssShapeImpl::Matrix4 fisher = calc_fisher(shape, bkgd_var); // Fisher matrix 

        shape.setCovar(fisher.inverse());
    }
    
    double const scale = shape.getFluxScale();
    return std::make_pair(shape.getI0()*scale, shape.getI0Err()*scale);
}

} // end detail namespace


SdssShapeResult::SdssShapeResult() :
    xy4(std::numeric_limits<ShapeElement>::quiet_NaN()),
    xy4Sigma(std::numeric_limits<ShapeElement>::quiet_NaN()),
    flux_xx_Cov(std::numeric_limits<ErrElement>::quiet_NaN()),
    flux_yy_Cov(std::numeric_limits<ErrElement>::quiet_NaN()),
    flux_xy_Cov(std::numeric_limits<ErrElement>::quiet_NaN())
{}

static boost::array<FlagDefinition,SdssShapeAlgorithm::N_FLAGS> const flagDefs = {{
        {"flag", "general failure flag, set if anything went wrong"},
        {"flag_unweightedBad", "Both weighted and unweighted moments were invalid"},
        {"flag_unweighted", "Weighted moments converged to an invalid value; using unweighted moments"},
        {"flag_shift", "centroid shifted by more than the maximum allowed amount"},
        {"flag_maxIter", "Too many iterations in adaptive moments"}
    }};

SdssShapeResultKey SdssShapeResultKey::addFields(
    afw::table::Schema & schema,
    std::string const & name
) {
    SdssShapeResultKey r;
    r._shapeResult = ShapeResultKey::addFields(schema, name, "elliptical Gaussian adaptive moments",
                                               SIGMA_ONLY);
    r._centroidResult = CentroidResultKey::addFields(schema, name, "elliptical Gaussian adaptive moments",
                                                     SIGMA_ONLY);
    r._fluxResult = FluxResultKey::addFields(schema, name, "elliptical Gaussian adaptive moments");
    r._xy4 = schema.addField<ShapeElement>(
        // TODO: get more mathematically precise documentation on this from RHL
        schema.join(name, "xy4"), "4th moment used in certain shear-estimation algorithms", "pixels^4"
    );
    r._xy4Sigma = schema.addField<ErrElement>(
        schema.join(name, "xy4Sigma"),
        "uncertainty on %s" + schema.join(name, "xy4"), "pixels^4"
    );
    r._flux_xx_Cov = schema.addField<ErrElement>(
        schema.join(name, "flux", "xx", "Cov"),
        (boost::format("uncertainty covariance between %s and %s")
         % schema.join(name, "flux") % schema.join(name, "xx")).str(),
        "dn*pixels^2"
    );
    r._flux_yy_Cov = schema.addField<ErrElement>(
        schema.join(name, "flux", "yy", "Cov"),
        (boost::format("uncertainty covariance between %s and %s")
         % schema.join(name, "flux") % schema.join(name, "yy")).str(),
        "dn*pixels^2"
    );
    r._flux_xy_Cov = schema.addField<ErrElement>(
        schema.join(name, "flux", "xy", "Cov"),
        (boost::format("uncertainty covariance between %s and %s")
         % schema.join(name, "flux") % schema.join(name, "xy")).str(),
        "dn*pixels^2"
    );
    r._flagHandler = FlagHandler::addFields(schema, name, flagDefs.begin(), flagDefs.end());
    return r;
}

SdssShapeResultKey::SdssShapeResultKey(afw::table::SubSchema const & s) :
    _shapeResult(s),
    _centroidResult(s),
    _fluxResult(s),
    _xy4(s["xy4"]),
    _xy4Sigma(s["xy4Sigma"]),
    _flux_xx_Cov(s["flux"]["xx"]["Cov"]),
    _flux_yy_Cov(s["flux"]["yy"]["Cov"]),
    _flux_xy_Cov(s["flux"]["xy"]["Cov"]),
    _flagHandler(s, flagDefs.begin(), flagDefs.end())
{}

SdssShapeResult SdssShapeResultKey::get(afw::table::BaseRecord const & record) const {
    SdssShapeResult result;
    static_cast<ShapeResult&>(result) = record.get(_shapeResult);
    static_cast<CentroidResult&>(result) = record.get(_centroidResult);
    static_cast<FluxResult&>(result) = record.get(_fluxResult);
    result.xy4 = record.get(_xy4);
    result.xy4Sigma = record.get(_xy4Sigma);
    result.flux_xx_Cov = record.get(_flux_xx_Cov);
    result.flux_yy_Cov = record.get(_flux_yy_Cov);
    result.flux_xy_Cov = record.get(_flux_xy_Cov);
    for (int n = 0; n < SdssShapeAlgorithm::N_FLAGS; ++n) {
        result.flags[n] = _flagHandler.getValue(record, n);
    }
    return result;
}

void SdssShapeResultKey::set(afw::table::BaseRecord & record, SdssShapeResult const & value) const {
    record.set(_shapeResult, value);
    record.set(_centroidResult, value);
    record.set(_fluxResult, value);
    record.set(_xy4, value.xy4);
    record.set(_xy4Sigma, value.xy4Sigma);
    record.set(_flux_xx_Cov, value.flux_xx_Cov);
    record.set(_flux_yy_Cov, value.flux_yy_Cov);
    record.set(_flux_xy_Cov, value.flux_xy_Cov);
    for (int n = 0; n < SdssShapeAlgorithm::N_FLAGS; ++n) {
        _flagHandler.setValue(record, n, value.flags[n]);
    }
}

bool SdssShapeResultKey::operator==(SdssShapeResultKey const & other) const {
    return _shapeResult == other._shapeResult &&
        _centroidResult == other._centroidResult &&
        _fluxResult == other._fluxResult &&
        _xy4 == other._xy4 &&
        _xy4Sigma == other._xy4Sigma &&
        _flux_xx_Cov == other._flux_xx_Cov &&
        _flux_yy_Cov == other._flux_yy_Cov &&
        _flux_xy_Cov == other._flux_xy_Cov;
    // don't bother with flags - if we've gotten this far, it's basically impossible the flags don't match
}

bool SdssShapeResultKey::isValid() const {
    return _shapeResult.isValid() &&
        _centroidResult.isValid() &&
        _fluxResult.isValid() &&
        _xy4.isValid() &&
        _xy4Sigma.isValid() &&
        _flux_xx_Cov.isValid() &&
        _flux_yy_Cov.isValid() &&
        _flux_xy_Cov.isValid();
    // don't bother with flags - if we've gotten this far, it's basically impossible the flags don't match
}

SdssShapeAlgorithm::SdssShapeAlgorithm(
    Control const & ctrl,
    std::string const & name,
    afw::table::Schema & schema
)
  : _ctrl(ctrl),
    _resultKey(ResultKey::addFields(schema, name)),
    _centroidExtractor(schema, name)
{}

template <typename T>
SdssShapeResult SdssShapeAlgorithm::apply(
    afw::image::Image<T> const & exposure,
    afw::detection::Footprint const & footprint,
    afw::geom::Point2D const & center,
    Control const & control
) {
    throw LSST_EXCEPT(
        pex::exceptions::LogicError,
        "Not implemented"
    );
}

template <typename T>
SdssShapeResult SdssShapeAlgorithm::apply(
    afw::image::MaskedImage<T> const & mimage,
    afw::detection::Footprint const & footprint,
    afw::geom::Point2D const & center,
    Control const & control
) {
    typedef typename afw::image::MaskedImage<T> MaskedImageT;
    double xcen = center.getX();         // object's column position
    double ycen = center.getY();         // object's row position

    xcen -= mimage.getX0();             // work in image Pixel coordinates
    ycen -= mimage.getY0();

    float shiftmax = control.maxShift;   // Max allowed centroid shift
    if (shiftmax < 2) {
        shiftmax = 2;
    } else if (shiftmax > 10) {
        shiftmax = 10;
    }

    SdssShapeResult result;
    detail::SdssShapeImpl shapeImpl;
    try {
        detail::getAdaptiveMoments(mimage, control.background, xcen, ycen, shiftmax, &shapeImpl,
                                   control.maxIter, control.tol1, control.tol2);
    } catch (pex::exceptions::Exception & err) {
        result.flags[FAILURE] = true;
    }

    result.x = shapeImpl.getX() + mimage.getX0();
    result.y = shapeImpl.getY() + mimage.getY0();
    // FIXME: should do off-diagonal covariance elements too
    result.xSigma = shapeImpl.getXErr();
    result.ySigma = shapeImpl.getYErr();
    result.xx = shapeImpl.getIxx();
    result.yy = shapeImpl.getIyy();
    result.xy = shapeImpl.getIxy();
    // FIXME: should do off-diagonal covariance elements too
    result.xxSigma = shapeImpl.getIxxErr();
    result.yySigma = shapeImpl.getIyyErr();
    result.xySigma = shapeImpl.getIxyErr();

    // Now set the flags from SdssShapeImpl
    for (int n = 0; n < detail::SdssShapeImpl::N_FLAGS; ++n) {
        if (shapeImpl.getFlag(detail::SdssShapeImpl::Flag(n))) {
            result.flags[n + 1] = true;
            result.flags[FAILURE] = true;
        }
    }
    return result;
}

void SdssShapeAlgorithm::measure(
    afw::table::SourceRecord & measRecord,
    afw::image::Exposure<float> const & exposure
) const {
    if (!measRecord.getFootprint()) {
        throw LSST_EXCEPT(
            pex::exceptions::RuntimeError,
            "No Footprint attached to SourceRecord"
        );
    }
    SdssShapeResult result = apply(
        exposure.getMaskedImage(), *measRecord.getFootprint(),
        _centroidExtractor(measRecord, _resultKey.getFlagHandler()),
        _ctrl
    );
    measRecord.set(_resultKey, result);
}

void SdssShapeAlgorithm::fail(
    afw::table::SourceRecord & measRecord,
    MeasurementError * error
) const {
    _resultKey.getFlagHandler().handleFailure(measRecord, error);
}



#define INSTANTIATE_IMAGE(IMAGE) \
    template bool detail::getAdaptiveMoments<IMAGE>( \
        IMAGE const&, double, double, double, double, SdssShapeImpl*, int, float, float); \
    template std::pair<double, double> detail::getFixedMomentsFlux<IMAGE>( \
        IMAGE const&, double, double, double, detail::SdssShapeImpl const&); \

#define INSTANTIATE_PIXEL(PIXEL) \
    INSTANTIATE_IMAGE(lsst::afw::image::Image<PIXEL>); \
    INSTANTIATE_IMAGE(lsst::afw::image::MaskedImage<PIXEL>);

INSTANTIATE_PIXEL(int);
INSTANTIATE_PIXEL(float);
INSTANTIATE_PIXEL(double);

#define INSTANTIATE(T)                                                  \
    template SdssShapeResult SdssShapeAlgorithm::apply(                 \
        afw::image::MaskedImage<T> const & exposure,                    \
        afw::detection::Footprint const & footprint,                    \
        afw::geom::Point2D const & position,                            \
        Control const & ctrl                                            \
    );                                                                  \
    template SdssShapeResult SdssShapeAlgorithm::apply(                 \
        afw::image::Image<T> const & exposure,                          \
        afw::detection::Footprint const & footprint,                    \
        afw::geom::Point2D const & position,                            \
        Control const & ctrl                                            \
    )

INSTANTIATE(float);
INSTANTIATE(double);

}}} // end namespace lsst::meas::base

