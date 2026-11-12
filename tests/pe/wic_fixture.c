/* SPDX-License-Identifier: MIT */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <wincodec.h>

static const unsigned char bitmap[] = {
    'B','M',62,0,0,0,0,0,0,0,54,0,0,0,
    40,0,0,0,2,0,0,0,1,0,0,0,1,0,24,0,0,0,0,0,8,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,255,0,255,0,0,0,
};
static const WCHAR path[] = L"C:\\wic-fixture.bmp";

static int test_bitmap(void)
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    if (CoCreateInstance(&CLSID_WICImagingFactory2, NULL, CLSCTX_INPROC_SERVER,
                         &IID_IWICImagingFactory, (void **)&factory) != CO_E_NOTINITIALIZED || factory)
        return 12;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) ||
        FAILED(CoCreateInstance(&CLSID_WICImagingFactory2, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&factory)) || !factory)
        return 13;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ,
                      WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder)
        return 14;
    UINT frames, width, height;
    if (FAILED(IWICBitmapDecoder_GetFrameCount(decoder, &frames)) || frames != 1 ||
        FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) || !frame ||
        FAILED(IWICBitmapFrameDecode_GetSize(frame, &width, &height)) || width != 2 || height != 1)
        return 15;
    BYTE rgba[8];
    if (FAILED(IWICBitmapFrameDecode_CopyPixels(frame, NULL, 8, sizeof(rgba), rgba)) ||
        rgba[0] != 255 || rgba[1] || rgba[2] || rgba[3] != 255 ||
        rgba[4] || rgba[5] != 255 || rgba[6] || rgba[7] != 255)
        return 16;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(factory, &converter)) || !converter ||
        FAILED(IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame,
                    &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0,
                    WICBitmapPaletteTypeCustom)))
        return 17;
    IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(decoder);
    const WICRect first = {0, 0, 1, 1};
    BYTE bgra[4];
    if (IWICFormatConverter_CopyPixels(converter, &first, 4, 3, bgra) != WINCODEC_ERR_INSUFFICIENTBUFFER ||
        FAILED(IWICFormatConverter_CopyPixels(converter, &first, 4, 4, bgra)) ||
        bgra[0] || bgra[1] || bgra[2] != 255 || bgra[3] != 255)
        return 18;
    IWICFormatConverter_Release(converter);
    IWICImagingFactory_Release(factory);
    CoUninitialize();
    return 0;
}

int import_fixture_entry(void)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 10;
    DWORD written = 0;
    BOOL saved = WriteFile(file, bitmap, sizeof(bitmap), &written, NULL);
    CloseHandle(file);
    int result = saved && written == sizeof(bitmap) ? test_bitmap() : 11;
    DeleteFileW(path);
    return result;
}
