//==============================================================================
// Copyright Intel Corporation
//
// SPDX-License-Identifier: MIT
//==============================================================================

///
/// A minimal oneAPI Video Processing Library (oneVPL) decode and infer application,
/// using oneVPL 2.2 API features including internal memory. For more information see:
/// https://software.intel.com/content/www/us/en/develop/articles/upgrading-from-msdk-to-onevpl.html
/// https://oneapi-src.github.io/oneAPI-spec/elements/oneVPL/source/index.html
///
/// @file

#include <inference_engine.hpp>
#include "util.h"
using namespace InferenceEngine;

#define BITSTREAM_BUFFER_SIZE      2000000
#define MAX_RESULTS                5
#define MAJOR_API_VERSION_REQUIRED 2
#define MINOR_API_VERSION_REQUIRED 2

void Usage(void) {
    printf("\n");
    printf("   Usage  :  hello-decode-infer \n\n");
    printf("     -sw/-hw        use software or hardware implementation\n");
    printf("     -i             input file name (HEVC elementary stream)\n\n");
    printf("     -m             input model name (OpenVINO)\n\n");
    printf("   Example:  hello-decode-infer -sw  -i in.h265 -m alexnet.xml\n");
    return;
}

// Sort and extract top n results from output blob
void PrintTopResults(const Blob::Ptr &output) {
    SizeVector dims = output->getTensorDesc().getDims();
    if (0 == dims.size() || 1 != dims[0]) {
        printf("Output blob has incorrect dimensions, skipping\n");
        return;
    }

    unsigned n       = static_cast<unsigned>(std::min<size_t>((size_t)MAX_RESULTS, output->size()));
    float *batchData = output->cbuffer().as<float *>();
    std::vector<unsigned> indexes(output->size());

    std::iota(std::begin(indexes), std::end(indexes), 0);
    std::partial_sort(std::begin(indexes),
                      std::begin(indexes) + n,
                      std::end(indexes),
                      [&batchData](unsigned l, unsigned r) {
                          return batchData[l] > batchData[r];
                      });

    printf("\nTop %d results for video frame:", n);
    printf("\nclassid probability\n");
    printf("------- -----------\n");
    for (unsigned j = 0; j < n; j++) {
        printf("%d %f\n", indexes.at(j), batchData[indexes.at(j)]);
    }
}

// Perform classify inference on video frame
void InferFrame(mfxFrameSurface1 *surface,
                InferRequest *infer_request,
                std::string input_name,
                std::string output_name) {
    mfxFrameInfo *info = &surface->Info;
    mfxFrameData *data = &surface->Data;
    Blob::Ptr in_blob, out_blob;
    size_t w = info->Width;
    size_t h = info->Height;
    size_t p = data->Pitch;

    switch (info->FourCC) {
        case MFX_FOURCC_I420: {
            TensorDesc y_desc(Precision::U8, { 1, 1, h, p }, Layout::NHWC);
            TensorDesc uv_desc(Precision::U8, { 1, 1, h / 2, p / 2 }, Layout::NHWC);

            Blob::Ptr y_blob = make_shared_blob<uint8_t>(y_desc, data->Y);
            Blob::Ptr u_blob = make_shared_blob<uint8_t>(uv_desc, data->U);
            Blob::Ptr v_blob = make_shared_blob<uint8_t>(uv_desc, data->V);

            in_blob = make_shared_blob<I420Blob>(y_blob, u_blob, v_blob);
        } break;

        case MFX_FOURCC_NV12: {
            TensorDesc y_desc(Precision::U8, { 1, 1, h, p }, Layout::NHWC);
            TensorDesc uv_desc(Precision::U8, { 1, 2, h / 2, p / 2 }, Layout::NHWC);

            Blob::Ptr y_blob  = make_shared_blob<uint8_t>(y_desc, data->Y);
            Blob::Ptr uv_blob = make_shared_blob<uint8_t>(uv_desc, data->UV);

            in_blob = make_shared_blob<NV12Blob>(y_blob, uv_blob);
        } break;

        default:
            printf("Unsupported FourCC code, skip InferFrame\n");
            return;
    }

    infer_request->SetBlob(input_name, in_blob);
    infer_request->Infer();
    out_blob = infer_request->GetBlob(output_name);

    PrintTopResults(out_blob);
}

