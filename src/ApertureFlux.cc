// -*- lsst-c++ -*-
/*
 * LSST Data Management System
 * Copyright 2008-2016 AURA/LSST.
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

#include <numeric>

#include "boost/algorithm/string/replace.hpp"

#include "ndarray/eigen.h"

#include "lsst/afw/math/offsetImage.h"
#include "lsst/afw/geom/ellipses/PixelRegion.h"
#include "lsst/afw/table/Source.h"
#include "lsst/meas/base/SincCoeffs.h"
#include "lsst/meas/base/ApertureFlux.h"

namespace lsst {
namespace meas {
namespace base {
namespace {
FlagDefinitionList flagDefinitions;
}  // namespace

FlagDefinition const ApertureFluxAlgorithm::FAILURE = flagDefinitions.addFailureFlag();
FlagDefinition const ApertureFluxAlgorithm::APERTURE_TRUNCATED =
        flagDefinitions.add("flag_apertureTruncated", "aperture did not fit within measurement image");
FlagDefinition const ApertureFluxAlgorithm::SINC_COEFFS_TRUNCATED = flagDefinitions.add(
        "flag_sincCoeffsTruncated", "full sinc coefficient image did not fit within measurement image");

FlagDefinitionList const &ApertureFluxAlgorithm::getFlagDefinitions() { return flagDefinitions; }

ApertureFluxControl::ApertureFluxControl() : radii(10), maxSincRadius(10.0), shiftKernel("lanczos5") {
    // defaults here stolen from HSC pipeline defaults
    static std::array<double, 10> defaultRadii = {{3.0, 4.5, 6.0, 9.0, 12.0, 17.0, 25.0, 35.0, 50.0, 70.0}};
    std::copy(defaultRadii.begin(), defaultRadii.end(), radii.begin());
}

std::string ApertureFluxAlgorithm::makeFieldPrefix(std::string const &name, double radius) {
    std::string prefix = (boost::format("%s_%.1f") % name % radius).str();
    return boost::replace_all_copy(prefix, ".", "_");
}

ApertureFluxAlgorithm::Keys::Keys(afw::table::Schema &schema, std::string const &prefix,
                                  std::string const &doc, bool isSinc)
        : instFluxKey(FluxResultKey::addFields(schema, prefix, doc)),
          flags(
                  //  The exclusion List can either be empty, or constain the sinc coeffs flag
                  FlagHandler::addFields(
                          schema, prefix, ApertureFluxAlgorithm::getFlagDefinitions(),
                          isSinc ? FlagDefinitionList() : FlagDefinitionList({{SINC_COEFFS_TRUNCATED}}))) {}

ApertureFluxAlgorithm::ApertureFluxAlgorithm(Control const &ctrl, std::string const &name,
                                             afw::table::Schema &schema, daf::base::PropertySet &metadata

                                             )
        : _ctrl(ctrl), _centroidExtractor(schema, name) {
    _keys.reserve(ctrl.radii.size());
    for (std::size_t i = 0; i < ctrl.radii.size(); ++i) {
        metadata.add(name + "_radii", ctrl.radii[i]);
        std::string prefix = ApertureFluxAlgorithm::makeFieldPrefix(name, ctrl.radii[i]);
        std::string doc = (boost::format("instFlux within %f-pixel aperture") % ctrl.radii[i]).str();
        _keys.push_back(Keys(schema, prefix, doc, ctrl.radii[i] <= ctrl.maxSincRadius));
    }
}

void ApertureFluxAlgorithm::fail(afw::table::SourceRecord &measRecord, MeasurementError *error) const {
    // This should only get called in the case of completely unexpected failures, so it's not terrible
    // that we just set the general failure flags for all radii here instead of trying to figure out
    // which ones we've already done.  Any known failure modes are handled inside measure().
    for (std::size_t i = 0; i < _ctrl.radii.size(); ++i) {
        _keys[i].flags.handleFailure(measRecord, error);
    }
}

void ApertureFluxAlgorithm::copyResultToRecord(Result const &result, afw::table::SourceRecord &record,
                                               int index) const {
    record.set(_keys[index].instFluxKey, result);
    if (result.getFlag(FAILURE.number)) {
        _keys[index].flags.setValue(record, FAILURE.number, true);
    }
    if (result.getFlag(APERTURE_TRUNCATED.number)) {
        _keys[index].flags.setValue(record, APERTURE_TRUNCATED.number, true);
    }
    if (result.getFlag(SINC_COEFFS_TRUNCATED.number)) {
        _keys[index].flags.setValue(record, SINC_COEFFS_TRUNCATED.number, true);
    }
}

namespace {

// Helper function for computeSincFlux get Sinc instFlux coefficients, and handle cases where the coeff
// image needs to be clipped to fit in the measurement image
template <typename T>
std::shared_ptr<afw::image::Image<T> const>
getSincCoeffs(geom::Box2I const &bbox,                      // measurement image bbox we need to fit inside
              afw::geom::ellipses::Ellipse const &ellipse,  // ellipse that defines the aperture
              ApertureFluxAlgorithm::Result &result,        // result object where we set flags if we do clip
              ApertureFluxAlgorithm::Control const &ctrl    // configuration
              ) {
    std::shared_ptr<afw::image::Image<T> const> cImage = SincCoeffs<T>::get(ellipse.getCore(), 0.0);
    cImage = afw::math::offsetImage(*cImage, ellipse.getCenter().getX(), ellipse.getCenter().getY(),
                                    ctrl.shiftKernel);
    if (!bbox.contains(cImage->getBBox())) {
        // We had to clip out at least part part of the coeff image,
        // but since that's much larger than the aperture (and close
        // to zero outside the aperture), it may not be a serious
        // problem.
        result.setFlag(ApertureFluxAlgorithm::SINC_COEFFS_TRUNCATED.number);
        geom::Box2I overlap = cImage->getBBox();
        overlap.clip(bbox);
        if (!overlap.contains(geom::Box2I(ellipse.computeBBox()))) {
            // The clipping was indeed serious, as we we did have to clip within
            // the aperture; can't expect any decent answer at this point.
            result.setFlag(ApertureFluxAlgorithm::APERTURE_TRUNCATED.number);
            result.setFlag(ApertureFluxAlgorithm::FAILURE.number);
        }
        cImage = std::make_shared<afw::image::Image<T> >(*cImage, overlap);
    }
    return cImage;
}

}  // namespace

template <typename T>
ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeSincFlux(
        afw::image::Image<T> const &image, afw::geom::ellipses::Ellipse const &ellipse, Control const &ctrl) {
    Result result;
    std::shared_ptr<afw::image::Image<T> const> cImage = getSincCoeffs<T>(image.getBBox(), ellipse, result, ctrl);
    if (result.getFlag(APERTURE_TRUNCATED.number)) return result;
    afw::image::Image<T> subImage(image, cImage->getBBox());
    result.instFlux =
            (ndarray::asEigenArray(subImage.getArray()) * ndarray::asEigenArray(cImage->getArray())).sum();
    return result;
}

template <typename T>
ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeSincFlux(
        afw::image::MaskedImage<T> const &image, afw::geom::ellipses::Ellipse const &ellipse,
        Control const &ctrl) {
    Result result;
    std::shared_ptr<afw::image::Image<T> const> cImage = getSincCoeffs<T>(image.getBBox(), ellipse, result, ctrl);
    if (result.getFlag(APERTURE_TRUNCATED.number)) return result;
    afw::image::MaskedImage<T> subImage(image, cImage->getBBox(afw::image::PARENT), afw::image::PARENT);
    result.instFlux = (ndarray::asEigenArray(subImage.getImage()->getArray()) *
                       ndarray::asEigenArray(cImage->getArray()))
                              .sum();
    result.instFluxErr =
            std::sqrt((ndarray::asEigenArray(subImage.getVariance()->getArray()).template cast<T>() *
                       ndarray::asEigenArray(cImage->getArray()).square())
                              .sum());
    return result;
}

template <typename T>
ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeNaiveFlux(
        afw::image::Image<T> const &image, afw::geom::ellipses::Ellipse const &ellipse, Control const &ctrl) {
    Result result;
    afw::geom::ellipses::PixelRegion region(ellipse);  // behaves mostly like a Footprint
    if (!image.getBBox().contains(region.getBBox())) {
        result.setFlag(APERTURE_TRUNCATED.number);
        result.setFlag(FAILURE.number);
        return result;
    }
    result.instFlux = 0;
    for (afw::geom::ellipses::PixelRegion::Iterator spanIter = region.begin(), spanEnd = region.end();
         spanIter != spanEnd; ++spanIter) {
        typename afw::image::Image<T>::x_iterator pixIter =
                image.x_at(spanIter->getBeginX() - image.getX0(), spanIter->getY() - image.getY0());
        result.instFlux += std::accumulate(pixIter, pixIter + spanIter->getWidth(), 0.0);
    }
    return result;
}

template <typename T>
ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeNaiveFlux(
        afw::image::MaskedImage<T> const &image, afw::geom::ellipses::Ellipse const &ellipse,
        Control const &ctrl) {
    Result result;
    afw::geom::ellipses::PixelRegion region(ellipse);  // behaves mostly like a Footprint
    if (!image.getBBox().contains(region.getBBox())) {
        result.setFlag(APERTURE_TRUNCATED.number);
        result.setFlag(FAILURE.number);
        return result;
    }
    result.instFlux = 0.0;
    result.instFluxErr = 0.0;
    for (afw::geom::ellipses::PixelRegion::Iterator spanIter = region.begin(), spanEnd = region.end();
         spanIter != spanEnd; ++spanIter) {
        typename afw::image::MaskedImage<T>::Image::x_iterator pixIter = image.getImage()->x_at(
                spanIter->getBeginX() - image.getX0(), spanIter->getY() - image.getY0());
        typename afw::image::MaskedImage<T>::Variance::x_iterator varIter = image.getVariance()->x_at(
                spanIter->getBeginX() - image.getX0(), spanIter->getY() - image.getY0());
        result.instFlux += std::accumulate(pixIter, pixIter + spanIter->getWidth(), 0.0);
        // we use this to hold variance as we accumulate...
        result.instFluxErr += std::accumulate(varIter, varIter + spanIter->getWidth(), 0.0);
    }
    result.instFluxErr = std::sqrt(result.instFluxErr);  // ...and switch back to sigma here.
    return result;
}

template <typename T>
ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeFlux(afw::image::Image<T> const &image,
                                                                 afw::geom::ellipses::Ellipse const &ellipse,
                                                                 Control const &ctrl) {
    return (afw::geom::ellipses::Axes(ellipse.getCore()).getB() <= ctrl.maxSincRadius)
                   ? computeSincFlux(image, ellipse, ctrl)
                   : computeNaiveFlux(image, ellipse, ctrl);
}

template <typename T>
ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeFlux(afw::image::MaskedImage<T> const &image,
                                                                 afw::geom::ellipses::Ellipse const &ellipse,
                                                                 Control const &ctrl) {
    return (afw::geom::ellipses::Axes(ellipse.getCore()).getB() <= ctrl.maxSincRadius)
                   ? computeSincFlux(image, ellipse, ctrl)
                   : computeNaiveFlux(image, ellipse, ctrl);
}
#define INSTANTIATE(T)                                                                                  \
    template ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeFlux(                          \
            afw::image::Image<T> const &, afw::geom::ellipses::Ellipse const &, Control const &);       \
    template ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeFlux(                          \
            afw::image::MaskedImage<T> const &, afw::geom::ellipses::Ellipse const &, Control const &); \
    template ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeSincFlux(                      \
            afw::image::Image<T> const &, afw::geom::ellipses::Ellipse const &, Control const &);       \
    template ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeSincFlux(                      \
            afw::image::MaskedImage<T> const &, afw::geom::ellipses::Ellipse const &, Control const &); \
    template ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeNaiveFlux(                     \
            afw::image::Image<T> const &, afw::geom::ellipses::Ellipse const &, Control const &);       \
    template ApertureFluxAlgorithm::Result ApertureFluxAlgorithm::computeNaiveFlux(                     \
            afw::image::MaskedImage<T> const &, afw::geom::ellipses::Ellipse const &, Control const &)

INSTANTIATE(float);
INSTANTIATE(double);

ApertureFluxTransform::ApertureFluxTransform(Control const &ctrl, std::string const &name,
                                             afw::table::SchemaMapper &mapper)
        : BaseTransform(name), _ctrl(ctrl) {
    for (std::size_t i = 0; i < _ctrl.radii.size(); ++i) {
        for (std::size_t j = 0; j < ApertureFluxAlgorithm::getFlagDefinitions().size(); j++) {
            FlagDefinition const &flag = ApertureFluxAlgorithm::getFlagDefinitions()[j];
            if (_ctrl.radii[i] > _ctrl.maxSincRadius &&
                flag == ApertureFluxAlgorithm::SINC_COEFFS_TRUNCATED) {
                continue;
            }
            mapper.addMapping(
                    mapper.getInputSchema()
                            .find<afw::table::Flag>(
                                    (boost::format("%s_%s") %
                                     ApertureFluxAlgorithm::makeFieldPrefix(name, _ctrl.radii[i]) % flag.name)
                                            .str())
                            .key);
        }
        _magKeys.push_back(MagResultKey::addFields(
                mapper.editOutputSchema(), ApertureFluxAlgorithm::makeFieldPrefix(name, _ctrl.radii[i])));
    }
}

void ApertureFluxTransform::operator()(afw::table::SourceCatalog const &inputCatalog,
                                       afw::table::BaseCatalog &outputCatalog, afw::geom::SkyWcs const &wcs,
                                       afw::image::Calib const &calib) const {
    checkCatalogSize(inputCatalog, outputCatalog);
    std::vector<FluxResultKey> instFluxKeys;
    for (std::size_t i = 0; i < _ctrl.radii.size(); ++i) {
        instFluxKeys.push_back(FluxResultKey(
                inputCatalog.getSchema()[ApertureFluxAlgorithm::makeFieldPrefix(_name, _ctrl.radii[i])]));
    }
    afw::table::SourceCatalog::const_iterator inSrc = inputCatalog.begin();
    afw::table::BaseCatalog::iterator outSrc = outputCatalog.begin();
    {
        // While noThrow is in scope, converting a negative instFlux to a magnitude
        // returns NaN rather than throwing.
        NoThrowOnNegativeFluxContext noThrow;
        for (; inSrc != inputCatalog.end() && outSrc != outputCatalog.end(); ++inSrc, ++outSrc) {
            for (std::size_t i = 0; i < _ctrl.radii.size(); ++i) {
                FluxResult instFluxResult = instFluxKeys[i].get(*inSrc);
                _magKeys[i].set(*outSrc,
                                calib.getMagnitude(instFluxResult.instFlux, instFluxResult.instFluxErr));
            }
        }
    }
}

}  // namespace base
}  // namespace meas
}  // namespace lsst
