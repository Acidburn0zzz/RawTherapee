/*
 *  This file is part of RawTherapee.
 *
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
#include "multilangmgr.h"
#include "thumbnail.h"
#include <sstream>
#include <iomanip>
#include "options.h"
#include "../rtengine/mytime.h"
#include <cstdio>
#include <cstdlib>
#include <glibmm.h>
#include "../rtengine/imagedata.h"
#include <glib/gstdio.h>
#include "guiutils.h"
#include "profilestore.h"
#include "batchqueue.h"
#include "../rtengine/safegtk.h"

using namespace rtengine::procparams;

Thumbnail::Thumbnail (CacheManager* cm, const Glib::ustring& fname, CacheImageData* cf)
    : fname(fname), cfs(*cf), cachemgr(cm), ref(1), enqueueNumber(0), tpp(NULL),
      tagsSet(false), exifSet(false), iptcSet(false), paramsSet(false), defaultParamsSet(false), needsReProcessing(true),
      imageLoading(false), lastImg(NULL), lastW(0), lastH(0), lastScale(0), initial_(false)
{

    loadProcParams ();
    printf("Thumbnail::Thumbnail / file %s / loadProcParams -> rank=%d\n", fname.c_str(), pparams.rank);

    // should be safe to use the unprotected version of loadThumbnail, since we are in the constructor
    _loadThumbnail ();
    generateExifDateTimeStrings ();

    if (cfs.rankOld >= 0) {
        // rank and inTrash were found in cache (old style), move them over to pparams

        setRank(cfs.rankOld);
        setStage(cfs.inTrashOld);
    }

    delete tpp;
    tpp = 0;
}

Thumbnail::Thumbnail (CacheManager* cm, const Glib::ustring& fname, const std::string& md5)
    : fname(fname), cachemgr(cm), ref(1), enqueueNumber(0), tpp(NULL), tagsSet(false),
      exifSet(false), iptcSet(false), paramsSet(false), defaultParamsSet(false), needsReProcessing(true),
      imageLoading(false), lastImg(NULL), initial_(true)
{


    cfs.md5 = md5;
    loadProcParams ();
    _generateThumbnailImage ();
    cfs.recentlySaved = false;

    initial_ = false;

    delete tpp;
    tpp = 0;
}

void Thumbnail::_generateThumbnailImage ()
{

    //  delete everything loaded into memory
    delete tpp;
    tpp = NULL;
    delete [] lastImg;
    lastImg = NULL;
    tw = -1;
    th = options.maxThumbnailHeight;
    imgRatio = -1.;

    // generate thumbnail image
    Glib::ustring ext = getExtension (fname);

    if (ext == "") {
        return;
    }

    cfs.supported = false;
    cfs.exifValid = false;
    cfs.timeValid = false;

    if (ext.lowercase() == "jpg" || ext.lowercase() == "jpeg") {
        infoFromImage (fname);
        tpp = rtengine::Thumbnail::loadFromImage (fname, tw, th, 1, pparams.wb.equal);

        if (tpp) {
            cfs.format = FT_Jpeg;
        }
    } else if (ext.lowercase() == "png") {
        tpp = rtengine::Thumbnail::loadFromImage (fname, tw, th, 1, pparams.wb.equal);

        if (tpp) {
            cfs.format = FT_Png;
        }
    } else if (ext.lowercase() == "tif" || ext.lowercase() == "tiff") {
        infoFromImage (fname);
        tpp = rtengine::Thumbnail::loadFromImage (fname, tw, th, 1, pparams.wb.equal);

        if (tpp) {
            cfs.format = FT_Tiff;
        }
    } else {
        // RAW works like this:
        //  1. if we are here it's because we aren't in the cache so load the JPG
        //     image out of the RAW. Mark as "quick".
        //  2. if we don't find that then just grab the real image.
        bool quick = false;
        rtengine::RawMetaDataLocation ri;

        if ( initial_ && options.internalThumbIfUntouched) {
            quick = true;
            tpp = rtengine::Thumbnail::loadQuickFromRaw (fname, ri, tw, th, 1, TRUE);
        }

        if ( tpp == NULL ) {
            quick = false;
            tpp = rtengine::Thumbnail::loadFromRaw (fname, ri, tw, th, 1, pparams.wb.equal, TRUE);
        }

        if (tpp) {
            cfs.format = FT_Raw;
            cfs.thumbImgType = quick ? CacheImageData::QUICK_THUMBNAIL : CacheImageData::FULL_THUMBNAIL;
            infoFromImage (fname, &ri);
        }
    }

    if (tpp) {
        tpp->getAutoWBMultipliers(cfs.redAWBMul, cfs.greenAWBMul, cfs.blueAWBMul);
        _saveThumbnail ();
        cfs.supported = true;
        needsReProcessing = true;

        cfs.save (getCacheFileName ("data") + ".txt");

        generateExifDateTimeStrings ();
    }
}

bool Thumbnail::isSupported ()
{
    return cfs.supported;
}

// Send back a full ProcParam object, i.e. including the Tool, Exif and IPTC part
const ProcParams& Thumbnail::getTagsParams ()
{
    MyMutex::MyLock lock(mutex);
    return getTagsParamsU();
}

// Send back a full ProcParam object, i.e. including the Tool, Exif and IPTC part
// Unprotected version of getTagParams
const ProcParams& Thumbnail::getTagsParamsU ()
{
    // set or not set, we have to return something
    return pparams;
}

// Send back a full ProcParam object, i.e. including the Tag, Exif and IPTC part
const ProcParams& Thumbnail::getToolParams ()
{
    MyMutex::MyLock lock(mutex);
    return getToolParamsU();
}

// Send back a full ProcParam object, i.e. including the Tag, Exif and IPTC part
// Unprotected version of getToolParams
const ProcParams& Thumbnail::getToolParamsU ()
{
    if (paramsSet || defaultParamsSet) {
        return pparams;
    } else {
        // We are getting the default ProcParams for this kind of file, but preserving
        // the FLAGS, EXIF and IPTC values, just in case that they have been set
        PartialProfile partProfile(profileStore.getDefaultProcParams (getType() == FT_Raw), new ParamsEdited(true));
        partProfile.pedited->set(false, ProcParams::FLAGS|ProcParams::EXIF|ProcParams::IPTC);
        partProfile.applyTo(&pparams);
        partProfile.deleteInstance();

        if (pparams.wb.method == "Camera") {
            double ct;
            getCamWB (ct, pparams.wb.green);
            pparams.wb.temperature = ct;
        } else if (pparams.wb.method == "Auto") {
            double ct;
            getAutoWB (ct, pparams.wb.green, pparams.wb.equal);
            pparams.wb.temperature = ct;
        }
        defaultParamsSet = true;
    }

    return pparams; // there is no valid pp to return, but we have to return something
}

// Send back a full ProcParam object, i.e. including the Tag, Exif and IPTC part
const ProcParams& Thumbnail::getExifParams ()
{
    MyMutex::MyLock lock(mutex);
    return getExifParamsU();
}

// Send back a full ProcParam object, i.e. including the Tag, Exif and IPTC part
// Unprotected version of getToolParams
const ProcParams& Thumbnail::getExifParamsU ()
{
    // TODO: should we handle a default Exif params in RT, like the default Proccessing params and use it here ?
    return pparams;
}

// Send back a full ProcParam object, i.e. including the Tag, Exif and IPTC part
const ProcParams& Thumbnail::getIptcParams ()
{
    MyMutex::MyLock lock(mutex);
    return getIptcParamsU();
}

// Send back a full ProcParam object, i.e. including the Tag, Exif and IPTC part
// Unprotected version of getToolParams
const ProcParams& Thumbnail::getIptcParamsU ()
{
    // TODO: should we handle a default IPTC params in RT, like the default Proccessing params and use it here ?
    return pparams;
}

/** @brief  Create default params on demand and returns a new updatable object
 *
 *  The loaded profile may be partial, but it return a complete ProcParams (i.e. without ParamsEdited)
 *
 *  @param returnParams Ask to return a pointer to a ProcParams object if true
 *  @param forceCPB True if the Custom Profile Builder has to be invoked, False if the CPB has to be invoked if the profile doesn't
 *                  exist yet. It depends on other conditions too
 *  @param flaggingMode True if the ProcParams will be created because the file browser is being flagging an image
 *                      (rank, to trash, color label). This parameter is passed to the CPB.
 *
 *  @return Return a pointer to a ProcParams structure to be updated if returnParams is true and if everything went fine, NULL otherwise.
 */
