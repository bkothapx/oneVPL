/*############################################################################
  # Copyright (C) Intel Corporation
  #
  # SPDX-License-Identifier: MIT
  ############################################################################*/

#include <algorithm>
#include <log/log.h>

#include "vpl/mfx_dispatcher_vpl.h"

// leave table formatting alone
// clang-format off

// new functions for API >= 2.0
static const VPLFunctionDesc FunctionDesc2[NumVPLFunctions] = {
    { "MFXQueryImplsDescription",               { {  0, 2 } } },
    { "MFXReleaseImplDescription",              { {  0, 2 } } },
    { "MFXMemory_GetSurfaceForVPP",             { {  0, 2 } } },
    { "MFXMemory_GetSurfaceForEncode",          { {  0, 2 } } },
    { "MFXMemory_GetSurfaceForDecode",          { {  0, 2 } } },
    { "MFXInitialize",                          { {  0, 2 } } },

    { "MFXMemory_GetSurfaceForVPPOut",          { {  1, 2 } } },
    { "MFXVideoDECODE_VPP_Init",                { {  1, 2 } } },
    { "MFXVideoDECODE_VPP_DecodeFrameAsync",    { {  1, 2 } } },
    { "MFXVideoDECODE_VPP_Reset",               { {  1, 2 } } },
    { "MFXVideoDECODE_VPP_GetChannelParam",     { {  1, 2 } } },
    { "MFXVideoDECODE_VPP_Close",               { {  1, 2 } } },
    { "MFXVideoVPP_ProcessFrameAsync",          { {  1, 2 } } },
};

static const VPLFunctionDesc MSDKCompatFunctions[NumMSDKFunctions] = {
    { "MFXInitEx",                              { { 14, 1 } } },
    { "MFXClose" ,                              { {  0, 1 } } },
};

// end table formatting
// clang-format on

// implementation of loader context (mfxLoader)
// each loader instance will build a list of valid runtimes and allow
// application to create sessions with them
LoaderCtxVPL::LoaderCtxVPL()
        : m_libInfoList(),
          m_implInfoList(),
          m_configCtxList(),
          m_specialConfig(),
          m_implIdxNext(0),
          m_bKeepCapsUntilUnload(true),
          m_envVar(),
          m_dispLog() {
    // allow loader to distinguish between property value of 0
    //   and property not set
    m_specialConfig.bIsSet_deviceHandleType = false;
    m_specialConfig.bIsSet_deviceHandle     = false;
    m_specialConfig.bIsSet_accelerationMode = false;
    m_specialConfig.bIsSet_ApiVersion       = false;
    m_specialConfig.bIsSet_dxgiAdapterIdx   = false;

    return;
}

LoaderCtxVPL::~LoaderCtxVPL() {
    return;
}

// creates ordered list of user-specified directories to search
mfxU32 LoaderCtxVPL::ParseEnvSearchPaths(const CHAR_TYPE *envVarName,
                                         std::list<STRING_TYPE> &searchDirs) {
    searchDirs.clear();

#if defined(_WIN32) || defined(_WIN64)
    DWORD err;
    m_envVar[0] = 0;

    err = GetEnvironmentVariableW(envVarName, m_envVar, MAX_ENV_VAR_LEN);
    if (err == 0 || err >= MAX_ENV_VAR_LEN)
        return 0; // environment variable not defined or string too long

    // parse env variable into individual directories
    std::wstringstream envPath((CHAR_TYPE *)m_envVar);
    STRING_TYPE s;
    while (std::getline(envPath, s, L';')) {
        searchDirs.push_back(s);
    }
#else
    CHAR_TYPE *envVar = getenv(envVarName);
    if (!envVar)
#ifdef ANDROID
        envVar = (CHAR_TYPE *)ONEVPL_SEARCH_PATH;
#else
        return 0; // environment variable not defined
#endif

    // parse env variable into individual directories
    std::stringstream envPath((CHAR_TYPE *)envVar);
    STRING_TYPE s;
    while (std::getline(envPath, s, ':')) {
        searchDirs.push_back(s);
    }
#endif

    return (mfxU32)searchDirs.size();
}

#define NUM_LIB_PREFIXES 2

