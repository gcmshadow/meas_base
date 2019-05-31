# This file is part of meas_base.
#
# Developed for the LSST Data Management System.
# This product includes software developed by the LSST Project
# (https://www.lsst.org).
# See the COPYRIGHT file at the top-level directory of this distribution
# for details of code ownership.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""Base command-line driver task for forced measurement.

Must be inherited to specialize for a specific dataset to be used (see
`ForcedPhotCcdTask`, `ForcedPhotCoaddTask`).
"""

import lsst.afw.table
import lsst.pex.config
import lsst.daf.base
import lsst.pipe.base
import lsst.pex.config

from .references import MultiBandReferencesTask
from .forcedMeasurement import ForcedMeasurementTask
from .applyApCorr import ApplyApCorrTask
from .catalogCalculation import CatalogCalculationTask

__all__ = ("ForcedPhotImageConfig", "ForcedPhotImageTask")


class ForcedPhotImageConfig(lsst.pipe.base.PipelineTaskConfig):
    """Config class for forced measurement driver task."""
    # Gen 3 options
    inputSchema = lsst.pipe.base.InitInputDatasetField(
        doc="Schema for the input measurement catalogs.",
        nameTemplate="{inputCoaddName}Coadd_ref_schema",
        storageClass="SourceCatalog",
    )
    outputSchema = lsst.pipe.base.InitOutputDatasetField(
        doc="Schema for the output forced measurement catalogs.",
        nameTemplate="{outputCoaddName}Coadd_forced_src_schema",
        storageClass="SourceCatalog",
    )
    exposure = lsst.pipe.base.InputDatasetField(
        doc="Input exposure to perform photometry on.",
        nameTemplate="{inputCoaddName}Coadd",
        scalar=True,
        storageClass="ExposureF",
        dimensions=["abstract_filter", "skymap", "tract", "patch"],
    )
    refCat = lsst.pipe.base.InputDatasetField(
        doc="Catalog of shapes and positions at which to force photometry.",
        nameTemplate="{inputCoaddName}Coadd_ref",
        scalar=True,
        storageClass="SourceCatalog",
        dimensions=["skymap", "tract", "patch"],
    )
    skyMap = lsst.pipe.base.InputDatasetField(
        doc="SkyMap dataset that defines the coordinate system of the reference catalog.",
        nameTemplate="{inputCoaddName}Coadd_skyMap",
        scalar=True,
        storageClass="SkyMap",
        dimensions=["skymap"],
    )
    measCat = lsst.pipe.base.OutputDatasetField(
        doc="Output forced photometry catalog.",
        nameTemplate="{outputCoaddName}Coadd_forced_src",
        scalar=True,
        storageClass="SourceCatalog",
        dimensions=["abstract_filter", "skymap", "tract", "patch"],
    )

    # ForcedPhotImage options
    references = lsst.pex.config.ConfigurableField(
        target=MultiBandReferencesTask,
        doc="subtask to retrieve reference source catalog"
    )
    measurement = lsst.pex.config.ConfigurableField(
        target=ForcedMeasurementTask,
        doc="subtask to do forced measurement"
    )
    coaddName = lsst.pex.config.Field(
        doc="coadd name: typically one of deep or goodSeeing",
        dtype=str,
        default="deep",
    )
    doApCorr = lsst.pex.config.Field(
        dtype=bool,
        default=True,
        doc="Run subtask to apply aperture corrections"
    )
    applyApCorr = lsst.pex.config.ConfigurableField(
        target=ApplyApCorrTask,
        doc="Subtask to apply aperture corrections"
    )
    catalogCalculation = lsst.pex.config.ConfigurableField(
        target=CatalogCalculationTask,
        doc="Subtask to run catalogCalculation plugins on catalog"
    )

    def setDefaults(self):
        # Docstring inherited.
        # Make catalogCalculation a no-op by default as no modelFlux is setup by default in
        # ForcedMeasurementTask
        super().setDefaults()

        self.catalogCalculation.plugins.names = []
        self.formatTemplateNames({"inputCoaddName": "deep",
                                  "outputCoaddName": "deep",
                                  "inputName": None})
        self.quantum.dimensions = ("abstract_filter", "skymap", "tract", "patch")


class ForcedPhotImageTask(lsst.pipe.base.PipelineTask, lsst.pipe.base.CmdLineTask):
    """A base class for command-line forced measurement drivers.

    Parameters
    ----------
    butler : `lsst.daf.persistence.butler.Butler`, optional
        A Butler which will be passed to the references subtask to allow it to
        load its schema from disk. Optional, but must be specified if
        ``refSchema`` is not; if both are specified, ``refSchema`` takes
        precedence.
    refSchema : `lsst.afw.table.Schema`, optional
        The schema of the reference catalog, passed to the constructor of the
        references subtask. Optional, but must be specified if ``butler`` is
        not; if both are specified, ``refSchema`` takes precedence.
    **kwds
        Keyword arguments are passed to the supertask constructor.

    Notes
    -----
    This is a an abstract class, which is the common ancestor for
    `ForcedPhotCcdTask` and `ForcedPhotCoaddTask`. It provides the
    `runDataRef` method that does most of the work, while delegating a few
    customization tasks to other methods that are overridden by subclasses.

    This task is not directly usable as a command line task. Subclasses must:

    - Set the `_DefaultName` class attribute;
    - Implement `makeIdFactory`;
    - Implement `fetchReferences`;
    - Optionally, implement `attachFootprints`.
    """

    ConfigClass = ForcedPhotImageConfig
    _DefaultName = "processImageForcedTask"

    def __init__(self, butler=None, refSchema=None, initInputs=None, **kwds):
        super().__init__(**kwds)

        if initInputs is not None:
            refSchema = initInputs['inputSchema'].schema

        self.makeSubtask("references", butler=butler, schema=refSchema)
        if refSchema is None:
            refSchema = self.references.schema
        self.makeSubtask("measurement", refSchema=refSchema)
        # It is necessary to get the schema internal to the forced measurement task until such a time
        # that the schema is not owned by the measurement task, but is passed in by an external caller
        if self.config.doApCorr:
            self.makeSubtask("applyApCorr", schema=self.measurement.schema)
        self.makeSubtask('catalogCalculation', schema=self.measurement.schema)

    def getInitOutputDatasets(self):
        return {"outputSchema": lsst.afw.table.SourceCatalog(self.measurement.schema)}

    def adaptArgsAndRun(self, inputData, inputDataIds, outputDataIds, butler):
        tract = outputDataIds["measCat"]["tract"]
        skyMap = inputData.pop("skyMap")
        inputData['refWcs'] = skyMap[tract].getWcs()
        inputData['measCat'] = self.generateMeasCat(inputDataIds['exposure'],
                                                    inputData['exposure'],
                                                    inputData['refCat'], inputData['refWcs'],
                                                    "tract_patch", butler)

        return self.run(**inputData)

    def generateMeasCat(self, exposureDataId, exposure, refCat, refWcs, idPackerName, butler):
        """Generate a measurement catalog for Gen3.

        Parameters
        ----------
        exposureDataId : `DataId`
            Butler dataId for this exposure.
        exposure : `lsst.afw.image.exposure.Exposure`
            Exposure to generate the catalog for.
        refCat : `lsst.afw.table.SourceCatalog`
            Catalog of shapes and positions at which to force photometry.
        refWcs : `lsst.afw.image.SkyWcs`
            Reference world coordinate system.
        idPackerName : `str`
            Type of ID packer to construct from the registry.
        butler : `lsst.daf.persistence.butler.Butler`
            Butler to use to construct id packer.

        Returns
        -------
        measCat : `lsst.afw.table.SourceCatalog`
            Catalog of forced sources to measure.
        """
        packer = butler.registry.makeDataIdPacker(idPackerName, exposureDataId)
        expId = packer.pack(exposureDataId)
        expBits = packer.maxBits
        idFactory = lsst.afw.table.IdFactory.makeSource(expId, 64 - expBits)

        measCat = self.measurement.generateMeasCat(exposure, refCat, refWcs,
                                                   idFactory=idFactory)
        return measCat

    def runDataRef(self, dataRef, psfCache=None):
        """Perform forced measurement on a single exposure.

        Parameters
        ----------
        dataRef : `lsst.daf.persistence.ButlerDataRef`
            Passed to the ``references`` subtask to obtain the reference WCS,
            the ``getExposure`` method (implemented by derived classes) to
            read the measurment image, and the ``fetchReferences`` method to
            get the exposure and load the reference catalog (see
            :lsst-task`lsst.meas.base.references.CoaddSrcReferencesTask`).
            Refer to derived class documentation for details of the datasets
            and data ID keys which are used.
        psfCache : `int`, optional
            Size of PSF cache, or `None`. The size of the PSF cache can have
            a significant effect upon the runtime for complicated PSF models.

        Notes
        -----
        Sources are generated with ``generateMeasCat`` in the ``measurement``
        subtask. These are passed to ``measurement``'s ``run`` method, which
        fills the source catalog with the forced measurement results. The
        sources are then passed to the ``writeOutputs`` method (implemented by
        derived classes) which writes the outputs.
        """
        refWcs = self.references.getWcs(dataRef)
        exposure = self.getExposure(dataRef)
        if psfCache is not None:
            exposure.getPsf().setCacheSize(psfCache)
        refCat = self.fetchReferences(dataRef, exposure)

        measCat = self.measurement.generateMeasCat(exposure, refCat, refWcs,
                                                   idFactory=self.makeIdFactory(dataRef))
        self.log.info("Performing forced measurement on %s" % (dataRef.dataId,))
        self.attachFootprints(measCat, refCat, exposure, refWcs, dataRef)

        exposureId = self.getExposureId(dataRef)

        forcedPhotResult = self.run(measCat, exposure, refCat, refWcs, exposureId=exposureId)

        self.writeOutput(dataRef, forcedPhotResult.measCat)

    def run(self, measCat, exposure, refCat, refWcs, exposureId=None):
        """Perform forced measurement on a single exposure.

        Parameters
        ----------
        measCat : `lsst.afw.table.SourceCatalog`
            The measurement catalog, based on the sources listed in the
            reference catalog.
        exposure : `lsst.afw.image.Exposure`
            The measurement image upon which to perform forced detection.
        refCat : `lsst.afw.table.SourceCatalog`
            The reference catalog of sources to measure.
        refWcs : `lsst.afw.image.SkyWcs`
            The WCS for the references.
        exposureId : `int`
            Optional unique exposureId used for random seed in measurement
            task.

        Returns
        -------
        result : `lsst.pipe.base.Struct`
            Structure with fields:

            ``measCat``
                Catalog of forced measurement results
                (`lsst.afw.table.SourceCatalog`).
        """
        self.measurement.run(measCat, exposure, refCat, refWcs, exposureId=exposureId)
        if self.config.doApCorr:
            self.applyApCorr.run(
                catalog=measCat,
                apCorrMap=exposure.getInfo().getApCorrMap()
            )
        self.catalogCalculation.run(measCat)

        return lsst.pipe.base.Struct(measCat=measCat)

    def makeIdFactory(self, dataRef):
        """Hook for derived classes to make an ID factory for forced sources.

        Notes
        -----
        That this applies to forced *source* IDs, not object IDs, which are
        usually handled by the ``measurement.copyColumns`` config option.

        """
        raise NotImplementedError()

    def getExposureId(self, dataRef):
        raise NotImplementedError()

    def fetchReferences(self, dataRef, exposure):
        """Hook for derived classes to define how to get reference objects.

        Notes
        -----
        Derived classes should call one of the ``fetch*`` methods on the
        ``references`` subtask, but which one they call depends on whether the
        region to get references for is a easy to describe in patches (as it
        would be when doing forced measurements on a coadd), or is just an
        arbitrary box (as it would be for CCD forced measurements).
        """
        raise NotImplementedError()

    def attachFootprints(self, sources, refCat, exposure, refWcs, dataRef):
        r"""Attach footprints to blank sources prior to measurements.

        Notes
        -----
        `~lsst.afw.detection.Footprint`\ s for forced photometry must be in the
        pixel coordinate system of the image being measured, while the actual
        detections may start out in a different coordinate system.

        Subclasses of this class must implement this method to define how
        those `~lsst.afw.detection.Footprint`\ s should be generated.

        This default implementation transforms the
        `~lsst.afw.detection.Footprint`\ s from the reference catalog from the
        reference WCS to the exposure's WcS, which downgrades
        `lsst.afw.detection.heavyFootprint.HeavyFootprint`\ s into regular
        `~lsst.afw.detection.Footprint`\ s, destroying deblend information.
        """
        return self.measurement.attachTransformedFootprints(sources, refCat, exposure, refWcs)

    def getExposure(self, dataRef):
        """Read input exposure on which measurement will be performed.

        Parameters
        ----------
        dataRef : `lsst.daf.persistence.ButlerDataRef`
            Butler data reference.
        """
        return dataRef.get(self.dataPrefix + "calexp", immediate=True)

    def writeOutput(self, dataRef, sources):
        """Write forced source table

        Parameters
        ----------
        dataRef : `lsst.daf.persistence.ButlerDataRef`
            Butler data reference. The forced_src dataset (with
            self.dataPrefix prepended) is all that will be modified.
        sources : `lsst.afw.table.SourceCatalog`
            Catalog of sources to save.
        """
        dataRef.put(sources, self.dataPrefix + "forced_src", flags=lsst.afw.table.SOURCE_IO_NO_FOOTPRINTS)

    def getSchemaCatalogs(self):
        """The schema catalogs that will be used by this task.

        Returns
        -------
        schemaCatalogs : `dict`
            Dictionary mapping dataset type to schema catalog.

        Notes
        -----
        There is only one schema for each type of forced measurement. The
        dataset type for this measurement is defined in the mapper.
        """
        catalog = lsst.afw.table.SourceCatalog(self.measurement.schema)
        catalog.getTable().setMetadata(self.measurement.algMetadata)
        datasetType = self.dataPrefix + "forced_src"
        return {datasetType: catalog}

    def _getConfigName(self):
        # Documented in superclass
        return self.dataPrefix + "forced_config"

    def _getMetadataName(self):
        # Documented in superclass
        return self.dataPrefix + "forced_metadata"