rtengine::procparams::ProcParams* Thumbnail::createProcParamsForUpdate(bool returnParams, bool forceCPB, bool flaggingMode)
{

    //static int index = 0; // Will act as unique identifier during the session

    // try to load the last saved parameters from the cache or from the paramfile file
    ProcParams* ldprof = NULL;

    Glib::ustring defProf = getType() == FT_Raw ? options.defProfRaw : options.defProfImg;

    const CacheImageData* cfs = getCacheImageData();
    Glib::ustring defaultPparamsPath = options.findProfilePath(defProf);

    if (!options.CPBPath.empty() && !defaultPparamsPath.empty() && (!hasToolParamsSet() || forceCPB) && cfs && cfs->exifValid) {
        // First generate the communication file, with general values and EXIF metadata
        rtengine::ImageMetaData* imageMetaData;

        if (getType() == FT_Raw) {
            rtengine::RawMetaDataLocation metaData = rtengine::Thumbnail::loadMetaDataFromRaw(fname);
            imageMetaData = rtengine::ImageMetaData::fromFile (fname, &metaData);
        } else {
            imageMetaData = rtengine::ImageMetaData::fromFile (fname, NULL);
        }

        Glib::ustring tmpFileName = getTempFileName() + ".txt";

        const rtexif::TagDirectory* exifDir = NULL;

        Glib::ustring outFName;
        if (imageMetaData && (exifDir = imageMetaData->getExifData())) {

            if (forceCPB) {
                // create a permanent file
                if (options.paramsLoadLocation == PLL_Input) {
                    outFName = fname + paramFileExtension;
                } else {
                    outFName = getCacheFileName("profiles") + paramFileExtension;
                }
            } else {
                // create a temporary file
                outFName = getTempFileName() + paramFileExtension;
            }

            exifDir->CPBDump(tmpFileName, fname, outFName,
                             defaultPparamsPath == DEFPROFILE_INTERNAL ? DEFPROFILE_INTERNAL : Glib::build_filename(defaultPparamsPath, Glib::path_get_basename(defProf) + paramFileExtension),
                             cfs,
                             flaggingMode);
        }

        // For the filename etc. do NOT use streams, since they are not UTF8 safe
        Glib::ustring cmdLine = options.CPBPath + Glib::ustring(" \"") + tmpFileName + Glib::ustring("\"");

        if (options.rtSettings.verbose) {
            printf("Custom profile builder's command line: %s\n", Glib::ustring(cmdLine).c_str());
        }

        bool success = safe_spawn_command_line_sync (cmdLine);

        // Now they SHOULD be there (and potentially "partial"), so try to load them and store it as a full procparam
        if (success) {
            loadProcParams(forceCPB ? "" : outFName);
        }

        // cleanup
        if (safe_file_test(tmpFileName, Glib::FILE_TEST_EXISTS )) {
            safe_g_remove (tmpFileName);
        }

        if (!forceCPB && !outFName.empty() && safe_file_test(outFName, Glib::FILE_TEST_EXISTS )) {
            safe_g_remove (outFName);
        }

        if (imageMetaData) {
            delete imageMetaData;
        }
    }

    if (returnParams && (hasToolParamsSet() || hasExifParamsSet() || hasIptcParamsSet() )) {
        ldprof = new ProcParams ();
        *ldprof = getToolParams ();
    }

    return ldprof;
}