int main(int argc, char *argv[]) {
    FILE *source                    = NULL;
    FILE *sink                      = NULL;
    int accel_fd                    = 0;
    mfxSession session              = NULL;
    mfxFrameSurface1 *decSurfaceOut = NULL;
    void *accelHandle               = NULL;
    mfxBitstream bitstream          = {};
    mfxSyncPoint syncp              = {};
    mfxVideoParam mfxDecParams      = {};
    mfxU32 frameNum                 = 0;
    bool isDraining                 = false;
    bool isStillGoing               = true;
    mfxStatus sts                   = MFX_ERR_NONE;
    Params cliParams                = {};

    // variables used only in 2.x version
    mfxConfig cfg[3];
    mfxVariant cfgVal[3];
    mfxLoader loader = NULL;

    // OpenVINO
    Core ie;
    CNNNetwork network;
    std::string input_name, output_name;
    InputInfo::Ptr input_info;
    DataPtr output_info;
    ExecutableNetwork executable_network;
    InferRequest infer_request;

    //Parse command line args to cliParams
    if (ParseArgsAndValidate(argc, argv, &cliParams, PARAMS_DECODE) == false) {
        Usage();
        return 1; // return 1 as error code
    }

    source = fopen(cliParams.infileName, "rb");
    VERIFY(source, "Could not open input file");

    // Initialize VPL session
    loader = MFXLoad();
    VERIFY(NULL != loader, "MFXLoad failed -- is implementation in path?");

    // Implementation used must be the type requested from command line
    cfg[0] = MFXCreateConfig(loader);
    VERIFY(NULL != cfg[0], "MFXCreateConfig failed")

    sts =
        MFXSetConfigFilterProperty(cfg[0], (mfxU8 *)"mfxImplDescription.Impl", cliParams.implValue);
    VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed for Impl");

    // Implementation must provide an HEVC decoder
    cfg[1] = MFXCreateConfig(loader);
    VERIFY(NULL != cfg[1], "MFXCreateConfig failed")
    cfgVal[1].Type     = MFX_VARIANT_TYPE_U32;
    cfgVal[1].Data.U32 = MFX_CODEC_HEVC;
    sts                = MFXSetConfigFilterProperty(
        cfg[1],
        (mfxU8 *)"mfxImplDescription.mfxDecoderDescription.decoder.CodecID",
        cfgVal[1]);
    VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed for decoder CodecID");

    // Implementation used must provide API version 2.2 or newer
    cfg[2] = MFXCreateConfig(loader);
    VERIFY(NULL != cfg[2], "MFXCreateConfig failed")
    cfgVal[2].Type     = MFX_VARIANT_TYPE_U32;
    cfgVal[2].Data.U32 = VPLVERSION(MAJOR_API_VERSION_REQUIRED, MINOR_API_VERSION_REQUIRED);
    sts                = MFXSetConfigFilterProperty(cfg[2],
                                     (mfxU8 *)"mfxImplDescription.ApiVersion.Version",
                                     cfgVal[2]);
    VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed for API version");

    sts = MFXCreateSession(loader, 0, &session);
    VERIFY(MFX_ERR_NONE == sts,
           "Cannot create session -- no implementations meet selection criteria");

    // Print info about implementation loaded
    ShowImplementationInfo(loader, 0);

    // Convenience function to initialize available accelerator(s)
    accelHandle = InitAcceleratorHandle(session, &accel_fd);

    // Prepare input bitstream and start decoding
    bitstream.MaxLength = BITSTREAM_BUFFER_SIZE;
    bitstream.Data      = (mfxU8 *)calloc(bitstream.MaxLength, sizeof(mfxU8));
    VERIFY(bitstream.Data, "Not able to allocate input buffer");
    bitstream.CodecId = MFX_CODEC_HEVC;

    sts = ReadEncodedStream(bitstream, source);
    VERIFY(MFX_ERR_NONE == sts, "Error reading bitstream\n");

    mfxDecParams.mfx.CodecId = MFX_CODEC_HEVC;
    mfxDecParams.IOPattern   = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    sts                      = MFXVideoDECODE_DecodeHeader(session, &bitstream, &mfxDecParams);
    VERIFY(MFX_ERR_NONE == sts, "Error decoding header\n");

    // input parameters finished, now initialize decode
    sts = MFXVideoDECODE_Init(session, &mfxDecParams);
    VERIFY(MFX_ERR_NONE == sts, "Error initializing decode\n");

    // Setup OpenVINO Inference Engine
    network = ie.ReadNetwork(cliParams.inmodelName);
    VERIFY(network.getInputsInfo().size() == 1, "Sample supports topologies with 1 input only");
    VERIFY(network.getOutputsInfo().size() == 1, "Sample supports topologies with 1 output only");

    input_info = network.getInputsInfo().begin()->second;
    input_info->getPreProcess().setResizeAlgorithm(RESIZE_BILINEAR);
    input_info->getPreProcess().setColorFormat(
        mfxDecParams.mfx.FrameInfo.FourCC == MFX_FOURCC_I420 ? I420 : NV12);
    input_info->setLayout(Layout::NHWC);
    input_info->setPrecision(Precision::U8);
    input_name = network.getInputsInfo().begin()->first;

    output_info = network.getOutputsInfo().begin()->second;
    output_info->setPrecision(Precision::FP32);
    output_name = network.getOutputsInfo().begin()->first;

    executable_network =
        ie.LoadNetwork(network, cliParams.impl == MFX_IMPL_SOFTWARE ? "CPU" : "GPU");
    infer_request = executable_network.CreateInferRequest();

    printf("Decoding and infering %s with %s\n", cliParams.infileName, cliParams.inmodelName);

    while (isStillGoing == true) {
        // Load encoded stream if not draining
        if (isDraining == false) {
            sts = ReadEncodedStream(bitstream, source);
            if (sts != MFX_ERR_NONE)
                isDraining = true;
        }

        sts = MFXVideoDECODE_DecodeFrameAsync(session,
                                              (isDraining) ? NULL : &bitstream,
                                              NULL,
                                              &decSurfaceOut,
                                              &syncp);

        switch (sts) {
            case MFX_ERR_NONE:
                do {
                    sts = decSurfaceOut->FrameInterface->Synchronize(decSurfaceOut,
                                                                     WAIT_100_MILLISECONDS);
                    if (MFX_ERR_NONE == sts) {
                        decSurfaceOut->FrameInterface->Map(decSurfaceOut, MFX_MAP_READ);
                        VERIFY(MFX_ERR_NONE == sts, "mfxFrameSurfaceInterface->Map failed");

                        InferFrame(decSurfaceOut, &infer_request, input_name, output_name);

                        sts = decSurfaceOut->FrameInterface->Unmap(decSurfaceOut);
                        VERIFY(MFX_ERR_NONE == sts, "mfxFrameSurfaceInterface->Unmap failed");

                        sts = decSurfaceOut->FrameInterface->Release(decSurfaceOut);
                        VERIFY(MFX_ERR_NONE == sts, "mfxFrameSurfaceInterface->Release failed");

                        frameNum++;
                    }
                } while (sts == MFX_WRN_IN_EXECUTION);
                break;
            case MFX_ERR_MORE_DATA:
                // The function requires more bitstream at input before decoding can proceed
                if (isDraining)
                    isStillGoing = false;
                break;
            case MFX_ERR_MORE_SURFACE:
                // The function requires more frame surface at output before decoding can proceed.
                // This applies to external memory allocations and should not be expected for
                // a simple internal allocation case like this
                break;
            case MFX_ERR_DEVICE_LOST:
                // For non-CPU implementations,
                // Cleanup if device is lost
                break;
            case MFX_WRN_DEVICE_BUSY:
                // For non-CPU implementations,
                // Wait a few milliseconds then try again
                break;
            case MFX_WRN_VIDEO_PARAM_CHANGED:
                // The decoder detected a new sequence header in the bitstream.
                // Video parameters may have changed.
                // In external memory allocation case, might need to reallocate the output surface
                break;
            case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
                // The function detected that video parameters provided by the application
                // are incompatible with initialization parameters.
                // The application should close the component and then reinitialize it
                break;
            case MFX_ERR_REALLOC_SURFACE:
                // Bigger surface_work required. May be returned only if
                // mfxInfoMFX::EnableReallocRequest was set to ON during initialization.
                // This applies to external memory allocations and should not be expected for
                // a simple internal allocation case like this
                break;
            default:
                printf("unknown status %d\n", sts);
                isStillGoing = false;
                break;
        }
    }

end:
    printf("Decoded %d frames\n", frameNum);

    // Clean up resources - It is recommended to close components first, before
    // releasing allocated surfaces, since some surfaces may still be locked by
    // internal resources.
    if (loader)
        MFXUnload(loader);

    if (sink)
        fclose(sink);

    MFXVideoDECODE_Close(session);
    MFXClose(session);

    if (bitstream.Data)
        free(bitstream.Data);

    if (accelHandle)
        FreeAcceleratorHandle(accelHandle, accel_fd);

    return 0;
}