mfxStatus LoaderCtxVPL::SearchDirForLibs(STRING_TYPE searchDir,
                                         std::list<LibInfo *> &libInfoList,
                                         mfxU32 priority) {
    // okay to call with empty searchDir
    if (searchDir.empty())
        return MFX_ERR_NONE;

#if defined(_WIN32) || defined(_WIN64)
    HANDLE hTestFile = nullptr;
    WIN32_FIND_DATAW testFileData;
    DWORD err;
    STRING_TYPE testFileName[NUM_LIB_PREFIXES] = { searchDir + MAKE_STRING("/libvpl*.dll"),
                                                   searchDir + MAKE_STRING("/libmfx*.dll") };

    CHAR_TYPE currDir[MAX_VPL_SEARCH_PATH] = L"";
    if (GetCurrentDirectoryW(MAX_VPL_SEARCH_PATH, currDir))
        SetCurrentDirectoryW(searchDir.c_str());

    // iterate over all candidate files in directory
    for (mfxU32 i = 0; i < NUM_LIB_PREFIXES; i++) {
        hTestFile = FindFirstFileW(testFileName[i].c_str(), &testFileData);
        if (hTestFile != INVALID_HANDLE_VALUE) {
            do {
                wchar_t libNameFull[MAX_VPL_SEARCH_PATH];
                wchar_t *libNameBase;

                // special case: do not include dispatcher itself (libmfx.dll, libvpl.dll)
                if (wcsstr(testFileData.cFileName, L"libmfx.dll") ||
                    wcsstr(testFileData.cFileName, L"libvpl.dll") ||
                    wcsstr(testFileData.cFileName, L"libvpld.dll"))
                    continue;

                err = GetFullPathNameW(testFileData.cFileName,
                                       MAX_VPL_SEARCH_PATH,
                                       libNameFull,
                                       &libNameBase);
                // unknown error - skip it and move on to next file
                if (!err)
                    continue;

                // skip duplicates
                auto libFound =
                    std::find_if(libInfoList.begin(), libInfoList.end(), [&](LibInfo *li) {
                        return (li->libNameFull == libNameFull);
                    });
                if (libFound != libInfoList.end())
                    continue;

                LibInfo *libInfo = new LibInfo;
                if (!libInfo)
                    return MFX_ERR_MEMORY_ALLOC;

                libInfo->libNameFull = libNameFull;
                libInfo->libPriority = priority;

                // add to list
                libInfoList.push_back(libInfo);
            } while (FindNextFileW(hTestFile, &testFileData));

            FindClose(hTestFile);
        }
    }

    // restore current directory
    if (currDir[0])
        SetCurrentDirectoryW(currDir);

#else
    DIR *pSearchDir;
    struct dirent *currFile;

    pSearchDir = opendir(searchDir.c_str());
    if (pSearchDir) {
        while (1) {
            currFile = readdir(pSearchDir);
            if (!currFile)
                break;

            // save files with ".so" (including .so.1, etc.)
            if (strstr(currFile->d_name, ".so")) {
                // library names must begin with "libvpl*" or "libmfx*"
                if ((strstr(currFile->d_name, "libvpl") != currFile->d_name) &&
                    (strstr(currFile->d_name, "libmfx") != currFile->d_name))
                    continue;

                // special case: do not include dispatcher itself (libmfx.so*, libvpl.so*) or tracer library
                if (strstr(currFile->d_name, "libmfx.so") ||
                    strstr(currFile->d_name, "libvpl.so") ||
                    strstr(currFile->d_name, "libmfx-tracer"))
                    continue;

                char filePathC[MAX_VPL_SEARCH_PATH];

                // get full path to found library
                snprintf(filePathC,
                         MAX_VPL_SEARCH_PATH,
                         "%s/%s",
                         searchDir.c_str(),
                         currFile->d_name);
                char *fullPath = realpath(filePathC, NULL);

                // unknown error - skip it and move on to next file
                if (!fullPath)
                    continue;

                // skip duplicates
                auto libFound =
                    std::find_if(libInfoList.begin(), libInfoList.end(), [&](LibInfo *li) {
                        return (li->libNameFull == fullPath);
                    });
                if (libFound != libInfoList.end()) {
                    free(fullPath);
                    continue;
                }

                LibInfo *libInfo = new LibInfo;
                if (!libInfo)
                    return MFX_ERR_MEMORY_ALLOC;

                libInfo->libNameFull = fullPath;
                libInfo->libPriority = priority;
                free(fullPath);

                // add to list
                libInfoList.push_back(libInfo);
            }
        }
        closedir(pSearchDir);
    }
#endif

    return MFX_ERR_NONE;
}

mfxU32 LoaderCtxVPL::GetSearchPathsDriverStore(std::list<STRING_TYPE> &searchDirs) {
    searchDirs.clear();

#if defined(_WIN32) || defined(_WIN64)
    mfxStatus sts = MFX_ERR_UNSUPPORTED;
    STRING_TYPE vplPath;

    mfxU32 numAdaptersD3D9  = 0;
    mfxU32 numAdaptersDXGI1 = 0;
    mfxU32 numAdaptersMax   = 0;

    // query for number of D3D9 and D3D11 adapters on system
    // conservatively check driver store from 0 to sum of adapters
    //   (though in practice D3D9 will usually be a subset of D3D11)
    sts = MFX::GetNumDXGIAdapters(numAdaptersD3D9, numAdaptersDXGI1);
    if (sts == MFX_ERR_NONE)
        numAdaptersMax = numAdaptersD3D9 + numAdaptersDXGI1;

    if (numAdaptersMax == 0)
        numAdaptersMax = MAX_WINDOWS_ADAPTER_ID + 1;

    // get path to Windows driver store
    for (mfxU32 adapterID = 0; adapterID < numAdaptersMax; adapterID++) {
        vplPath.clear();
        sts = MFX::MFXLibraryIterator::GetDriverStoreDir(vplPath,
                                                         MAX_VPL_SEARCH_PATH,
                                                         adapterID,
                                                         MFX::MFX_DRIVER_STORE_ONEVPL);
        if (sts == MFX_ERR_NONE)
            searchDirs.push_back(vplPath);
    }
#endif

    return (mfxU32)searchDirs.size();
}

mfxU32 LoaderCtxVPL::GetSearchPathsCurrentExe(std::list<STRING_TYPE> &searchDirs) {
    searchDirs.clear();

#if defined(_WIN32) || defined(_WIN64)
    // get path to location of current executable
    wchar_t implPath[MFX::msdk_disp_path_len];
    MFX::GetImplPath(MFX::MFX_APP_FOLDER, implPath);
    STRING_TYPE exePath = implPath;

    // strip trailing backslach
    size_t exePathLen = exePath.find_last_of(L"\\");
    if (exePathLen > 0)
        exePath.erase(exePathLen);

    if (!exePath.empty())
        searchDirs.push_back(exePath);

#endif

    return (mfxU32)searchDirs.size();
}

mfxU32 LoaderCtxVPL::GetSearchPathsCurrentDir(std::list<STRING_TYPE> &searchDirs) {
    searchDirs.clear();

#if defined(_WIN32) || defined(_WIN64)
    CHAR_TYPE currDir[MAX_VPL_SEARCH_PATH] = L"";
    if (GetCurrentDirectoryW(MAX_VPL_SEARCH_PATH, currDir)) {
        searchDirs.push_back(currDir);
    }
#else
    CHAR_TYPE currDir[MAX_VPL_SEARCH_PATH] = "";
    if (getcwd(currDir, MAX_VPL_SEARCH_PATH)) {
        searchDirs.push_back(currDir);
    }
#endif

    return (mfxU32)searchDirs.size();
}

