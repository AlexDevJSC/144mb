/* SPDX-License-Identifier: MIT */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdbool.h>

static const GUID cls_compiler = {0x73e22d93,0xe6ce,0x47f3,{0xb5,0xbf,0xf0,0x66,0x4f,0x39,0xc1,0xb0}};
static const GUID cls_utils = {0x6245d6af,0x66e0,0x48fd,{0x80,0xb4,0x4d,0x27,0x17,0x96,0x74,0x8c}};
static const GUID iid_compiler = {0x228b4687,0x5a6a,0x4730,{0x90,0x0c,0x97,0x02,0xb2,0x20,0x3f,0x54}};
static const GUID iid_utils = {0x4605c4cb,0x2019,0x492a,{0xad,0xa4,0x65,0xf2,0x0b,0xb7,0xd6,0x7f}};
static const GUID iid_result = {0x58346cda,0xdde7,0x4497,{0x94,0x61,0x6f,0x87,0xaf,0x5e,0x06,0x59}};
static const GUID iid_blob = {0x8ba5fb08,0x5195,0x40e2,{0xac,0x58,0x0d,0x98,0x9c,0x3a,0x01,0x02}};
static const GUID iid_utf8 = {0x3da636c9,0xba71,0x4024,{0xa3,0x01,0x30,0xcb,0xf1,0x25,0x30,0x5b}};

struct dxc_buffer { const void *data; SIZE_T size; UINT32 encoding; };
struct dxc_interface { const void *vtable; };
struct compiler_api {
    void *unknown[3];
    HRESULT (WINAPI *compile)(void *, const struct dxc_buffer *, const WCHAR **, UINT, void *, REFIID, void **);
};
struct utils_api {
    void *unknown_and_utils[9];
    HRESULT (WINAPI *include)(void *, void **);
};
struct result_api {
    void *unknown[3];
    HRESULT (WINAPI *status)(void *, HRESULT *);
    void *result, *errors;
    BOOL (WINAPI *has_output)(void *, UINT);
    HRESULT (WINAPI *output)(void *, UINT, REFIID, void **, void **);
};
struct blob_api {
    void *unknown[3];
    const void *(WINAPI *data)(void *);
    SIZE_T (WINAPI *size)(void *);
    HRESULT (WINAPI *encoding)(void *, BOOL *, UINT *);
    const void *(WINAPI *string)(void *);
    SIZE_T (WINAPI *length)(void *);
};

__declspec(dllimport) HRESULT WINAPI DxcCreateInstance(REFCLSID, REFIID, void **);
static const WCHAR include_path[] = L"C:\\color.hlsli";
static const char include_source[] = "float4 color() { return float4(1,0.5,0.25,1); }\n";
static const char source[] = "#include \"color.hlsli\"\nfloat4 main() : SV_Target { return color(); }";
static const char bad_source[] = "float4 main() : SV_Target { return this_is_not_valid; }";

static bool dx_container(const void *data, SIZE_T size)
{
    const unsigned char *bytes = data;
    return bytes && size > 32 && bytes[0] == 'D' && bytes[1] == 'X' &&
           bytes[2] == 'B' && bytes[3] == 'C';
}

static void report_dxc_error(struct dxc_interface *result, const struct result_api *methods)
{
    struct dxc_interface *errors = NULL;
    if (FAILED(methods->output(result, 2, &iid_utf8, (void **)&errors, NULL)) || !errors)
        return;
    const struct blob_api *blob = errors->vtable;
    const SIZE_T length = blob->length(errors);
    if (length && length <= MAXDWORD) {
        DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), blob->string(errors), (DWORD)length, &written, NULL);
    }
    IUnknown_Release((IUnknown *)errors);
}