bool Thumbnail::hasTagsSet()
{
    return tagsSet;
}
bool Thumbnail::hasToolParamsSet()
{
    return paramsSet;
}
bool Thumbnail::hasExifParamsSet()
{
    return exifSet;
}
bool Thumbnail::hasIptcParamsSet()
{
    return iptcSet;
}

void Thumbnail::notifylisterners_procParamsChanged(int whoChangedIt)
{
    for (size_t i = 0; i < listeners.size(); i++) {
        listeners[i]->procParamsChanged (this, whoChangedIt);
    }
}

/*
 * Load the procparams specified in fname. If empty, load it from the cache or from
 * the sidecar file (priority set in the Preferences).
 *
 * The result is a complete ProcParams with default values merged with the values
 * from the default Raw or Image ProcParams, then with the values from the loaded
 * ProcParams (sidecar or cache file).
 */
void Thumbnail::loadProcParams (Glib::ustring fileName)
{
    MyMutex::MyLock lock(mutex);

    bool pparamsValid;
    int ppres;
    tagsSet = paramsSet = exifSet = iptcSet = false;
    pparams.setDefaults();
    {
    const PartialProfile *defaultPP = profileStore.getDefaultPartialProfile(getType() == FT_Raw);
    defaultPP->applyTo(&pparams);
    }
    ParamsEdited pe(false);

    if (!fileName.empty()) {
        // try to load it from params file next to the image file
        ppres = pparams.load (fileName, &pe);
        pparamsValid = !ppres && pparams.ppVersion >= 220;
#ifndef NDEBUG
        printf("Thumbnail::loadProcParams / fname not empty -> loading pparams \"%s\" -> rank=%d\n", fname.c_str(), pparams.rank);
#endif
    } else if (options.paramsLoadLocation == PLL_Input) {
        // try to load it from params file next to the image file
        ppres = pparams.load (fname + paramFileExtension, &pe);
        pparamsValid = !ppres && pparams.ppVersion >= 220;
#ifndef NDEBUG
        if (pparamsValid) {
            Glib::ustring fn_ = fname + paramFileExtension;
            printf("Thumbnail::loadProcParams / fname empty -> loading pparams \"%s\" -> rank=%d\n", fn_.c_str(), pparams.rank);
        }
#endif

        // if no success, try to load the cached version of the procparams
        if (!pparamsValid) {
            pparamsValid = !pparams.load (getCacheFileName ("profiles") + paramFileExtension, &pe);
#ifndef NDEBUG
            if (pparamsValid) {
                Glib::ustring fn_ = getCacheFileName ("profiles") + paramFileExtension;
                printf("Thumbnail::loadProcParams / fname empty -> loading pparams \"%s\" -> rank=%d\n", fn_.c_str(), pparams.rank);
            }
#endif
        }
    } else {
        // try to load it from cache
        pparamsValid = !pparams.load (getCacheFileName ("profiles") + paramFileExtension, &pe);

#ifndef NDEBUG
        if (pparamsValid) {
            Glib::ustring fn_ = getCacheFileName ("profiles") + paramFileExtension;
            printf("Thumbnail::loadProcParams / fname empty -> loading pparams \"%s\" -> rank=%d\n", fn_.c_str(), pparams.rank);
        }
#endif

        // if no success, try to load it from params file next to the image file
        if (!pparamsValid) {
            ppres = pparams.load (fname + paramFileExtension, &pe);
            pparamsValid = !ppres && pparams.ppVersion >= 220;
#ifndef NDEBUG
            if (pparamsValid) {
                Glib::ustring fn_ = fname + paramFileExtension;
                printf("Thumbnail::loadProcParams / fname empty -> loading pparams \"%s\" -> rank=%d\n", fn_.c_str(), pparams.rank);
            }
#endif
        }
    }
    if (pparamsValid) {
        paramsSet = pe.isToolSet();
        tagsSet = pe.isTagsSet();
        exifSet = pe.isExifSet();
        iptcSet = pe.isIptcSet();
    }
}