// get legacy MSDK dispatcher search paths
// see "oneVPL Session" section in spec
mfxU32 LoaderCtxVPL::GetSearchPathsLegacy(std::list<STRING_TYPE> &searchDirs) {
    searchDirs.clear();

#if defined(_WIN32) || defined(_WIN64)
    mfxStatus sts = MFX_ERR_UNSUPPORTED;
    STRING_TYPE msdkPath;

    // get path to Windows driver store (MSDK)
    for (mfxU32 adapterID = 0; adapterID <= MAX_WINDOWS_ADAPTER_ID; adapterID++) {
        msdkPath.clear();
        sts = MFX::MFXLibraryIterator::GetDriverStoreDir(msdkPath,
                                                         MAX_VPL_SEARCH_PATH,
                                                         adapterID,
                                                         MFX::MFX_DRIVER_STORE);
        if (sts == MFX_ERR_NONE)
            searchDirs.push_back(msdkPath);
    }

    // get path via dispatcher regkey - HKCU
    msdkPath.clear();
    sts = MFX::MFXLibraryIterator::GetRegkeyDir(msdkPath,
                                                MAX_VPL_SEARCH_PATH,
                                                MFX::MFX_CURRENT_USER_KEY);
    if (sts == MFX_ERR_NONE)
        searchDirs.push_back(msdkPath);

    // get path via dispatcher regkey - HKLM
    msdkPath.clear();
    sts = MFX::MFXLibraryIterator::GetRegkeyDir(msdkPath,
                                                MAX_VPL_SEARCH_PATH,
                                                MFX::MFX_LOCAL_MACHINE_KEY);
    if (sts == MFX_ERR_NONE)
        searchDirs.push_back(msdkPath);

    // get path to %windir%\system32 and %windir%\syswow64
    std::list<STRING_TYPE> winSysDir;
    ParseEnvSearchPaths(L"windir", winSysDir);

    // should resolve to a single directory, otherwise something went wrong
    if (winSysDir.size() == 1) {
        msdkPath = winSysDir.front() + L"\\system32";
        searchDirs.push_back(msdkPath);

        msdkPath = winSysDir.front() + L"\\syswow64";
        searchDirs.push_back(msdkPath);
    }

#else
    // MSDK open-source installation directories
    searchDirs.push_back("/opt/intel/mediasdk/lib");
    searchDirs.push_back("/opt/intel/mediasdk/lib64");
#endif

    return (mfxU32)searchDirs.size();
}

mfxU32 LoaderCtxVPL::GetSearchPathsSystemDefault(std::list<STRING_TYPE> &searchDirs) {
    searchDirs.clear();

#ifdef __linux__
    // Add the standard path for libmfx1 install in Ubuntu
    searchDirs.push_back("/usr/lib/x86_64-linux-gnu");

    // Add other default paths
    searchDirs.push_back("/lib");
    searchDirs.push_back("/usr/lib");
    searchDirs.push_back("/lib64");
    searchDirs.push_back("/usr/lib64");
#endif

    return (mfxU32)searchDirs.size();
}

// search for implementations of oneAPI Video Processing Library (oneVPL)
//   according to the rules in the spec
mfxStatus LoaderCtxVPL::BuildListOfCandidateLibs() {
    DISP_LOG_FUNCTION(&m_dispLog);

    mfxStatus sts = MFX_ERR_NONE;

    STRING_TYPE emptyPath; // default construction = empty
    std::list<STRING_TYPE> searchDirList;
    std::list<STRING_TYPE>::iterator it;

#if defined(_WIN32) || defined(_WIN64)
    // first priority: Windows driver store
    searchDirList.clear();
    GetSearchPathsDriverStore(searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_01);
        it++;
    }

    // second priority: path to current executable
    searchDirList.clear();
    GetSearchPathsCurrentExe(searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_02);
        it++;
    }

    // third priority: current working directory
    searchDirList.clear();
    GetSearchPathsCurrentDir(searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_03);
        it++;
    }

    // fourth priority: PATH environment variable
    searchDirList.clear();
    ParseEnvSearchPaths(L"PATH", searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_04);
        it++;
    }

    // fifth priority: ONEVPL_SEARCH_PATH environment variable
    searchDirList.clear();
    ParseEnvSearchPaths(L"ONEVPL_SEARCH_PATH", searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_05);
        it++;
    }

    // lowest priority: legacy MSDK installation
    searchDirList.clear();
    GetSearchPathsLegacy(searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_LEGACY);
        it++;
    }
#else
    // first priority: LD_LIBRARY_PATH environment variable
    searchDirList.clear();
    ParseEnvSearchPaths("LD_LIBRARY_PATH", searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_01);
        it++;
    }

    // second priority: Linux default paths
    searchDirList.clear();
    GetSearchPathsSystemDefault(searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_03);
        it++;
    }

    // third priority: current working directory
    searchDirList.clear();
    GetSearchPathsCurrentDir(searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_04);
        it++;
    }

    // fourth priority: ONEVPL_SEARCH_PATH environment variable
    searchDirList.clear();
    ParseEnvSearchPaths("ONEVPL_SEARCH_PATH", searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_05);
        it++;
    }

    // lowest priority: legacy MSDK installation
    searchDirList.clear();
    GetSearchPathsLegacy(searchDirList);
    it = searchDirList.begin();
    while (it != searchDirList.end()) {
        STRING_TYPE nextDir = (*it);
        sts                 = SearchDirForLibs(nextDir, m_libInfoList, LIB_PRIORITY_LEGACY);
        it++;
    }
#endif

    return sts;
}

