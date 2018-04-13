/*
 * * Copyright (C) 2018 Intel Corporation. All Rights Reserved.
 * *
 ** Permission is hereby granted, free of charge, to any person obtaining a
 * * copy of this software and associated documentation files (the
 * * "Software"), to deal in the Software without restriction, including
 * * without limitation the rights to use, copy, modify, merge, publish,
 * * distribute, sub license, and/or sell copies of the Software, and to
 * * permit persons to whom the Software is furnished to do so, subject to
 * * the following conditions:
 * *
 * * The above copyright notice and this permission notice (including the
 * * next paragraph) shall be included in all copies or substantial portions
 * * of the Software.
 * *
 * * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * */


/**
 * @file VDecAccelVA.cpp
 * @brief LibVA decode accelerator implementation.
 */

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <assert.h>
#include <algorithm>
#include <thread>
#include <chrono>
#include <string.h>
#include "VDecAccelVA.h"
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_x11.h>

#define VASUCCEEDED(err)    (err == VA_STATUS_SUCCESS)
#define VAFAILED(err)       (err != VA_STATUS_SUCCESS)

#define VA_SURF_ERR_MB_RANGE  1
#define VA_SURF_ERR_MB_NUMBER 2
#define VA_SURF_ERR_INVALID   -1

#define VACONTEXTID_CLEAR         0x0FFFFFFF
#define VACONTEXTID_DECODER       0x10000000
#define VACONTEXTID_ENCODER       0x20000000
#define VACONTEXTID_HUC           0x30000000

mvaccel::VDecAccelVAImpl::VDecAccelVAImpl(void* device)
    : m_vaDisplay(0)
    , m_vaProfile(VAProfileNone)
    , m_vaEntrypoint(VAEntrypointVLD)
    , m_vaConfigID(0)
    , m_vaContextID(0)
    , m_surfaceType(VA_RT_FORMAT_YUV420)
{
    if (device)
        m_vaDisplay = *(reinterpret_cast<VADisplay*>(device));

    if (!m_vaDisplay)
    {
        printf("Invalid VADisplay\n");
        delete this;
        return;
    }
}

mvaccel::VDecAccelVAImpl::VDecAccelVAImpl()
    : m_vaDisplay(0)
    , m_vaProfile(VAProfileNone)
    , m_vaEntrypoint(VAEntrypointVLD)
    , m_vaConfigID(0)
    , m_vaContextID(0)
    , m_surfaceType(VA_RT_FORMAT_YUV420)
{
}

mvaccel::VDecAccelVAImpl::~VDecAccelVAImpl()
{
}

//////////////////////////////////////////////////////////////////////////
// VDecAccel interface
//////////////////////////////////////////////////////////////////////////