void Thumbnail::clearProcParams (int ppSubPart, int whoClearedIt)
{
    /* NEW BEHAVIOR (Hombre, 2015-09-02):
     *
     * 1. ProcParams can be partially cleared. Subparts (i.e. sections) that has been cleared
     *    are removed from the pp3 file.
     * 2. The pp3 file will be physically removed only if all subparts are cleared.
     * 3. If the TOOL subpart is cleared (and in this case only), the thumbnail switch
     *    back to the embedded preview.
     */
    {
        MyMutex::MyLock lock(mutex);

        bool oldParamsSet = paramsSet;

        //TODO: run though customprofilebuilder?
        // probably not as this is the only option to set param values to default

        // reset the params to defaults
        pparams.setDefaults(ppSubPart);

        if (ppSubPart & ProcParams::FLAGS) {
            tagsSet = false;
        }
        if (ppSubPart & ProcParams::TOOL) {
            paramsSet = false;
            cfs.recentlySaved = false;
            needsReProcessing = true;
        }
        if (ppSubPart & ProcParams::EXIF) {
            exifSet = false;
        }
        if (ppSubPart & ProcParams::IPTC) {
            iptcSet = false;
        }

        if (tagsSet || paramsSet || exifSet || iptcSet) {
            // There's still something in the procparams, so we save it
            updateCache();
        } else {
            // Nothing left, we delete it

            // params could get validated by rank/inTrash values restored above
            // remove param file from cache
            Glib::ustring fname_ = getCacheFileName ("profiles") + paramFileExtension;

            if (safe_file_test (fname_, Glib::FILE_TEST_EXISTS)) {
                safe_g_remove (fname_);
            }

            // remove param file located next to the file
            //fname_ = removeExtension(fname) + paramFileExtension;
            fname_ = fname + paramFileExtension;

            if (safe_file_test(fname_, Glib::FILE_TEST_EXISTS)) {
                safe_g_remove (fname_);
            }

            // WARNING: [HOMBRE] removing IMGP1102.pp3 might be dangerous, since RT doesn't create this
            //          files anymore and since a long time now, but users can manually save with this pattern.
            //          So I'm removing this part of the code
            /*
            fname_ = removeExtension(fname) + paramFileExtension;

            if (safe_file_test (fname_, Glib::FILE_TEST_EXISTS)) {
                safe_g_remove (fname_);
            }
            */
        }

        if (oldParamsSet!=paramsSet && cfs.format == FT_Raw && options.internalThumbIfUntouched && cfs.thumbImgType != CacheImageData::QUICK_THUMBNAIL) {
            // regenerate thumbnail, ie load the quick thumb again. For the rare formats not supporting quick thumbs this will
            // be a bit slow as a new full thumbnail will be generated unnecessarily, but currently there is no way to pre-check
            // if the format supports quick thumbs.
            initial_ = true;
            _generateThumbnailImage();
            initial_ = false;
        }

    } // end of mutex lock

    for (size_t i = 0; i < listeners.size(); i++) {
        listeners[i]->procParamsChanged (this, whoClearedIt);
    }
}