// return number of valid libraries found
mfxU32 LoaderCtxVPL::CheckValidLibraries() {
    DISP_LOG_FUNCTION(&m_dispLog);

    LibInfo *msdkLibBest = nullptr;

    // load all libraries
    std::list<LibInfo *>::iterator it = m_libInfoList.begin();
    while (it != m_libInfoList.end()) {
        mfxU32 i         = 0;
        LibInfo *libInfo = (*it);
        mfxStatus sts    = MFX_ERR_NONE;

        // load DLL
        sts = LoadSingleLibrary(libInfo);

        // load video functions: pointers to exposed functions
        if (sts == MFX_ERR_NONE && libInfo->hModuleVPL) {
            for (i = 0; i < NumVPLFunctions; i += 1) {
                VPLFunctionPtr pProc =
                    (VPLFunctionPtr)GetFunctionAddr(libInfo->hModuleVPL, FunctionDesc2[i].pName);
                if (pProc)
                    libInfo->vplFuncTable[i] = pProc;
            }
        }

        // all runtime libraries with API >= 2.0 must export MFXInitialize()
        // validation of additional functions vs. API version takes place
        //   during UpdateValidImplList() since the minimum API version requested
        //   by application is not known yet (use SetConfigFilterProperty)
        if (libInfo->vplFuncTable[IdxMFXInitialize] &&
            libInfo->libPriority != LIB_PRIORITY_LEGACY) {
            libInfo->libType = LibTypeVPL;
            it++;
            continue;
        }

        // not a valid 2.x runtime - check for 1.x API (legacy caps query)
        i = 0;
        if (sts == MFX_ERR_NONE && libInfo->hModuleVPL) {
            if (libInfo->libNameFull.find(MSDK_LIB_NAME) != std::string::npos) {
                // legacy runtime must be named libmfxhw64 (or 32)
                for (i = 0; i < NumMSDKFunctions; i += 1) {
                    VPLFunctionPtr pProc =
                        (VPLFunctionPtr)GetFunctionAddr(libInfo->hModuleVPL,
                                                        MSDKCompatFunctions[i].pName);
                    if (pProc)
                        libInfo->msdkFuncTable[i] = pProc;
                    else
                        break;
                }
            }
        }

        // check if all of the required MSDK functions were found
        //   and this is valid library (can create session, query version)
        if (i == NumMSDKFunctions) {
            sts = LoaderCtxMSDK::QueryAPIVersion(libInfo->libNameFull, &(libInfo->msdkVersion));

            if (sts == MFX_ERR_NONE) {
                libInfo->libType = LibTypeMSDK;
                if (msdkLibBest == nullptr ||
                    (libInfo->msdkVersion.Version > msdkLibBest->msdkVersion.Version)) {
                    msdkLibBest = libInfo;
                }

                it++;
                continue;
            }
        }

        // required functions missing from DLL, or DLL failed to load
        // remove this library from the list of options
        UnloadSingleLibrary(libInfo);
        it = m_libInfoList.erase(it);
    }

    // prune duplicate MSDK libraries (only keep one with highest API version)
    it = m_libInfoList.begin();
    while (it != m_libInfoList.end()) {
        LibInfo *libInfo = (*it);

        if (libInfo->libType == LibTypeMSDK && libInfo != msdkLibBest) {
            UnloadSingleLibrary(libInfo);
            it = m_libInfoList.erase(it);
        }
        else {
            it++;
        }
    }

    // number of valid oneVPL libs
    return (mfxU32)m_libInfoList.size();
}

VPLFunctionPtr LoaderCtxVPL::GetFunctionAddr(void *hModuleVPL, const char *pName) {
    VPLFunctionPtr pProc = nullptr;

    if (hModuleVPL) {
#if defined(_WIN32) || defined(_WIN64)
        pProc = (VPLFunctionPtr)MFX::mfx_dll_get_addr(hModuleVPL, pName);
#else
        pProc = (VPLFunctionPtr)dlsym(hModuleVPL, pName);
#endif
    }

    return pProc;
}

// load single runtime
mfxStatus LoaderCtxVPL::LoadSingleLibrary(LibInfo *libInfo) {
    if (!libInfo)
        return MFX_ERR_NULL_PTR;

#if defined(_WIN32) || defined(_WIN64)
    libInfo->hModuleVPL = MFX::mfx_dll_load(libInfo->libNameFull.c_str());
#else
    libInfo->hModuleVPL = dlopen(libInfo->libNameFull.c_str(), RTLD_LOCAL | RTLD_NOW);
#endif

    if (!libInfo->hModuleVPL)
        return MFX_ERR_NOT_FOUND;

    return MFX_ERR_NONE;
}

// unload single runtime
mfxStatus LoaderCtxVPL::UnloadSingleLibrary(LibInfo *libInfo) {
    if (libInfo) {
        if (libInfo->hModuleVPL) {
#if defined(_WIN32) || defined(_WIN64)
            MFX::mfx_dll_free(libInfo->hModuleVPL);
#else
            dlclose(libInfo->hModuleVPL);
#endif
        }
        delete libInfo;
        return MFX_ERR_NONE;
    }
    else {
        return MFX_ERR_INVALID_HANDLE;
    }
}

// iterate over all implementation runtimes
// unload DLL's and free memory
mfxStatus LoaderCtxVPL::UnloadAllLibraries() {
    DISP_LOG_FUNCTION(&m_dispLog);

    std::list<ImplInfo *>::iterator it2 = m_implInfoList.begin();
    while (it2 != m_implInfoList.end()) {
        ImplInfo *implInfo = (*it2);

        if (implInfo) {
            UnloadSingleImplementation(implInfo);
        }
        it2++;
    }

    // lastly, unload and destroy LibInfo for each library
    std::list<LibInfo *>::iterator it = m_libInfoList.begin();
    while (it != m_libInfoList.end()) {
        LibInfo *libInfo = (*it);

        if (libInfo) {
            UnloadSingleLibrary(libInfo);
        }
        it++;
    }

    return MFX_ERR_NONE;
}

// unload single implementation
// each runtime library may contain 1 or more implementations
mfxStatus LoaderCtxVPL::UnloadSingleImplementation(ImplInfo *implInfo) {
    if (implInfo && implInfo->libInfo) {
        LibInfo *libInfo     = implInfo->libInfo;
        VPLFunctionPtr pFunc = libInfo->vplFuncTable[IdxMFXReleaseImplDescription];

        // call MFXReleaseImplDescription() for this implementation if it
        //   was never called by the application
        // this is a valid scenario, e.g. app did not call MFXEnumImplementations()
        //   and just used the first available implementation provided by dispatcher
        if (libInfo->libType == LibTypeVPL) {
            if (implInfo->implDesc) {
                // MFX_IMPLCAPS_IMPLDESCSTRUCTURE;
                (*(mfxStatus(MFX_CDECL *)(mfxHDL))pFunc)(implInfo->implDesc);
                implInfo->implDesc = nullptr;
            }

            if (implInfo->implFuncs) {
                // MFX_IMPLCAPS_IMPLEMENTEDFUNCTIONS;
                (*(mfxStatus(MFX_CDECL *)(mfxHDL))pFunc)(implInfo->implFuncs);
                implInfo->implFuncs = nullptr;
            }

            // nothing to do if (capsFormat == MFX_IMPLCAPS_IMPLPATH) since no new memory was allocated
        }
        delete implInfo;
        return MFX_ERR_NONE;
    }
    else {
        return MFX_ERR_INVALID_HANDLE;
    }
}