int mvaccel::VDecAccelVAImpl::Open()
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    //get display device
    int MajorVer, MinorVer;
    Display*    g_pDisplay = 0;
    g_pDisplay = XOpenDisplay(":0.0");

    int drm_fd = -1;

    if (g_pDisplay != NULL) {
        m_vaDisplay = vaGetDisplay(g_pDisplay);
        vaInitialize(m_vaDisplay, &MajorVer, &MinorVer);
    }

    if (vaStatus != VA_STATUS_SUCCESS) {
        drm_fd = open("/dev/dri/card0", O_RDWR);
        if (drm_fd >= 0) {
            m_vaDisplay = vaGetDisplayDRM(drm_fd);
            vaStatus = vaInitialize(m_vaDisplay, &MajorVer, &MinorVer);
        }
    }

    //initialize decode description
    create_decode_desc();

    m_vaProfile = VAProfileH264Main;

    // We only support VLD currently
    m_vaEntrypoint = VAEntrypointVLD;

    int count = 0;
    count = vaMaxNumEntrypoints(m_vaDisplay);
    assert(count);

    std::vector<VAEntrypoint> vaEntrypoints(count);
    vaStatus = vaQueryConfigEntrypoints(
        m_vaDisplay,
        m_vaProfile,
        &vaEntrypoints[0],
        &count);
    if(VAFAILED(vaStatus))
            printf("vaQueryConfigEntrypoints fail\n");

    auto it = std::find(vaEntrypoints.begin(), vaEntrypoints.end(), m_vaEntrypoint);
    if (it == vaEntrypoints.end())
    {
        if(VAFAILED(vaStatus))
            printf("VAEntrypoint is not found\n");
        return 1;
    }

    if (!is_config_compatible(m_DecodeDesc))
    {
        if(VAFAILED(vaStatus))
            printf("Decode configuration is not compatible\n");
        return 1;
    }

    // Setup config attributes
    std::vector<VAConfigAttrib> vaAttribs;
    prepare_config_attribs(m_DecodeDesc, vaAttribs);
    // Create config
    vaStatus = vaCreateConfig(
        m_vaDisplay,
        m_vaProfile,
        m_vaEntrypoint,
        &vaAttribs.at(0),
        vaAttribs.size(),
        &m_vaConfigID
        );
    if(VAFAILED(vaStatus))
            printf("vaCreateConfig fail\n");

    if (!is_rt_foramt_supported(m_DecodeDesc))
    {
        if(VAFAILED(vaStatus))
            printf("Render target is not supported\n");
        return 1;
    }

    // Calculate aligned width/height for gfx surface
    uint32_t aligned_width  = m_DecodeDesc.width;
    uint32_t aligned_height = m_DecodeDesc.height;

    // Setup surface attributes
    prepare_surface_attribs(m_DecodeDesc, m_vaSurfAttribs, false);
    // Create surfaces
    for (uint32_t i=0; i<m_DecodeDesc.surfaces_num; i++)
    {
        VASurfaceID vaID = VA_INVALID_SURFACE;
        vaStatus = create_surface(
            aligned_width,
            aligned_height,
            m_vaSurfAttribs,
            vaID);

        if (VASUCCEEDED(vaStatus))
            m_vaIDs.push_back(vaID);
    }

    // Check if surfaces created is equal to requested.
    if (m_vaIDs.size() != m_DecodeDesc.surfaces_num)
    {
        if(VAFAILED(vaStatus))
            printf("Create surface fail\n");
        return 1;
    }

    // Create context
    vaStatus = vaCreateContext(
        m_vaDisplay,
        m_vaConfigID,
        aligned_width,
        aligned_height,
        VA_PROGRESSIVE,
        &(m_vaIDs.at(0)),
        m_vaIDs.size(),
        &m_vaContextID
        );
    if(VAFAILED(vaStatus))
            printf("vaCreateContext fail\n");

    check_process_pipeline_caps(m_DecodeDesc);

    return vaStatus;
}


void mvaccel::VDecAccelVAImpl::Close()
{
    for (auto it = m_vaIDs.begin(); it != m_vaIDs.end(); ++it)
        delete_surface(*it);

    vaDestroyConfig(m_vaDisplay, m_vaConfigID);
    vaDestroyContext(m_vaDisplay, m_vaContextID);

    //m_locks.clear();
    m_images.clear();
    m_vaIDs.clear();
    m_vaSurfAttribs.clear();

    m_vaConfigID = 0;
    m_vaContextID = 0;
}


uint32_t mvaccel::VDecAccelVAImpl::GetSurfaceID(uint32_t index)
{
    assert(index < m_vaIDs.size());
    return m_vaIDs[index];
}

//////////////////////////////////////////////////////////////////////////
// member functions
//////////////////////////////////////////////////////////////////////////

/**
 * @brief   Check if video decode acceleration description is supported.
 * @param   cc Video decode acceleration description.
 * @return  true if supported, false if not.
 */
bool mvaccel::VDecAccelVAImpl::is_config_compatible(DecodeDesc& desc)
{
    if (!is_slice_mode_supported(desc))
        return false;

    if (!is_encryption_supported(desc))
        return false;

    if(!is_sfc_config_supported(desc))
        return false;

    return true;
}

/**
 * @brief   Check if long or short format is supported not not.
 * @param   cc Video decode acceleration description.
 * @return  true if supported, false if not.
 */
bool mvaccel::VDecAccelVAImpl::is_slice_mode_supported(DecodeDesc& desc)
{
    VAConfigAttrib vaAttrib;
    memset(&vaAttrib, 0, sizeof(vaAttrib));
    vaAttrib.type = VAConfigAttribDecSliceMode;
    vaAttrib.value = 0;

    vaGetConfigAttributes(
        m_vaDisplay,
        m_vaProfile,
        m_vaEntrypoint,
        &vaAttrib,
        1);

        return true;
}

/**
 * @brief   Check if encryption is supported or not.
 * @param   cc Video decode acceleration description.
 * @return  true if supported, false if not.
 */