void Thumbnail::setProcParams (const ProcParams& pp, ParamsEdited* pe, int whoChangedIt, bool updateCacheNow)
{

    {
        MyMutex::MyLock lock(mutex);

        if (pparams.sharpening.threshold.isDouble() != pp.sharpening.threshold.isDouble()) {
            printf("WARNING: Sharpening different!\n");
        }

        if (pparams.vibrance.psthreshold.isDouble() != pp.vibrance.psthreshold.isDouble()) {
            printf("WARNING: Vibrance different!\n");
        }

        if (pparams != pp) {
            cfs.recentlySaved = false;
        }

        // do not update rank, colorlabel and inTrash
        int rank = getRank();
        int colorlabel = getColorLabel();
        int inTrash = getStage();

        if (pe) {
            pe->combine(pparams, pp, true);
            paramsSet = pe->isToolSet();
            exifSet = pe->isExifSet();
            iptcSet = pe->isIptcSet();
            printf("Thumbnail::setProcParams / file %s / PE / isToolSet=%d / rank=%d\n", fname.c_str(), paramsSet, pparams.rank);
        } else {
            pparams = pp;
            paramsSet = exifSet = iptcSet = true;
            printf("Thumbnail::setProcParams / file %s / isToolSet=%d / rank=%d\n", fname.c_str(), paramsSet, pparams.rank);
        }

        needsReProcessing = true;

        if (tagsSet) {
            // restore tags
            setRank(rank);
            setColorLabel(colorlabel);
            setStage(inTrash);
            printf("Thumbnail::setProcParams / %s / Reset flags where rank=%d, colorLabel=%d, inTrash=%d\n", fname.c_str(), rank, colorlabel, inTrash);
        }
        else {
            // May have been set by combine, so we restore it to default
            pparams.setDefaults(ProcParams::FLAGS);
            printf("Thumbnail::setProcParams / %s / Reset rank to default\n", fname.c_str());
        }

        if (updateCacheNow) {
            printf("Thumbnail::setProcParams / %s / after updateCache, rank=%d\n", fname.c_str(), getRank());
            updateCache ();
        }

    } // end of mutex lock

    for (size_t i = 0; i < listeners.size(); i++) {
        listeners[i]->procParamsChanged (this, whoChangedIt);
    }
    printf("Thumbnail::setProcParams / %s / after listeners, rank=%d\n", fname.c_str(), getRank());
}

bool Thumbnail::isRecentlySaved ()
{

    return cfs.recentlySaved;
}

void Thumbnail::imageDeveloped ()
{

    cfs.recentlySaved = true;
    cfs.save (getCacheFileName ("data") + ".txt");

    if (options.saveParamsCache) {
        pparams.save (getCacheFileName ("profiles") + paramFileExtension);
    }
}

void Thumbnail::imageEnqueued ()
{

    enqueueNumber++;
}

void Thumbnail::imageRemovedFromQueue ()
{

    enqueueNumber--;
}

bool Thumbnail::isEnqueued ()
{

    return enqueueNumber > 0;
}

void Thumbnail::increaseRef ()
{
    MyMutex::MyLock lock(mutex);
    ++ref;
}

void Thumbnail::decreaseRef ()
{
    {
        MyMutex::MyLock lock(mutex);

        if ( ref == 0 ) {
            return;
        }

        if ( --ref != 0 ) {
            return;
        }
    }
    cachemgr->closeThumbnail (this);
}

void Thumbnail::getThumbnailSize (int &w, int &h, const rtengine::procparams::ProcParams *pparams)
{
    int tw_ = tw;
    int th_ = th;
    float imgRatio_ = imgRatio;

    if (pparams) {
        int ppCoarse = pparams->coarse.rotate;

        if (ppCoarse >= 180) {
            ppCoarse -= 180;
        }

        int thisCoarse = this->pparams.coarse.rotate;

        if (thisCoarse >= 180) {
            thisCoarse -= 180;
        }

        if (thisCoarse != ppCoarse) {
            // different orientation -> swapping width & height
            int tmp = th_;
            th_ = tw_;
            tw_ = tmp;

            if (imgRatio_ >= 0.0001f) {
                imgRatio_ = 1.f / imgRatio_;
            }
        }
    }

    if (imgRatio_ > 0.) {
        w = (int)(imgRatio_ * (float)h);
    } else {
        w = tw_ * h / th_;
    }
}

void Thumbnail::getFinalSize (const rtengine::procparams::ProcParams& pparams, int& w, int& h)
{
    MyMutex::MyLock lock(mutex);

    // WARNING: When downscaled, the ratio have loosed a lot of precision, so we can't get back the exact initial dimensions
    double fw = lastW * lastScale;
    double fh = lastH * lastScale;

    if (pparams.coarse.rotate == 90 || pparams.coarse.rotate == 270) {
        fh = lastW * lastScale;
        fw = lastH * lastScale;
    }

    if (!pparams.resize.enabled) {
        w = fw;
        h = fh;
    } else {
        w = (int)(fw + 0.5);
        h = (int)(fh + 0.5);
    }
}


rtengine::IImage8* Thumbnail::processThumbImage (const rtengine::procparams::ProcParams& pparams, int h, double& scale)
{

    MyMutex::MyLock lock(mutex);

    if ( tpp == 0 ) {
        _loadThumbnail();

        if ( tpp == 0 ) {
            return 0;
        }
    }

    rtengine::IImage8* image = 0;

    if ( cfs.thumbImgType == CacheImageData::QUICK_THUMBNAIL ) {
        // RAW internal thumbnail, no profile yet: just do some rotation etc.
        image = tpp->quickProcessImage (pparams, h, rtengine::TI_Nearest, scale);
    } else {
        // Full thumbnail: apply profile
        image = tpp->processImage (pparams, h, rtengine::TI_Bilinear, cfs.getCamera(), cfs.focalLen, cfs.focalLen35mm, cfs.focusDist, cfs.shutter, cfs.fnumber, cfs.iso, cfs.expcomp, scale );
    }

    tpp->getDimensions(lastW, lastH, lastScale);

    delete tpp;
    tpp = 0;
    return image;
}