// check that all functions for this API version are available in library
mfxStatus LoaderCtxVPL::ValidateAPIExports(VPLFunctionPtr *vplFuncTable,
                                           mfxVersion reportedVersion) {
    for (mfxU32 i = 0; i < NumVPLFunctions; i += 1) {
        if (!vplFuncTable[i] && (FunctionDesc2[i].apiVersion.Version <= reportedVersion.Version))
            return MFX_ERR_UNSUPPORTED;
    }

    return MFX_ERR_NONE;
}

// convert full path into char* for MFX_IMPLCAPS_IMPLPATH query
mfxStatus LoaderCtxVPL::UpdateImplPath(LibInfo *libInfo) {
#if defined(_WIN32) || defined(_WIN64)
    // Windows - strings are 16-bit
    size_t nCvt = 0;
    if (wcstombs_s(&nCvt,
                   libInfo->implCapsPath,
                   sizeof(libInfo->implCapsPath),
                   libInfo->libNameFull.c_str(),
                   _TRUNCATE)) {
        // unknown error - set to empty string
        libInfo->implCapsPath[0] = 0;
        return MFX_ERR_UNSUPPORTED;
    }
#else
    // Linux - strings are 8-bit
    strncpy(libInfo->implCapsPath, libInfo->libNameFull.c_str(), sizeof(libInfo->implCapsPath) - 1);
#endif

    return MFX_ERR_NONE;
}

bool LoaderCtxVPL::IsValidX86GPU(ImplInfo *implInfo, mfxU32 &deviceID, mfxU32 &adapterIdx) {
    mfxImplDescription *implDesc = (mfxImplDescription *)(implInfo->implDesc);

    if (implInfo->validImplIdx >= 0 && implDesc->VendorID == 0x8086 &&
        implDesc->Impl == MFX_IMPL_TYPE_HARDWARE) {
        // verify that DeviceID is a valid format for x86 GPU
        // either "DeviceID" (hex) or "DeviceID/AdapterIdx" (hex/dec)
        return ConfigCtxVPL::ParseDeviceIDx86(implDesc->Dev.DeviceID, deviceID, adapterIdx);
    }

    return false;
}