static int compile_legacy(void)
{
    ID3DBlob *bytecode = NULL, *errors = NULL;
    if (FAILED(D3DCompile(source, sizeof(source) - 1, "C:\\fixture.hlsl", NULL,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0", 0, 0, &bytecode, &errors)) ||
        !bytecode || !dx_container(ID3D10Blob_GetBufferPointer(bytecode), ID3D10Blob_GetBufferSize(bytecode)))
        return 12;
    ID3D10Blob_Release(bytecode);
    if (errors)
        ID3D10Blob_Release(errors);
    bytecode = NULL;
    errors = NULL;
    if (SUCCEEDED(D3DCompile(bad_source, sizeof(bad_source) - 1, "invalid.hlsl",
            NULL, NULL, "main", "ps_5_0", 0, 0, &bytecode, &errors)) ||
        bytecode || !errors || !ID3D10Blob_GetBufferSize(errors))
        return 13;
    ID3D10Blob_Release(errors);
    return 0;
}

static int compile_modern(void)
{
    struct dxc_interface *compiler = NULL, *utils = NULL, *result = NULL, *blob = NULL, *name = NULL;
    void *include = NULL;
    if (FAILED(DxcCreateInstance(&cls_compiler, &iid_compiler, (void **)&compiler)) ||
        FAILED(DxcCreateInstance(&cls_utils, &iid_utils, (void **)&utils)) || !compiler || !utils)
        return 20;
    const struct compiler_api *compiler_methods = compiler->vtable;
    const struct utils_api *utils_methods = utils->vtable;
    if (FAILED(utils_methods->include(utils, &include)) || !include)
        return 21;
    /* IDxcCompiler3::Compile receives the shader source in DxcBuffer.
     * Only switches belong in pArguments; a positional source file is not
     * accepted consistently by the native compiler on Fedora. */
    const WCHAR *arguments[] = {L"-E", L"main", L"-T", L"ps_6_0",
                                L"-Fo", L"r\x00e9sultat.dxil"};
    const struct dxc_buffer shader = {source, sizeof(source) - 1, CP_UTF8};
    if (FAILED(compiler_methods->compile(compiler, &shader, arguments, 6, include, &iid_result, (void **)&result)) ||
        !result)
        return 22;
    const struct result_api *result_methods = result->vtable;
    HRESULT compilation;
    if (FAILED(result_methods->status(result, &compilation)))
        return 23;
    if (FAILED(compilation)) {
        /* Report the native HRESULT even if DXC provides no error blob. */
        static const char prefix[] = "DXC status 0x";
        char diagnostic[sizeof(prefix) - 1 + 8 + 1];
        static const char digits[] = "0123456789abcdef";
        for (unsigned int i = 0; i < sizeof(prefix) - 1; ++i)
            diagnostic[i] = prefix[i];
        for (unsigned int i = 0; i < 8; ++i)
            diagnostic[sizeof(prefix) - 1 + i] =
                digits[((UINT32)compilation >> ((7 - i) * 4)) & 15];
        diagnostic[sizeof(diagnostic) - 1] = '\n';
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), diagnostic,
                  sizeof(diagnostic), &written, NULL);
        report_dxc_error(result, result_methods);
        return 24;
    }
    if (!result_methods->has_output(result, 1) ||
        FAILED(result_methods->output(result, 1, &iid_blob, (void **)&blob, (void **)&name)) || !blob || !name)
        return 25;
    const struct blob_api *blob_methods = blob->vtable, *name_methods = name->vtable;
    const WCHAR *filename = name_methods->string(name);
    BOOL known = FALSE;
    UINT encoding = 0;
    if (!dx_container(blob_methods->data(blob), blob_methods->size(blob)) || !filename ||
        name_methods->length(name) != 13 || filename[1] != 0x00e9 ||
        FAILED(name_methods->encoding(name, &known, &encoding)) || !known || encoding != 1200)
        return 26;
    IUnknown_Release((IUnknown *)name);
    IUnknown_Release((IUnknown *)blob);
    IUnknown_Release((IUnknown *)result);
    result = NULL;
    const struct dxc_buffer bad = {bad_source, sizeof(bad_source) - 1, CP_UTF8};
    if (FAILED(compiler_methods->compile(compiler, &bad, arguments, 6, include, &iid_result, (void **)&result)) ||
        !result || FAILED(result_methods->status(result, &compilation)) || SUCCEEDED(compilation) ||
        !result_methods->has_output(result, 2))
        return 27;
    blob = NULL;
    if (FAILED(result_methods->output(result, 2, &iid_utf8, (void **)&blob, NULL)) || !blob)
        return 28;
    blob_methods = blob->vtable;
    if (!blob_methods->length(blob) || !blob_methods->string(blob))
        return 29;
    IUnknown_Release((IUnknown *)blob);
    IUnknown_Release((IUnknown *)result);
    IUnknown_Release((IUnknown *)include);
    IUnknown_Release((IUnknown *)utils);
    IUnknown_Release((IUnknown *)compiler);
    return 0;
}

int import_fixture_entry(void)
{
    HANDLE file = CreateFileW(include_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 10;
    DWORD written = 0;
    bool saved = WriteFile(file, include_source, sizeof(include_source) - 1, &written, NULL) &&
                 written == sizeof(include_source) - 1;
    CloseHandle(file);
    int result = saved ? compile_legacy() : 11;
    if (!result)
        result = compile_modern();
    DeleteFileW(include_path);
    return result;
}