bool mvaccel::VDecAccelVAImpl::is_encryption_supported(DecodeDesc& desc)
{
    VAConfigAttrib vaAttrib;
    memset(&vaAttrib, 0, sizeof(vaAttrib));
    vaAttrib.type = VAConfigAttribEncryption;
    vaAttrib.value = 0;

    vaGetConfigAttributes(
        m_vaDisplay,
        m_vaProfile,
        m_vaEntrypoint,
        &vaAttrib,
        1
        );

    return true;
}

/**
 * @brief   Check if SFC attribute is supported or not.
 * @param   cc Video decode acceleration description.
 * @return  true if supported, false if not.
 */
bool mvaccel::VDecAccelVAImpl::is_sfc_config_supported(DecodeDesc& desc)
{
    // SFC attribute check
    VAConfigAttrib vaAttrib;
    memset(&vaAttrib, 0, sizeof(vaAttrib));
    vaAttrib.type = VAConfigAttribDecProcessing;
    vaAttrib.value = 0;

    vaGetConfigAttributes(
        m_vaDisplay,
        m_vaProfile,
        m_vaEntrypoint,
        &vaAttrib,
        1);

    if (vaAttrib.value != VA_DEC_PROCESSING)
        return false;

    return true;
}


/**
 * @brief   Check if render target format is supported or not.
 * @param   cc Video decode acceleration description.
 * @return  true if supported, false if not.
 */
bool mvaccel::VDecAccelVAImpl::is_rt_foramt_supported(DecodeDesc& desc)
{
    uint32_t count = VASurfaceAttribCount + vaMaxNumImageFormats(m_vaDisplay);
    std::vector<VASurfaceAttrib> attribs(count);
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    vaStatus = vaQuerySurfaceAttributes(
        m_vaDisplay,
        m_vaConfigID,
        &attribs.at(0),
        &count
        );
    if (VAFAILED(vaStatus))
    {
        printf("vaQuerySurfaceAttributes failed\n");
        return false;
    }

    return true;
}

/**
 * @brief   Prepare config attribs VAContext creation.
 * @param   desc Video decode acceleration description.
 * @param   vaAttribs Array of VASurfaceAttrib which will contains the attrib.
 */
void mvaccel::VDecAccelVAImpl::prepare_config_attribs(
    DecodeDesc& desc,
    VAConfigAttribArray& attribs)
{
    VAConfigAttrib attrib;
    memset(&attrib, 0, sizeof(attrib));

    // RT formats
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420;
    attribs.push_back(attrib);

    // Slice Mode
    attrib.type = VAConfigAttribDecSliceMode;
    attrib.value = VA_DEC_SLICE_MODE_NORMAL;
    attribs.push_back(attrib);

    //dec processing attribs
    attrib.type = VAConfigAttribDecProcessing;
    attrib.value = VA_DEC_PROCESSING;
    attribs.push_back(attrib);
}

/**
 * @brief   Prepare the VA surface attribs for creation.
 * @param   desc Video decode acceleration description.
 * @param   vaSurfAttribs Array of VASurfaceAttrib which will contains attrib.
 */
void mvaccel::VDecAccelVAImpl::prepare_surface_attribs(
    DecodeDesc& desc,
    VASurfaceAttribArray& attribs,
    bool bDecodeDownsamplingHinted)
{
    VASurfaceAttrib attrib;
    memset(&attrib, 0, sizeof(attrib));

    attrib.type = VASurfaceAttribPixelFormat;
    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.value.type = VAGenericValueTypeInteger;

    // VA_FOURCC and MVFOURCC are interchangeable
    if(bDecodeDownsamplingHinted)
        attrib.value.value.i = VA_FOURCC_ARGB;
    else
        attrib.value.value.i = VA_FOURCC_NV12;

    attribs.push_back(attrib);
}

/**
 * @brief   Create surface for decode output
 * @param   width Width of the surface
 * @param   height Height of the surface
 * @param   vaAttribs VA surface attributes
 * @param   surfaceID Surface IDs of vaapi and gralloc for allocated surface.
 * @return  VAStatus
 */