rtengine::IImage8* Thumbnail::upgradeThumbImage (const rtengine::procparams::ProcParams& pparams, int h, double& scale)
{

    MyMutex::MyLock lock(mutex);

    if ( cfs.thumbImgType != CacheImageData::QUICK_THUMBNAIL ) {
        return 0;
    }

    _generateThumbnailImage();

    if ( tpp == 0 ) {
        return 0;
    }

    rtengine::IImage8* image = tpp->processImage (pparams, h, rtengine::TI_Bilinear, cfs.getCamera(), cfs.focalLen, cfs.focalLen35mm, cfs.focusDist, cfs.shutter, cfs.fnumber, cfs.iso, cfs.expcomp,  scale );
    tpp->getDimensions(lastW, lastH, lastScale);

    delete tpp;
    tpp = 0;
    return image;
}

void Thumbnail::generateExifDateTimeStrings ()
{

    exifString = "";
    dateTimeString = "";

    if (!cfs.exifValid) {
        return;
    }

    exifString = Glib::ustring::compose ("f/%1 %2s %3%4 %5mm", Glib::ustring(rtengine::ImageData::apertureToString(cfs.fnumber)), Glib::ustring(rtengine::ImageData::shutterToString(cfs.shutter)), M("QINFO_ISO"), cfs.iso, Glib::ustring::format(std::setw(3), std::fixed, std::setprecision(2), cfs.focalLen));

    if (options.fbShowExpComp && cfs.expcomp != "0.00" && cfs.expcomp != "") { // don't show exposure compensation if it is 0.00EV;old cache iles do not have ExpComp, so value will not be displayed.
        exifString = Glib::ustring::compose ("%1 %2EV", exifString, cfs.expcomp);    // append exposure compensation to exifString
    }

    std::string dateFormat = options.dateFormat;
    std::ostringstream ostr;
    bool spec = false;

    for (size_t i = 0; i < dateFormat.size(); i++)
        if (spec && dateFormat[i] == 'y') {
            ostr << cfs.year;
            spec = false;
        } else if (spec && dateFormat[i] == 'm') {
            ostr << (int)cfs.month;
            spec = false;
        } else if (spec && dateFormat[i] == 'd') {
            ostr << (int)cfs.day;
            spec = false;
        } else if (dateFormat[i] == '%') {
            spec = true;
        } else {
            ostr << (char)dateFormat[i];
            spec = false;
        }

    ostr << " " << (int)cfs.hour;
    ostr << ":" << std::setw(2) << std::setfill('0') << (int)cfs.min;
    ostr << ":" << std::setw(2) << std::setfill('0') << (int)cfs.sec;

    dateTimeString = ostr.str ();
}

const Glib::ustring& Thumbnail::getExifString ()
{

    return exifString;
}

const Glib::ustring& Thumbnail::getDateTimeString ()
{

    return dateTimeString;
}

void Thumbnail::getAutoWB (double& temp, double& green, double equal)
{
    if (cfs.redAWBMul != -1.0) {
        rtengine::ColorTemp ct(cfs.redAWBMul, cfs.greenAWBMul, cfs.blueAWBMul, equal);
        temp = ct.getTemp();
        green = ct.getGreen();
    } else {
        temp = green = -1.0;
    }
}


ThFileType Thumbnail::getType ()
{

    return (ThFileType) cfs.format;
}

int Thumbnail::infoFromImage (const Glib::ustring& fname, rtengine::RawMetaDataLocation* rml)
{

    rtengine::ImageMetaData* idata = rtengine::ImageMetaData::fromFile (fname, rml);

    if (!idata) {
        return 0;
    }

    int deg = 0;
    cfs.timeValid = false;
    cfs.exifValid = false;

    if (idata->hasExif()) {
        cfs.shutter  = idata->getShutterSpeed ();
        cfs.fnumber  = idata->getFNumber ();
        cfs.focalLen = idata->getFocalLen ();
        cfs.focalLen35mm = idata->getFocalLen35mm ();
        cfs.focusDist = idata->getFocusDist ();
        cfs.iso      = idata->getISOSpeed ();
        cfs.expcomp  = idata->expcompToString (idata->getExpComp(), false); // do not mask Zero expcomp
        cfs.year     = 1900 + idata->getDateTime().tm_year;
        cfs.month    = idata->getDateTime().tm_mon + 1;
        cfs.day      = idata->getDateTime().tm_mday;
        cfs.hour     = idata->getDateTime().tm_hour;
        cfs.min      = idata->getDateTime().tm_min;
        cfs.sec      = idata->getDateTime().tm_sec;
        cfs.timeValid = true;
        cfs.exifValid = true;
        cfs.lens      = idata->getLens();
        cfs.camMake   = idata->getMake();
        cfs.camModel  = idata->getModel();

        if (idata->getOrientation() == "Rotate 90 CW") {
            deg = 90;
        } else if (idata->getOrientation() == "Rotate 180") {
            deg = 180;
        } else if (idata->getOrientation() == "Rotate 270 CW") {
            deg = 270;
        }
    } else {
        cfs.lens     = "Unknown";
        cfs.camMake  = "Unknown";
        cfs.camModel = "Unknown";
    }

    // get image filetype
    std::string::size_type idx;
    idx = fname.rfind('.');

    if(idx != std::string::npos) {
        cfs.filetype = fname.substr(idx + 1);
    } else {
        cfs.filetype = "";
    }

    delete idata;
    return deg;
}