// query capabilities of all valid libraries
//   and add to list for future calls to EnumImplementations()
//   as well as filtering by functionality
// assume MFX_IMPLCAPS_IMPLDESCSTRUCTURE is the only format supported
mfxStatus LoaderCtxVPL::QueryLibraryCaps() {
    DISP_LOG_FUNCTION(&m_dispLog);

    mfxStatus sts = MFX_ERR_NONE;

    std::list<LibInfo *>::iterator it = m_libInfoList.begin();
    while (it != m_libInfoList.end()) {
        LibInfo *libInfo = (*it);

        if (libInfo->libType == LibTypeVPL) {
            VPLFunctionPtr pFunc = libInfo->vplFuncTable[IdxMFXQueryImplsDescription];

            // call MFXQueryImplsDescription() for this implementation
            // return handle to description in requested format
            mfxHDL *hImpl;
            mfxU32 numImpls = 0;
            hImpl           = (*(mfxHDL * (MFX_CDECL *)(mfxImplCapsDeliveryFormat, mfxU32 *))
                         pFunc)(MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &numImpls);

            // validate description pointer for each implementation
            bool b_isValidDesc = true;
            if (!hImpl) {
                b_isValidDesc = false;
            }
            else {
                for (mfxU32 i = 0; i < numImpls; i++) {
                    if (!hImpl[i]) {
                        b_isValidDesc = false;
                        break;
                    }
                }
            }

            if (!b_isValidDesc) {
                // the required function is implemented incorrectly
                // remove this library from the list of valid libraries
                UnloadSingleLibrary(libInfo);
                it = m_libInfoList.erase(it);
                continue;
            }

            // query for list of implemented functions
            // prior to API 2.2, this will return null since the format was not defined yet
            //   so we need to check whether the returned handle is valid before attempting to use it
            mfxHDL *hImplFuncs   = nullptr;
            mfxU32 numImplsFuncs = 0;
            hImplFuncs           = (*(mfxHDL * (MFX_CDECL *)(mfxImplCapsDeliveryFormat, mfxU32 *))
                              pFunc)(MFX_IMPLCAPS_IMPLEMENTEDFUNCTIONS, &numImplsFuncs);

            // save user-friendly path for MFX_IMPLCAPS_IMPLPATH query (API >= 2.4)
            UpdateImplPath(libInfo);

            for (mfxU32 i = 0; i < numImpls; i++) {
                ImplInfo *implInfo = new ImplInfo;
                if (!implInfo)
                    return MFX_ERR_MEMORY_ALLOC;

                // library which contains this implementation
                implInfo->libInfo = libInfo;

                // implementation descriptor returned from runtime
                implInfo->implDesc = hImpl[i];

                // implemented function description, if available
                if (hImplFuncs && i < numImplsFuncs)
                    implInfo->implFuncs = hImplFuncs[i];

                // fill out mfxInitParam struct for when we call MFXInitEx
                //   in CreateSession()
                mfxImplDescription *implDesc = reinterpret_cast<mfxImplDescription *>(hImpl[i]);

                // fill out mfxInitializationParam for use in CreateSession (MFXInitialize path)
                memset(&(implInfo->vplParam), 0, sizeof(mfxInitializationParam));

                // default mode for this impl
                // this may be changed later by MFXSetConfigFilterProperty(AccelerationMode)
                implInfo->vplParam.AccelerationMode = implDesc->AccelerationMode;

                implInfo->version = implDesc->ApiVersion;

                // save local index for this library
                implInfo->libImplIdx = i;

                // validate that library exports all required functions for the reported API version
                if (ValidateAPIExports(libInfo->vplFuncTable, implInfo->version)) {
                    UnloadSingleImplementation(implInfo);
                    continue;
                }

                // initially all libraries have a valid, sequential value (>= 0)
                // list of valid libraries is updated with every call to MFXSetConfigFilterProperty()
                //   (see UpdateValidImplList)
                // libraries that do not support all the required props get a value of -1, and
                //   indexing of the valid libs is recalculated from 0,1,...
                implInfo->validImplIdx = m_implIdxNext++;

                // add implementation to overall list
                m_implInfoList.push_back(implInfo);
            }
        }
        else if (libInfo->libType == LibTypeMSDK) {
            // save user-friendly path for MFX_IMPLCAPS_IMPLPATH query (API >= 2.4)
            UpdateImplPath(libInfo);

            mfxU32 numImplMSDK = 0;
            for (mfxU32 i = 0; i < MAX_NUM_IMPL_MSDK; i++) {
                mfxImplDescription *implDesc       = nullptr;
                mfxImplementedFunctions *implFuncs = nullptr;

                LoaderCtxMSDK *msdkCtx = &(libInfo->msdkCtx[i]);

                sts = msdkCtx->QueryMSDKCaps(libInfo->libNameFull, &implDesc, &implFuncs, i);

                if (sts || !implDesc || !implFuncs) {
                    // this adapter (i) is not supported
                    continue;
                }

                ImplInfo *implInfo = new ImplInfo;
                if (!implInfo)
                    return MFX_ERR_MEMORY_ALLOC;

                // library which contains this implementation
                implInfo->libInfo = libInfo;

                // implementation descriptor returned from runtime
                implInfo->implDesc = implDesc;

                // implemented function description, if available
                implInfo->implFuncs = implFuncs;

                // fill out mfxInitializationParam for use in CreateSession (MFXInitialize path)
                memset(&(implInfo->vplParam), 0, sizeof(mfxInitializationParam));

                // default mode for this impl
                // this may be changed later by MFXSetConfigFilterProperty(AccelerationMode)
                implInfo->vplParam.AccelerationMode = implDesc->AccelerationMode;

                implInfo->version = implDesc->ApiVersion;

                // adapter number
                implInfo->msdkImplIdx = i;

                // save local index for this library
                implInfo->libImplIdx = 0;

                // initially all libraries have a valid, sequential value (>= 0)
                // list of valid libraries is updated with every call to MFXSetConfigFilterProperty()
                //   (see UpdateValidImplList)
                // libraries that do not support all the required props get a value of -1, and
                //   indexing of the valid libs is recalculated from 0,1,...
                implInfo->validImplIdx = m_implIdxNext++;

                // add implementation to overall list
                m_implInfoList.push_back(implInfo);

                // update number of valid MSDK adapters
                numImplMSDK++;

#ifdef __linux__
                // currently only one adapter on Linux (avoid multiple copies)
                break;
#endif
            }

            if (numImplMSDK == 0) {
                // error loading MSDK library in compatibility mode - remove from list
                UnloadSingleLibrary(libInfo);
                it = m_libInfoList.erase(it);
                continue;
            }
        }
        it++;
    }

    if (!m_implInfoList.empty()) {
        std::list<ImplInfo *>::iterator it2 = m_implInfoList.begin();
        while (it2 != m_implInfoList.end()) {
            ImplInfo *implInfo = (*it2);

            mfxU32 deviceID   = 0;
            mfxU32 adapterIdx = 0;
            if (IsValidX86GPU(implInfo, deviceID, adapterIdx)) {
                // save the adapterIdx for any x86 GPU devices (may be used later for filtering)
                implInfo->adapterIdx = adapterIdx;
            }

            // per spec: if both VPL (HW) and MSDK are installed on the same system, only load
            //   the VPL library (mark MSDK as invalid)
            if (implInfo->libInfo->libType == LibTypeMSDK) {
                auto vplIdx = std::find_if(
                    m_implInfoList.begin(),
                    m_implInfoList.end(),

                    [](const ImplInfo *t) {
                        mfxImplDescription *implDesc = (mfxImplDescription *)(t->implDesc);

                        return (t->libInfo->libType == LibTypeVPL && implDesc != nullptr &&
                                implDesc->Impl == MFX_IMPL_TYPE_HARDWARE);
                    });

                if (vplIdx != m_implInfoList.end())
                    implInfo->validImplIdx = -1;
            }

            it2++;
        }

        // sort valid implementations according to priority rules in spec
        PrioritizeImplList();
    }

    return m_implInfoList.empty() ? MFX_ERR_UNSUPPORTED : MFX_ERR_NONE;
}

// query implementation i
mfxStatus LoaderCtxVPL::QueryImpl(mfxU32 idx, mfxImplCapsDeliveryFormat format, mfxHDL *idesc) {
    DISP_LOG_FUNCTION(&m_dispLog);

    *idesc = nullptr;

    std::list<ImplInfo *>::iterator it = m_implInfoList.begin();
    while (it != m_implInfoList.end()) {
        ImplInfo *implInfo = (*it);
        if (implInfo->validImplIdx == (mfxI32)idx) {
            if (format == MFX_IMPLCAPS_IMPLDESCSTRUCTURE) {
                *idesc = implInfo->implDesc;
            }
            else if (format == MFX_IMPLCAPS_IMPLEMENTEDFUNCTIONS) {
                *idesc = implInfo->implFuncs;
            }
            else if (format == MFX_IMPLCAPS_IMPLPATH) {
                *idesc = implInfo->libInfo->implCapsPath;
            }

            // implementation found, but requested query format is not supported
            if (*idesc == nullptr)
                return MFX_ERR_UNSUPPORTED;

            return MFX_ERR_NONE;
        }
        it++;
    }

    // invalid idx
    return MFX_ERR_NOT_FOUND;
}