VAStatus mvaccel::VDecAccelVAImpl::create_surface(
    uint32_t width,
    uint32_t height,
    std::vector<VASurfaceAttrib>& attribs,
    VASurfaceID& vaID)
{
    VASurfaceAttribExternalBuffers extBuf = {0};

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    vaStatus = vaCreateSurfaces(
        m_vaDisplay,
        m_surfaceType,
        width,
        height,
        &vaID,
        1,
        &(attribs.at(0)),
        attribs.size()
        );

    return vaStatus;
}

/**
 * @brief   Delete allocated surface
 * @param   surfaceID Index of allocated surface. After delete, the surface
 *          values will be set to invalid value.
 */

void mvaccel::VDecAccelVAImpl::delete_surface(VASurfaceID& vaID)
{
    // Make sure no others is using this surface
    //m_locks[vaID].lock();
    //m_locks[vaID].unlock();
    //m_locks.erase(m_locks.find(vaID));
    if (m_images.count(vaID))
        m_images.erase(m_images.find(vaID));

    vaDestroySurfaces(m_vaDisplay, &vaID, 1);
    vaID = VA_INVALID_SURFACE;
}

uint8_t* mvaccel::VDecAccelVAImpl::lock_surface(VASurfaceID id, bool write)
{
    // Check if decode is completed
    VAStatus status = vaSyncSurface(m_vaDisplay, id);
    if (VAFAILED(status))
    {
        using namespace std;
        if (status == VA_STATUS_ERROR_OPERATION_FAILED)
        {
            cout << "*** vaSyncSurface return VA_STATUS_ERROR_OPERATION_FAILED ***" << endl;
            return nullptr;
        }
        else if (status == VA_STATUS_ERROR_DECODING_ERROR)
        {
            cout << "*** vaSyncSurface return VA_STATUS_ERROR_DECODING_ERROR ***" << endl;

            VASurfaceDecodeMBErrors *error_info = nullptr;
            status = vaQuerySurfaceError(m_vaDisplay, id, 0, (void **)&error_info);
            if (VAFAILED(status) || (error_info == nullptr))
            {
                cout << "*** vaQuerySurfaceError failed - ";
                if (VAFAILED(status))
                {
                    cout << "return value is "<< status << endl;
                }
                else
                {
                    cout << "error_info queried is NULL." << endl;
                }
                return nullptr;
            }

            // vaQuerySurfaceError has a valid error report, print out the report
            static std::map<int, std::string> err_status =
            {
                {VA_SURF_ERR_INVALID,   "invalid record"},
                {VA_SURF_ERR_MB_RANGE,  "start_mb/end_mb with errors is returned"},
                {VA_SURF_ERR_MB_NUMBER, "num_mb with errors is returned"},
            };
            static std::map<int, std::string> err_type =
            {
                {VADecodeSliceMissing, "VADecodeSliceMissing"},
                {VADecodeMBError,      "VADecodeMBError"},
            };
            auto e = error_info;

            cout << "*** Surface Error report start >>>>>" << endl;

            cout << "status: " << (err_status.count(e->status) ?
                                   err_status[e->status]       :
                                   "Unknown: " + e->status     ) << endl;
            cout << "type:   " << (err_type.count(e->decode_error_type) ?
                                   err_type[e->decode_error_type]       :
                                   "Unknown: " + e->decode_error_type   ) << endl;
                                  // + std::string((int)e->decode_error_type)
            cout << "MBs (start/end/num): " << e->start_mb << "/"
                                            << e->end_mb   << "/"
                                            << e->num_mb   << endl;

            cout << "*** Surface Error report end <<<<<<<" << endl;

            // hw decode continues with error report, don't think it's a fatal error.
            // so continue without nullptr return
        }
        else
        {
            return nullptr;
        }
    }

    VASurfaceStatus surf_status = VASurfaceSkipped;
    for(;;)
    {
        vaQuerySurfaceStatus(m_vaDisplay, id, &surf_status);
        if (surf_status != VASurfaceRendering &&
            surf_status != VASurfaceDisplaying)
            break;
    }

    if (surf_status != VASurfaceReady)
    {
        //VAWARN(status, "Surface is not ready by vaQueryStatusSurface");
        return nullptr;
    }

    m_locks[id].lock();
    uint8_t* buffer = nullptr;

    for(;;)
    {
        status = vaDeriveImage(m_vaDisplay, id, &m_images[id]);
        //BREAK_WARNING(status, "vaDeriveImage fail");

        status = vaMapBuffer(m_vaDisplay, m_images[id].buf, (void**)&buffer);
        //BREAK_WARNING(status, "vaDeriveImage fail");

        break;
    }

    if (VAFAILED(status))
    {
        status = vaUnmapBuffer(m_vaDisplay, m_images[id].buf);
        status = vaDestroyImage(m_vaDisplay, m_images[id].image_id);
        m_locks[id].unlock();
    }
    return buffer;
}