/*
 * Read all thumbnail's data from the cache; build and save them if doesn't exist - NON PROTECTED
 * This includes:
 *  - image's bitmap (*.rtti)
 *  - auto exposure's histogram (full thumbnail only)
 *  - embedded profile (full thumbnail only)
 *  - LiveThumbData section of the data file
 */
void Thumbnail::_loadThumbnail(bool firstTrial)
{

    needsReProcessing = true;
    tw = -1;
    th = options.maxThumbnailHeight;
    delete tpp;
    tpp = new rtengine::Thumbnail ();
    tpp->isRaw = (cfs.format == (int) FT_Raw);

    // load supplementary data
    bool succ = tpp->readData (getCacheFileName ("data") + ".txt");

    if (succ) {
        tpp->getAutoWBMultipliers(cfs.redAWBMul, cfs.greenAWBMul, cfs.blueAWBMul);
    }

    // thumbnail image
    succ = succ && tpp->readImage (getCacheFileName ("images"));

    if (!succ && firstTrial) {
        _generateThumbnailImage ();

        if (cfs.supported && firstTrial) {
            _loadThumbnail (false);
        }

        if (tpp == NULL) {
            return;
        }
    } else if (!succ) {
        delete tpp;
        tpp = NULL;
        return;
    }

    if ( cfs.thumbImgType == CacheImageData::FULL_THUMBNAIL ) {
        // load aehistogram
        tpp->readAEHistogram (getCacheFileName ("aehistograms"));

        // load embedded profile
        tpp->readEmbProfile (getCacheFileName ("embprofiles") + ".icc");

        tpp->init ();
    }

    if (!initial_ && tpp) {
        tw = tpp->getImageWidth (getToolParamsU(), th, imgRatio);    // this might return 0 if image was just building
    }
}

/*
 * Read all thumbnail's data from the cache; build and save them if doesn't exist - MUTEX PROTECTED
 * This includes:
 *  - image's bitmap (*.rtti)
 *  - auto exposure's histogram (full thumbnail only)
 *  - embedded profile (full thumbnail only)
 *  - LiveThumbData section of the data file
 */
void Thumbnail::loadThumbnail (bool firstTrial)
{
    MyMutex::MyLock lock(mutex);
    _loadThumbnail(firstTrial);
}

/*
 * Save thumbnail's data to the cache - NON PROTECTED
 * This includes:
 *  - image's bitmap (*.rtti)
 *  - auto exposure's histogram (full thumbnail only)
 *  - embedded profile (full thumbnail only)
 *  - LiveThumbData section of the data file
 */
void Thumbnail::_saveThumbnail ()
{

    if (!tpp) {
        return;
    }

    if (safe_g_remove (getCacheFileName ("images") + ".rtti") == -1) {
        // No file deleted, so we try to deleted obsolete files, if any
        safe_g_remove (getCacheFileName ("images") + ".cust");
        safe_g_remove (getCacheFileName ("images") + ".cust16");
        safe_g_remove (getCacheFileName ("images") + ".jpg");
    }

    // save thumbnail image
    tpp->writeImage (getCacheFileName ("images"), 1);

    // save aehistogram
    tpp->writeAEHistogram (getCacheFileName ("aehistograms"));

    // save embedded profile
    tpp->writeEmbProfile (getCacheFileName ("embprofiles") + ".icc");

    // save supplementary data
    tpp->writeData (getCacheFileName ("data") + ".txt");
}

/*
 * Save thumbnail's data to the cache - MUTEX PROTECTED
 * This includes:
 *  - image's bitmap (*.rtti)
 *  - auto exposure's histogram (full thumbnail only)
 *  - embedded profile (full thumbnail only)
 *  - LiveThumbData section of the data file
 */
void Thumbnail::saveThumbnail ()
{
    MyMutex::MyLock lock(mutex);
    _saveThumbnail();
}

/*
 * Update the cached files
 *  - updatePParams==true (default)        : write the procparams file (sidecar or cache, depending on the options)
 *  - updateCacheImageData==true (default) : write the CacheImageData values in the cache folder,
 *                                           i.e. some General, DateTime, ExifInfo, File info and ExtraRawInfo,
 */