mfxStatus LoaderCtxVPL::ReleaseImpl(mfxHDL idesc) {
    DISP_LOG_FUNCTION(&m_dispLog);

    mfxStatus sts = MFX_ERR_NONE;

    if (idesc == nullptr)
        return MFX_ERR_NULL_PTR;

    // all we get from the application is a handle to the descriptor,
    //   not the implementation associated with it, so we search
    //   through the full list until we find a match
    std::list<ImplInfo *>::iterator it = m_implInfoList.begin();
    while (it != m_implInfoList.end()) {
        ImplInfo *implInfo                   = (*it);
        mfxImplCapsDeliveryFormat capsFormat = (mfxImplCapsDeliveryFormat)0; // unknown format

        // determine type of descriptor so we know which handle to
        //   invalidate in the Loader context
        if (implInfo->implDesc == idesc) {
            capsFormat = MFX_IMPLCAPS_IMPLDESCSTRUCTURE;
        }
        else if (implInfo->implFuncs == idesc) {
            capsFormat = MFX_IMPLCAPS_IMPLEMENTEDFUNCTIONS;
        }
        else if (implInfo->libInfo->implCapsPath == idesc) {
            capsFormat = MFX_IMPLCAPS_IMPLPATH;
        }
        else {
            // no match - try next implementation
            it++;
            continue;
        }

        // if true, do not actually call ReleaseImplDescription() until
        //   MFXUnload() --> UnloadAllLibraries()
        // this permits the application to call Enum/CreateSession/DispRelease multiple
        //   times on the same implementation
        if (m_bKeepCapsUntilUnload)
            return MFX_ERR_NONE;

        // LibTypeMSDK does not require calling a release function
        if (implInfo->libInfo->libType == LibTypeVPL) {
            // call MFXReleaseImplDescription() for this implementation
            VPLFunctionPtr pFunc = implInfo->libInfo->vplFuncTable[IdxMFXReleaseImplDescription];

            if (capsFormat == MFX_IMPLCAPS_IMPLDESCSTRUCTURE) {
                sts                = (*(mfxStatus(MFX_CDECL *)(mfxHDL))pFunc)(implInfo->implDesc);
                implInfo->implDesc = nullptr;
            }
            else if (capsFormat == MFX_IMPLCAPS_IMPLEMENTEDFUNCTIONS) {
                sts                 = (*(mfxStatus(MFX_CDECL *)(mfxHDL))pFunc)(implInfo->implFuncs);
                implInfo->implFuncs = nullptr;
            }

            // nothing to do if (capsFormat == MFX_IMPLCAPS_IMPLPATH) since no new memory was allocated
        }

        return sts;
    }

    // did not find a matching handle - should not happen
    return MFX_ERR_INVALID_HANDLE;
}

mfxStatus LoaderCtxVPL::UpdateValidImplList(void) {
    DISP_LOG_FUNCTION(&m_dispLog);

    mfxStatus sts = MFX_ERR_NONE;

    mfxI32 validImplIdx = 0;

    // iterate over all libraries and update list of those that
    //   meet current current set of config props
    std::list<ImplInfo *>::iterator it = m_implInfoList.begin();
    while (it != m_implInfoList.end()) {
        ImplInfo *implInfo = (*it);

        // already invalidated by previous filter
        if (implInfo->validImplIdx == -1) {
            it++;
            continue;
        }

        // compare caps from this library vs. config filters
        sts = ConfigCtxVPL::ValidateConfig((mfxImplDescription *)implInfo->implDesc,
                                           (mfxImplementedFunctions *)implInfo->implFuncs,
                                           m_configCtxList,
                                           implInfo->libInfo->libType,
                                           &m_specialConfig);

        // check special filter properties which are not part of mfxImplDescription
        if (m_specialConfig.bIsSet_dxgiAdapterIdx &&
            (m_specialConfig.dxgiAdapterIdx != implInfo->adapterIdx)) {
            sts = MFX_ERR_UNSUPPORTED;
        }

        if (sts == MFX_ERR_NONE) {
            // library supports all required properties
            implInfo->validImplIdx = validImplIdx++;
        }
        else {
            // library does not support required props, do not include in list for
            //   MFXEnumImplementations() or MFXCreateSession()
            implInfo->validImplIdx = -1;
        }

        it++;
    }

    // re-sort valid implementations according to priority rules in spec
    PrioritizeImplList();

    return MFX_ERR_NONE;
}

// From specification section "oneVPL Session":
//
// When the dispatcher searches for the implementation, it uses the following priority rules
//  1) Hardware implementation has priority over software implementation.
//  2) General hardware implementation has priority over VSI hardware implementation.
//  3) Highest API version has higher priority over lower API version.
//  4) Search path priority: lower values = higher priority
mfxStatus LoaderCtxVPL::PrioritizeImplList(void) {
    DISP_LOG_FUNCTION(&m_dispLog);

    // stable sort - work from lowest to highest priority conditions

    // 4 - sort by search path priority
    m_implInfoList.sort([](const ImplInfo *impl1, const ImplInfo *impl2) {
        // prioritize lowest value for libPriority (1 = highest priority)
        return (impl1->libInfo->libPriority < impl2->libInfo->libPriority);
    });

    // 3 - sort by API version
    m_implInfoList.sort([](const ImplInfo *impl1, const ImplInfo *impl2) {
        mfxImplDescription *implDesc1 = (mfxImplDescription *)(impl1->implDesc);
        mfxImplDescription *implDesc2 = (mfxImplDescription *)(impl2->implDesc);

        // prioritize greatest API version
        return (implDesc1->ApiVersion.Version > implDesc2->ApiVersion.Version);
    });

    // 2 - sort by general HW vs. VSI
    m_implInfoList.sort([](const ImplInfo *impl1, const ImplInfo *impl2) {
        mfxImplDescription *implDesc1 = (mfxImplDescription *)(impl1->implDesc);
        mfxImplDescription *implDesc2 = (mfxImplDescription *)(impl2->implDesc);

        // prioritize general HW accelerator over VSI (if none, i.e. SW, will be sorted in final step)
        return (implDesc1->AccelerationMode != MFX_ACCEL_MODE_VIA_HDDLUNITE &&
                implDesc2->AccelerationMode == MFX_ACCEL_MODE_VIA_HDDLUNITE);
    });

    // 1 - sort by implementation type (HW > SW)
    m_implInfoList.sort([](const ImplInfo *impl1, const ImplInfo *impl2) {
        mfxImplDescription *implDesc1 = (mfxImplDescription *)(impl1->implDesc);
        mfxImplDescription *implDesc2 = (mfxImplDescription *)(impl2->implDesc);

        // prioritize greatest Impl value (HW = 2, SW = 1)
        return (implDesc1->Impl > implDesc2->Impl);
    });

    // final pass - update index to match new priority order
    // validImplIdx will be the index associated with MFXEnumImplememntations()
    mfxI32 validImplIdx                = 0;
    std::list<ImplInfo *>::iterator it = m_implInfoList.begin();
    while (it != m_implInfoList.end()) {
        ImplInfo *implInfo = (*it);

        if (implInfo->validImplIdx >= 0) {
            implInfo->validImplIdx = validImplIdx++;
        }
        it++;
    }

    return MFX_ERR_NONE;
}

