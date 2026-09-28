#define UNICODE
#define _UNICODE
#define INITGUID
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <objbase.h>
#include <appxpackaging.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "urlmon.lib")

DEFINE_GUID(CLSID_MyAppxFactory, 
    0x5842a140, 0xff9f, 0x4166, 0x8f, 0x5c, 0x62, 0xf5, 0xb7, 0xb0, 0xc7, 0x81);

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 3) {
        wprintf(L"Usage: create_msix.exe <output.msix> <staging_dir>\n");
        return 1;
    }
    const wchar_t* outputPath = argv[1];
    const wchar_t* stagingDir = argv[2];

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { wprintf(L"CoInit: 0x%08X\n", hr); return 1; }

    IAppxFactory* factory = NULL;
    hr = CoCreateInstance(CLSID_MyAppxFactory, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { wprintf(L"CoCreate: 0x%08X\n", hr); return 1; }

    IUri* hashUri = NULL;
    hr = CreateUri(L"http://www.w3.org/2001/04/xmlenc#sha256", Uri_CREATE_CANONICALIZE, 0, &hashUri);
    if (FAILED(hr)) { wprintf(L"CreateUri: 0x%08X\n", hr); return 1; }

    DeleteFileW(outputPath);
    IStream* outStream = NULL;
    hr = SHCreateStreamOnFileEx(outputPath, STGM_READWRITE | STGM_CREATE, 0, FALSE, NULL, &outStream);
    if (FAILED(hr)) { wprintf(L"Stream: 0x%08X\n", hr); return 1; }

    APPX_PACKAGE_SETTINGS settings = {0};
    settings.forceZip32 = FALSE;
    settings.hashMethod = hashUri;
    IAppxPackageWriter* writer = NULL;
    hr = factory->CreatePackageWriter(outStream, &settings, &writer);
    if (FAILED(hr)) { wprintf(L"CreateWriter: 0x%08X\n", hr); return 1; }
    wprintf(L"Writer OK\n");

    wchar_t path[MAX_PATH];
    IStream* s = NULL;

    auto addFile = [&](const wchar_t* diskPath, const wchar_t* pkgName, const wchar_t* contentType, APPX_COMPRESSION_OPTION comp) {
        wcscpy_s(path, stagingDir); wcscat_s(path, L"\\"); wcscat_s(path, diskPath);
        
        hr = SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, NULL, &s);
        if (SUCCEEDED(hr)) {
            LARGE_INTEGER liZero = {0};
            s->Seek(liZero, STREAM_SEEK_SET, NULL);
            hr = writer->AddPayloadFile(pkgName, contentType, comp, s);
            s->Release();
        }
        wprintf(L"  %s: %s 0x%08X\n", pkgName, SUCCEEDED(hr) ? L"OK" : L"FAIL", hr);
    };

    addFile(L"ShellExtension.dll", L"ShellExtension.dll", L"application/x-msdownload", APPX_COMPRESSION_OPTION_NONE);
    if (FAILED(hr)) return 1;
    
    addFile(L"GroupManager.exe", L"GroupManager.exe", L"application/x-msdownload", APPX_COMPRESSION_OPTION_NORMAL);
    if (FAILED(hr)) return 1;
    
    addFile(L"Assets\\StoreLogo.png", L"Assets\\StoreLogo.png", L"image/png", APPX_COMPRESSION_OPTION_NONE);
    addFile(L"Assets\\Square150x150Logo.png", L"Assets\\Square150x150Logo.png", L"image/png", APPX_COMPRESSION_OPTION_NONE);
    addFile(L"Assets\\Square44x44Logo.png", L"Assets\\Square44x44Logo.png", L"image/png", APPX_COMPRESSION_OPTION_NONE);
    addFile(L"Assets\\Wide310x150Logo.png", L"Assets\\Wide310x150Logo.png", L"image/png", APPX_COMPRESSION_OPTION_NONE);

    // Write manifest via Close
    wcscpy_s(path, stagingDir); wcscat_s(path, L"\\AppxManifest.xml");
    hr = SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, NULL, &s);
    if (SUCCEEDED(hr)) {
        LARGE_INTEGER liZero = {0};
        s->Seek(liZero, STREAM_SEEK_SET, NULL);
        hr = writer->Close(s);
        s->Release();
    } else {
        hr = writer->Close(NULL);
    }
    wprintf(L"\nClose+Manifest: %s 0x%08X\n", SUCCEEDED(hr) ? L"OK" : L"FAIL", hr);

    if (SUCCEEDED(hr)) {
        HANDLE h = CreateFileW(outputPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { wprintf(L"MSIX: %s (%u bytes)\n", outputPath, GetFileSize(h, NULL)); CloseHandle(h); }
    }

    if (writer) writer->Release();
    hashUri->Release(); outStream->Release(); factory->Release();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