void mvaccel::VDecAccelVAImpl::unlock_surface(VASurfaceID id)
{
    // if mutex is lockable, then unlock is needed.
    if (m_locks[id].try_lock())
    {
        m_locks[id].unlock();
        return;
    }

    m_locks[id].unlock();

    VAStatus status = vaUnmapBuffer(m_vaDisplay, m_images[id].buf);
    assert(VASUCCEEDED(status));

    status = vaDestroyImage(m_vaDisplay, m_images[id].image_id);
    assert(VASUCCEEDED(status));
}

void mvaccel::VDecAccelVAImpl::create_decode_desc()
{
    m_DecodeDesc.format       = VA_FOURCC_NV12;
    m_DecodeDesc.sfcformat    = VA_FOURCC_ARGB;
    m_DecodeDesc.width        = 352;
    m_DecodeDesc.height       = 288;
    m_DecodeDesc.sfc_widht    = 176;
    m_DecodeDesc.sfc_height   = 144;
    m_DecodeDesc.surfaces_num = 32;
}

bool mvaccel::VDecAccelVAImpl::DecodePicture()
{
    // Create addition surfaces for scaled video output
    if (m_sfcIDs.empty())
    {
        if (create_resources())
            return 1;
    }

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VAContextID vaContextID = 0;
    VASurfaceID vaID = 0;

    //va begin picture
    vaStatus = vaBeginPicture(m_vaDisplay, m_vaContextID, vaID);
    if (VAFAILED(vaStatus))
        printf("vaBeginPicture fail.");

    // Set Context ID for End Picture
    vaContextID = m_vaContextID;

    std::vector<VABufferID> vaBufferIDs;
    // Pic parameters buffers
    VABufferID vaBufferID;
    vaStatus = vaCreateBuffer(  m_vaDisplay,
                                m_vaContextID,
                                VAPictureParameterBufferType,
                                sizeof(g_PicParams_AVC),
                                1,
                                (uint8_t*)g_PicParams_AVC,
                                &vaBufferID );
    if (VASUCCEEDED(vaStatus))
                vaBufferIDs.push_back(vaBufferID);

    //CHECK_VASTATUS(va_status, "vaCreateBuffer");

    // IQ matrics
    vaStatus = vaCreateBuffer( m_vaDisplay,
                                m_vaContextID,
                                VAIQMatrixBufferType,
                                sizeof(g_Qmatrix_AVC),
                                1,
                                (uint8_t*)g_Qmatrix_AVC,
                                &vaBufferID );
    //CHECK_VASTATUS(va_status, "vaCreateBuffer");
    if (VASUCCEEDED(vaStatus))
                vaBufferIDs.push_back(vaBufferID);

    //slice parameter buffers
    vaStatus = vaCreateBuffer(m_vaDisplay,
                               m_vaContextID,
                               VASliceParameterBufferType,
                               sizeof(g_SlcParams_AVC),
                               1,
                               (uint8_t*)g_SlcParams_AVC,
                               &vaBufferID );
    //CHECK_VASTATUS(va_status, "vaCreateBuffer");
    if (VASUCCEEDED(vaStatus))
                vaBufferIDs.push_back(vaBufferID);

    //BITSTREAM buffers
    vaStatus = vaCreateBuffer(m_vaDisplay,
                               m_vaContextID,
                               VASliceDataBufferType,
                               sizeof(g_Bitstream_AVC),
                               1,
                               (uint8_t*)g_Bitstream_AVC,
                               &vaBufferID );
    //CHECK_VASTATUS(va_status, "vaCreateBuffer");
    if (VASUCCEEDED(vaStatus))
                vaBufferIDs.push_back(vaBufferID);

    //PROC_PIPELINE buffers
    vaStatus = vaCreateBuffer(m_vaDisplay,
                               m_vaContextID,
                               VAProcPipelineParameterBufferType,
                               sizeof(m_vaProcBuffer),
                               1,
                               (uint8_t*)&m_vaProcBuffer,
                               &vaBufferID );
    //CHECK_VASTATUS(va_status, "vaCreateBuffer");
    if (VASUCCEEDED(vaStatus))
                vaBufferIDs.push_back(vaBufferID);


    if (vaBufferIDs.size())
    {
        vaStatus = vaRenderPicture(
            m_vaDisplay,
            m_vaContextID,
            &(vaBufferIDs.at(0)),
            vaBufferIDs.size());
    }

    //va end picture
    vaStatus = vaEndPicture(m_vaDisplay, vaContextID);
    if (VAFAILED(vaStatus))
        printf("vaEndPicture fail.");


    auto gfx_surface_buf = lock_surface(m_sfcIDs[0], false);
    if (gfx_surface_buf == nullptr)
    {
        printf("Fail to lock gfx surface\n");
        return 1;
    }

    FILE* sfc_stream = fopen("sfc_sample.yuv", "wb");
    fwrite(gfx_surface_buf, 101376, 1, sfc_stream);

    fclose(sfc_stream);

    unlock_surface(m_sfcIDs[0]);
    Close();
}