mfxStatus LoaderCtxVPL::CreateSession(mfxU32 idx, mfxSession *session) {
    DISP_LOG_FUNCTION(&m_dispLog);

    mfxStatus sts = MFX_ERR_NONE;

    // find library with given implementation index
    // list of valid implementations (and associated indices) is updated
    //   every time a filter property is added/modified
    std::list<ImplInfo *>::iterator it = m_implInfoList.begin();
    while (it != m_implInfoList.end()) {
        ImplInfo *implInfo = (*it);

        if (implInfo->validImplIdx == (mfxI32)idx) {
            LibInfo *libInfo = implInfo->libInfo;
            mfxU16 deviceID  = 0;

            if (sts == MFX_ERR_NONE) {
                // pass VendorImplID for this implementation (disambiguate if one
                //   library contains multiple implementations)
                mfxImplDescription *implDesc = (mfxImplDescription *)(implInfo->implDesc);

                // should not happen in normal circumstances, but avoid using nullptr if something went wrong
                if (!implDesc)
                    return MFX_ERR_NULL_PTR;

                implInfo->vplParam.VendorImplID = implDesc->VendorImplID;

                // set any special parameters passed in via SetConfigProperty
                // if application did not specify accelerationMode, use default
                if (m_specialConfig.bIsSet_accelerationMode)
                    implInfo->vplParam.AccelerationMode = m_specialConfig.accelerationMode;

                mfxIMPL msdkImpl = 0;
                if (libInfo->libType == LibTypeMSDK) {
                    if (implInfo->vplParam.AccelerationMode == MFX_ACCEL_MODE_VIA_D3D9)
                        msdkImpl = libInfo->msdkCtx[implInfo->msdkImplIdx].m_msdkAdapterD3D9;
                    else
                        msdkImpl = libInfo->msdkCtx[implInfo->msdkImplIdx].m_msdkAdapter;
                }

                // initialize this library via MFXInitialize or else fail
                //   (specify full path to library)
                sts = MFXInitEx2(implInfo->version,
                                 implInfo->vplParam,
                                 msdkImpl,
                                 session,
                                 &deviceID,
                                 (CHAR_TYPE *)libInfo->libNameFull.c_str());
            }

            // optionally call MFXSetHandle() if present via SetConfigProperty
            if (sts == MFX_ERR_NONE && m_specialConfig.bIsSet_deviceHandleType &&
                m_specialConfig.bIsSet_deviceHandle && m_specialConfig.deviceHandleType &&
                m_specialConfig.deviceHandle) {
                sts = MFXVideoCORE_SetHandle(*session,
                                             m_specialConfig.deviceHandleType,
                                             m_specialConfig.deviceHandle);
            }

            return sts;
        }
        it++;
    }

    // invalid idx
    return MFX_ERR_NOT_FOUND;
}

ConfigCtxVPL *LoaderCtxVPL::AddConfigFilter() {
    DISP_LOG_FUNCTION(&m_dispLog);

    // create new config filter context and add
    //   to list associated with this loader
    std::unique_ptr<ConfigCtxVPL> configCtx;
    try {
        configCtx.reset(new ConfigCtxVPL{});
    }
    catch (...) {
        return nullptr;
    }

    ConfigCtxVPL *config   = (ConfigCtxVPL *)(configCtx.release());
    config->m_parentLoader = this;

    m_configCtxList.push_back(config);

    return config;
}

mfxStatus LoaderCtxVPL::FreeConfigFilters() {
    DISP_LOG_FUNCTION(&m_dispLog);

    std::list<ConfigCtxVPL *>::iterator it = m_configCtxList.begin();

    while (it != m_configCtxList.end()) {
        ConfigCtxVPL *config = (*it);
        if (config)
            delete config;
        it++;
    }

    return MFX_ERR_NONE;
}

mfxStatus LoaderCtxVPL::InitDispatcherLog() {
    std::string strLogEnabled, strLogFile;

#if defined(_WIN32) || defined(_WIN64)
    DWORD err;

    char logEnabled[MAX_VPL_SEARCH_PATH] = "";
    err = GetEnvironmentVariable("ONEVPL_DISPATCHER_LOG", logEnabled, MAX_VPL_SEARCH_PATH);
    if (err == 0 || err >= MAX_VPL_SEARCH_PATH)
        return MFX_ERR_UNSUPPORTED; // environment variable not defined or string too long

    strLogEnabled = logEnabled;

    char logFile[MAX_VPL_SEARCH_PATH] = "";
    err = GetEnvironmentVariable("ONEVPL_DISPATCHER_LOG_FILE", logFile, MAX_VPL_SEARCH_PATH);
    if (err == 0 || err >= MAX_VPL_SEARCH_PATH) {
        // nothing to do - strLogFile is an empty string
    }
    else {
        strLogFile = logFile;
    }

#else
    const char *logEnabled = std::getenv("ONEVPL_DISPATCHER_LOG");
    if (!logEnabled)
        return MFX_ERR_UNSUPPORTED;

    strLogEnabled = logEnabled;

    const char *logFile = std::getenv("ONEVPL_DISPATCHER_LOG_FILE");
    if (logFile)
        strLogFile = logFile;
#endif

    if (strLogEnabled != "ON")
        return MFX_ERR_UNSUPPORTED;

    // currently logLevel is either 0 or non-zero
    // additional levels will be added with future API updates
    return m_dispLog.Init(1, strLogFile);
}

// public function to return logger object
// allows logging from C API functions outside of loaderCtx
DispatcherLogVPL *LoaderCtxVPL::GetLogger() {
    return &m_dispLog;
}