void Thumbnail::updateCache (bool updatePParams, bool updateCacheImageData)
{
    printf("Thumbnail::updateCache\n");
    ParamsEdited pe(paramsSet);  // set to false by default in the constructor
    // Now the the minor sub-parts of the ParamsEdited
    if (tagsSet != paramsSet) {
        pe.set(tagsSet, ProcParams::FLAGS);
    }
    if (exifSet != paramsSet) {
        pe.set(exifSet, ProcParams::EXIF);
    }
    if (iptcSet != paramsSet) {
        pe.set(iptcSet, ProcParams::IPTC);
    }

    if (updatePParams) {
        // if tagsSet, paramsSet, exifSet and iptcSet are all false, the procparams will only contain RT's version !
        Glib::ustring file1(options.saveParamsFile  ? fname + paramFileExtension : "");
        Glib::ustring file2(options.saveParamsCache ? getCacheFileName ("profiles") + paramFileExtension : "");
        pparams.save (file1, file2, true, &pe);
    }

    if (updateCacheImageData) {
        cfs.save (getCacheFileName ("data") + ".txt");
    }
}

Thumbnail::~Thumbnail ()
{
    mutex.lock();

    delete [] lastImg;
    delete tpp;
    mutex.unlock();
}

Glib::ustring Thumbnail::getCacheFileName (Glib::ustring subdir)
{

    return cachemgr->getCacheFileName (subdir, fname, cfs.md5);
}

Glib::ustring Thumbnail::getTempFileName ()
{

    return cachemgr->getTempFileNameSmall (Glib::path_get_basename (fname) + "." + cfs.md5);
}

void Thumbnail::setFileName (const Glib::ustring fn)
{

    fname = fn;
    cfs.md5 = cachemgr->getMD5 (fname);
}

void Thumbnail::addThumbnailListener (ThumbnailListener* tnl)
{

    increaseRef();
    listeners.push_back (tnl);
}

void Thumbnail::removeThumbnailListener (ThumbnailListener* tnl)
{

    std::vector<ThumbnailListener*>::iterator f = std::find (listeners.begin(), listeners.end(), tnl);

    if (f != listeners.end()) {
        listeners.erase (f);
        decreaseRef();
    }
}

// Calculates the standard filename for the automatically named batch result
// and opens it in OS default viewer
// destination: 1=Batch conf. file; 2=batch out dir; 3=RAW dir
// Return: Success?
bool Thumbnail::openDefaultViewer(int destination)
{

#ifdef WIN32
    Glib::ustring openFName;

    if (destination == 1) {
        openFName = Glib::ustring::compose ("%1.%2", BatchQueue::calcAutoFileNameBase(fname), options.saveFormatBatch.format);

        if (safe_file_test (openFName, Glib::FILE_TEST_EXISTS)) {
            wchar_t *wfilename = (wchar_t*)g_utf8_to_utf16 (openFName.c_str(), -1, NULL, NULL, NULL);
            ShellExecuteW(NULL, L"open", wfilename, NULL, NULL, SW_SHOWMAXIMIZED );
            g_free(wfilename);
        } else {
            printf("%s not found\n", openFName.data());
            return false;
        }
    } else {
        openFName = destination == 3 ? fname
                    : Glib::ustring::compose ("%1.%2", BatchQueue::calcAutoFileNameBase(fname), options.saveFormatBatch.format);

        printf("Opening %s\n", openFName.c_str());

        if (safe_file_test (openFName, Glib::FILE_TEST_EXISTS)) {
            // Output file exists, so open explorer and select output file
            wchar_t* org = (wchar_t*)g_utf8_to_utf16 (Glib::ustring::compose("/select,\"%1\"", openFName).c_str(), -1, NULL, NULL, NULL);
            wchar_t* par = new wchar_t[wcslen(org) + 1];
            wcscpy(par, org);

            // In this case the / disturbs
            wchar_t* p = par + 1; // skip the first backslash

            while (*p != 0) {
                if (*p == L'/') {
                    *p = L'\\';
                }

                p++;
            }

            ShellExecuteW(NULL, L"open", L"explorer.exe", par, NULL, SW_SHOWNORMAL );

            delete[] par;
            g_free(org);
        } else if (safe_file_test (Glib::path_get_dirname(openFName), Glib::FILE_TEST_EXISTS)) {
            // Out file does not exist, but directory
            wchar_t *wfilename = (wchar_t*)g_utf8_to_utf16 (Glib::path_get_dirname(openFName).c_str(), -1, NULL, NULL, NULL);
            ShellExecuteW(NULL, L"explore", wfilename, NULL, NULL, SW_SHOWNORMAL );
            g_free(wfilename);
        } else {
            printf("File and dir not found\n");
            return false;
        }
    }

    return true;

#else
    // TODO: Add more OSes here
    printf("Automatic opening not supported on this OS\n");
    return false;
#endif

}

bool Thumbnail::imageLoad(bool loading)
{
    MyMutex::MyLock lock(mutex);
    bool previous = imageLoading;

    if( loading && !previous ) {
        imageLoading = true;
        return true;
    } else if( !loading ) {
        imageLoading = false;
    }

    return false;
}