int mvaccel::VDecAccelVAImpl::check_process_pipeline_caps(DecodeDesc& desc)
{
    VAProcPipelineCaps caps;

    VAProcColorStandardType inputCST[VAProcColorStandardCount];
    VAProcColorStandardType outputCST[VAProcColorStandardCount];
    caps.input_color_standards = inputCST;
    caps.output_color_standards = outputCST;

    m_in4CC.resize(vaMaxNumImageFormats(m_vaDisplay));
    m_out4CC.resize(vaMaxNumImageFormats(m_vaDisplay));
    caps.input_pixel_format = &m_in4CC.at(0);
    caps.output_pixel_format = &m_out4CC.at(0);

    VABufferID filterIDs[VAProcFilterCount];
    uint32_t filterCount = 0;

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    vaStatus = vaQueryVideoProcPipelineCaps(
        m_vaDisplay,
        m_vaContextID,
        filterIDs,
        filterCount,
        &caps
        );
    if (VAFAILED(vaStatus))
    {
        printf("vaQueryVideoProcPipelineCaps fail\n");
        return 1;
    }

    m_in4CC.resize(caps.num_input_pixel_formats);
    m_out4CC.resize(caps.num_output_pixel_formats);

    //m_sfcFmt = desc.sfcformat.FOURCC();
    return 0;
}

int mvaccel::VDecAccelVAImpl::create_resources()
{
    if (m_sfcIDs.empty())
        m_sfcIDs.resize(1);

    // Create addition surfaces for scaled video output
    //auto align = mvaccel::kGfxSurfaceAlign[m_vaProfile];
    //uint32_t width = ALIGN_TO(m_sfcSize[NEW_W], align.width);
    //uint32_t height = ALIGN_TO(m_sfcSize[NEW_H], align.height);

    uint16_t width = m_DecodeDesc.sfc_widht;
    uint16_t height = m_DecodeDesc.sfc_height;

    //m_sfcSamples.clear();
    //prepare sfc surface
    DecodeDesc SfcDesc;
    SfcDesc.sfcformat = MVFOURCC('A','R','G','B');
    VASurfaceAttribArray Sfc_vaSurfAttribs;
    prepare_surface_attribs(SfcDesc, Sfc_vaSurfAttribs, true);

    uint32_t surfaceType = m_surfaceType;
    m_surfaceType = (uint32_t)SfcDesc.sfcformat;

    for(uint32_t i = 0; i < m_sfcIDs.size(); i++)
    {
        create_surface(width, height, Sfc_vaSurfAttribs, m_sfcIDs[i]);
    }
    m_surfaceType = surfaceType;
    m_rectSFC = { 0, 0, width, height};
    //return (m_sfcSamples.size() == m_sfcIDs.size()) ? 0 : 1;

    // Prepare VAProcPipelineParameterBuffer for decode
    VAProcPipelineParameterBuffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    m_rectSrc = { 0, 0, (uint16_t)m_DecodeDesc.width, (uint16_t)m_DecodeDesc.height};
    buffer.surface_region = &m_rectSrc;
    buffer.output_region = &m_rectSFC;
    buffer.additional_outputs = (VASurfaceID*)&(m_sfcIDs[0]);
    buffer.num_additional_outputs = 1;
    m_vaProcBuffer = buffer;

    return 0;
}
