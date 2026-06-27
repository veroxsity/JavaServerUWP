#include "Server.h"
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

std::atomic<bool> g_shutdownRequested(false);
std::atomic<bool> g_serverRunning(false);
std::atomic<bool> g_appSuspending(false);
std::atomic<bool> g_appInBackground(false);
std::atomic<bool> g_extendedExecutionRequested(false);
HANDLE g_stdinWritePipe = INVALID_HANDLE_VALUE;
std::wstring g_localStateDir;
std::mutex g_extendedExecutionMutex;
ComPtr<IExtendedExecutionForegroundSession> g_extendedExecutionSession;

std::wstring ResolveLocalStateDir() {
    if (!g_localStateDir.empty()) {
        return g_localStateDir;
    }

    ComPtr<ABI::Windows::Storage::IApplicationDataStatics> appDataStatics;
    HRESULT hr = RoGetActivationFactory(
        HStringReference(L"Windows.Storage.ApplicationData").Get(),
        IID_PPV_ARGS(&appDataStatics));
    if (SUCCEEDED(hr)) {
        ComPtr<ABI::Windows::Storage::IApplicationData> appData;
        hr = appDataStatics->get_Current(&appData);
        if (SUCCEEDED(hr)) {
            ComPtr<ABI::Windows::Storage::IStorageFolder> localFolder;
            hr = appData->get_LocalFolder(&localFolder);
            if (SUCCEEDED(hr)) {
                ComPtr<ABI::Windows::Storage::IStorageItem> localItem;
                hr = localFolder.As(&localItem);
                if (SUCCEEDED(hr)) {
                    HSTRING hPath;
                    hr = localItem->get_Path(&hPath);
                    if (SUCCEEDED(hr)) {
                        UINT32 len;
                        PCWSTR raw = WindowsGetStringRawBuffer(hPath, &len);
                        g_localStateDir = std::wstring(raw, len);
                        WindowsDeleteString(hPath);
                        return g_localStateDir;
                    }
                }
            }
        }
    }
    WCHAR buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exeDir(buf);
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/") + 1) + L"LocalState";
    CreateDirectoryW(exeDir.c_str(), nullptr);
    g_localStateDir = exeDir;
    return g_localStateDir;
}

void EnsureDirectoryTree(const std::wstring& path) {
    std::wstring current;
    for (size_t i = 0; i < path.size(); ++i) {
        current += path[i];
        if (path[i] == L'\\' || path[i] == L'/' || i == path.size() - 1) {
            CreateDirectoryW(current.c_str(), nullptr);
        }
    }
}
static std::wstring g_launchLogPath;

const std::wstring& ResolveLaunchLogPath() {
    if (!g_launchLogPath.empty()) return g_launchLogPath;
    std::wstring localState = ResolveLocalStateDir();
    if (localState.empty()) return g_launchLogPath;
    std::wstring logsDir = localState + L"\\logs";
    EnsureDirectoryTree(logsDir);
    g_launchLogPath = logsDir + L"\\server_launch.log";
    return g_launchLogPath;
}

void WriteLaunchLog(const wchar_t* msg) {
    const std::wstring& path = ResolveLaunchLogPath();
    if (path.empty()) {
        OutputDebugStringW(msg);
        return;
    }
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"a");
    if (!f) {
        OutputDebugStringW(msg);
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d.%03d] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
    OutputDebugStringW(msg);
}

void WriteLaunchLogF(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    WriteLaunchLog(buf);
}

static LONG WINAPI LogUnhandledException(EXCEPTION_POINTERS* info) {
    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    void* address = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    WriteLaunchLogF(L"!! Unhandled SEH exception code=0x%08X address=%p", code, address);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void LogTerminate() {
    WriteLaunchLog(L"!! std::terminate called");
    abort();
}

static void LogSignal(int sig) {
    WriteLaunchLogF(L"!! CRT signal raised sig=%d", sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void LogInvalidParameter(
    const wchar_t* expression,
    const wchar_t* function,
    const wchar_t* file,
    unsigned int line,
    uintptr_t) {
    WriteLaunchLogF(L"!! invalid parameter function=%ls file=%ls line=%u expression=%ls",
        function ? function : L"(null)",
        file ? file : L"(null)",
        line,
        expression ? expression : L"(null)");
}

void InstallProcessDiagnostics() {
    SetUnhandledExceptionFilter(LogUnhandledException);
    std::set_terminate(LogTerminate);
    signal(SIGABRT, LogSignal);
    signal(SIGFPE, LogSignal);
    signal(SIGILL, LogSignal);
    signal(SIGINT, LogSignal);
    signal(SIGSEGV, LogSignal);
    signal(SIGTERM, LogSignal);
    _set_invalid_parameter_handler(LogInvalidParameter);
    WriteLaunchLog(L"Process diagnostics installed");
}

void RequestForegroundExtendedExecution() {
    if (g_extendedExecutionRequested.exchange(true)) {
        return;
    }

    WriteLaunchLog(L"ExtendedExecution: requesting foreground unconstrained session");

    ComPtr<IInspectable> sessionObj;
    HRESULT hr = RoActivateInstance(
        HStringReference(RuntimeClass_Windows_ApplicationModel_ExtendedExecution_Foreground_ExtendedExecutionForegroundSession).Get(),
        &sessionObj);
    if (FAILED(hr)) {
        WriteLaunchLogF(L"ExtendedExecution: RoActivateInstance failed hr=0x%08X", hr);
        return;
    }

    ComPtr<IExtendedExecutionForegroundSession> session;
    hr = sessionObj.As(&session);
    if (FAILED(hr) || !session) {
        WriteLaunchLogF(L"ExtendedExecution: QueryInterface failed hr=0x%08X", hr);
        return;
    }

    session->put_Reason(ExtendedExecutionForegroundReason_Unconstrained);
    session->put_Description(HStringReference(L"Minecraft Java server").Get());

    {
        std::lock_guard<std::mutex> lock(g_extendedExecutionMutex);
        g_extendedExecutionSession = session;
    }

    ComPtr<__FIAsyncOperation_1_Windows__CApplicationModel__CExtendedExecution__CForeground__CExtendedExecutionForegroundResult> op;
    hr = session->RequestExtensionAsync(&op);
    if (FAILED(hr) || !op) {
        WriteLaunchLogF(L"ExtendedExecution: RequestExtensionAsync failed hr=0x%08X", hr);
        return;
    }

    auto completedHandler = Microsoft::WRL::Callback<
        __FIAsyncOperationCompletedHandler_1_Windows__CApplicationModel__CExtendedExecution__CForeground__CExtendedExecutionForegroundResult>(
        [](__FIAsyncOperation_1_Windows__CApplicationModel__CExtendedExecution__CForeground__CExtendedExecutionForegroundResult* asyncInfo,
           AsyncStatus asyncStatus) -> HRESULT {
            ExtendedExecutionForegroundResult result = ExtendedExecutionForegroundResult_Denied;
            HRESULT resultHr = asyncInfo ? asyncInfo->GetResults(&result) : E_POINTER;
            WriteLaunchLogF(L"ExtendedExecution: completed status=%d hr=0x%08X result=%d",
                static_cast<int>(asyncStatus), resultHr, static_cast<int>(result));
            return S_OK;
        });
    hr = op->put_Completed(completedHandler.Get());
    if (FAILED(hr)) {
        WriteLaunchLogF(L"ExtendedExecution: put_Completed failed hr=0x%08X", hr);
    }
}
bool EnsureDirRecursive(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        EnsureDirRecursive(path.substr(0, slash));
    }
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// fabric remap fails under uwp so preseed exact cache path
void SeedFabricCache(const std::wstring& exeDir, const std::wstring& localStateDir) {
    std::wstring cacheDir = localStateDir + L"\\.fabric\\remappedJars\\minecraft-";
    cacheDir += RuntimeConfig::MC_VERSION;
    cacheDir += L"-";
    cacheDir += RuntimeConfig::FABRIC_LOADER_VERSION;

    std::wstring cacheJar = cacheDir + L"\\server-intermediary.jar";
    std::wstring cacheTmp = cacheJar + L".tmp";
    std::wstring prebuiltJar = exeDir + L"\\prebuilt-remap\\server-intermediary.jar";

    DeleteFileW(cacheTmp.c_str());

    DWORD attr = GetFileAttributesW(cacheJar.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        WriteLaunchLog(L"SeedFabricCache: cache jar already present, skipping copy");
        return;
    }

    if (GetFileAttributesW(prebuiltJar.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteLaunchLogF(L"SeedFabricCache: prebuilt jar not found at %ls", prebuiltJar.c_str());
        return;
    }

    if (!EnsureDirRecursive(cacheDir)) {
        WriteLaunchLogF(L"SeedFabricCache: failed to create %ls (err=%u)", cacheDir.c_str(), GetLastError());
        return;
    }

    if (CopyFileW(prebuiltJar.c_str(), cacheJar.c_str(), TRUE)) {
        WriteLaunchLogF(L"SeedFabricCache: pre-seeded %ls", cacheJar.c_str());
    } else {
        WriteLaunchLogF(L"SeedFabricCache: CopyFile failed err=%u dst=%ls", GetLastError(), cacheJar.c_str());
    }
}
void CollectJars(const std::wstring& root, std::vector<std::wstring>& jars) {
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring fullPath = root + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectJars(fullPath, jars);
        } else {
            std::wstring name(fd.cFileName);
            if (name.size() > 4 && name.substr(name.size() - 4) == L".jar") {
                if (name.find(L"sources") == std::wstring::npos &&
                    name.find(L"javadoc") == std::wstring::npos) {
                    jars.push_back(fullPath);
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

static std::wstring ReadFirstLineW(const std::wstring& path) {
    std::ifstream f(path.c_str());
    std::string s;
    if (f) std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t st = s.find_first_not_of(" \t");
    s = (st == std::string::npos) ? std::string() : s.substr(st);
    return Utf8ToWide(s);
}
static std::string ReadMainClassSlash(const std::wstring& dir) {
    std::string s = WideToUtf8(ReadFirstLineW(dir + L"\\mainclass.txt"));
    if (s.empty()) s = "io.papermc.paperclip.Main";
    for (auto& c : s) if (c == '.') c = '/';
    return s;
}

std::vector<ServerVersion> DiscoverServerVersions(const std::wstring& localStateDir, const std::wstring& exeDir) {
    std::vector<ServerVersion> versions;

    std::wstring bundledJar = exeDir + L"\\server\\paper.jar";
    if (GetFileAttributesW(bundledJar.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ServerVersion v;
        v.id = L"bundled";
        v.label = std::wstring(L"Paper ") + RuntimeConfig::MC_VERSION + L" (bundled)";
        v.jarPath = bundledJar;
        v.mainClass = ReadMainClassSlash(exeDir + L"\\server");
        v.bundled = true;
        versions.push_back(v);
    }

    std::wstring jarsRoot = localStateDir + L"\\server-jars";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((jarsRoot + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring dir = jarsRoot + L"\\" + fd.cFileName;
            std::wstring jar = dir + L"\\paper.jar";
            if (GetFileAttributesW(jar.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::vector<std::wstring> jars;
                CollectJars(dir, jars);
                if (jars.empty()) continue;
                jar = jars[0];
            }
            ServerVersion v;
            v.id = fd.cFileName;
            std::wstring label = ReadFirstLineW(dir + L"\\version.txt");
            v.label = label.empty() ? std::wstring(fd.cFileName) : label;
            v.jarPath = jar;
            v.mainClass = ReadMainClassSlash(dir);
            v.bundled = false;
            versions.push_back(v);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    return versions;
}

static bool RemoveDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring child = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirRecursive(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

bool DeleteServerVersion(const std::wstring& localStateDir, const std::wstring& id) {
    if (id.empty() || id == L"bundled") return false;
    std::wstring dir = localStateDir + L"\\server-jars\\" + id;
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    bool ok = RemoveDirRecursive(dir);
    WriteLaunchLogF(L"DeleteServerVersion: %ls ok=%d", id.c_str(), ok ? 1 : 0);
    return ok;
}

static std::wstring SanitizeInstanceId(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name) {
        bool alnum = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
        if (alnum) out.push_back(c);
        else if (c == L' ' || c == L'-' || c == L'_') out.push_back(L'_');
    }
    while (!out.empty() && out.front() == L'_') out.erase(out.begin());
    while (!out.empty() && out.back() == L'_') out.pop_back();
    if (out.empty()) out = L"server";
    return out;
}

std::vector<ServerInstance> DiscoverInstances(const std::wstring& localStateDir) {
    std::vector<ServerInstance> out;
    std::wstring root = localStateDir + L"\\instances";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring dir = root + L"\\" + fd.cFileName;
            std::wstring jar = dir + L"\\paper.jar";
            if (GetFileAttributesW(jar.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            ServerInstance inst;
            inst.id = fd.cFileName;
            std::wstring label = ReadFirstLineW(dir + L"\\name.txt");
            inst.label = label.empty() ? std::wstring(fd.cFileName) : label;
            inst.dir = dir;
            inst.jarPath = jar;
            inst.mainClass = ReadMainClassSlash(dir);
            out.push_back(inst);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

bool CreateInstance(const std::wstring& localStateDir, const std::wstring& displayName,
                    const ServerVersion& source, std::wstring& outId) {
    std::wstring instancesRoot = localStateDir + L"\\instances";
    EnsureDirRecursive(instancesRoot);

    std::wstring base = SanitizeInstanceId(displayName);
    std::wstring id = base;
    int n = 2;
    while (GetFileAttributesW((instancesRoot + L"\\" + id).c_str()) != INVALID_FILE_ATTRIBUTES)
        id = base + L"-" + std::to_wstring(n++);

    std::wstring dir = instancesRoot + L"\\" + id;
    if (!EnsureDirRecursive(dir)) return false;

    if (!CopyFileW(source.jarPath.c_str(), (dir + L"\\paper.jar").c_str(), FALSE)) {
        WriteLaunchLogF(L"CreateInstance: copy jar failed err=%u", GetLastError());
        RemoveDirRecursive(dir);
        return false;
    }

    std::wstring srcDir = source.jarPath.substr(0, source.jarPath.find_last_of(L'\\'));
    std::wstring srcMain = srcDir + L"\\mainclass.txt";
    if (GetFileAttributesW(srcMain.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(srcMain.c_str(), (dir + L"\\mainclass.txt").c_str(), FALSE);

    std::wstring label = displayName.empty() ? id : displayName;
    HANDLE f = CreateFileW((dir + L"\\name.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::string u = WideToUtf8(label);
        DWORD w;
        WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
        CloseHandle(f);
    }

    HANDLE e = CreateFileW((dir + L"\\eula.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (e != INVALID_HANDLE_VALUE) {
        std::string eula = "#Accepted via JavaServerUWP on instance creation\r\neula=true\r\n";
        DWORD w;
        WriteFile(e, eula.data(), (DWORD)eula.size(), &w, nullptr);
        CloseHandle(e);
    }

    outId = id;
    WriteLaunchLogF(L"CreateInstance: %ls from %ls", id.c_str(), source.label.c_str());
    return true;
}

bool DeleteInstance(const std::wstring& localStateDir, const std::wstring& id) {
    if (id.empty()) return false;
    std::wstring dir = localStateDir + L"\\instances\\" + id;
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    bool ok = RemoveDirRecursive(dir);
    WriteLaunchLogF(L"DeleteInstance: %ls ok=%d", id.c_str(), ok ? 1 : 0);
    return ok;
}

std::wstring BuildClasspath(const std::wstring& libsDir) {
    std::vector<std::wstring> jars;
    CollectJars(libsDir, jars);
    std::wstring cp;
    for (size_t i = 0; i < jars.size(); ++i) {
        if (i > 0) cp += L";";
        cp += jars[i];
    }
    return cp;
}

std::wstring GetModuleDir() {
    WCHAR buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0) return L".";
    std::wstring path(buf, len);
    size_t slash = path.find_last_of(L"\\/");
    return path.substr(0, slash);
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string result(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &result[0], needed, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring result(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &result[0], needed);
    return result;
}

std::wstring GetEnvVarString(const wchar_t* name) {
    DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
    if (len == 0) return std::wstring();
    std::wstring value(len, L'\0');
    if (GetEnvironmentVariableW(name, value.data(), len) == 0) return std::wstring();
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}
HMODULE LoadJvmLibrary(const std::wstring& exeDir) {
    std::wstring jreDir = exeDir + L"\\jre";
    std::wstring jreBin = jreDir + L"\\bin";
    std::wstring jreServer = jreBin + L"\\server";
    std::wstring newPath = jreBin + L";" + jreServer + L";" + exeDir + L";" + GetEnvVarString(L"PATH");
    SetEnvironmentVariableW(L"PATH", newPath.c_str());
    SetEnvironmentVariableW(L"JAVA_HOME", jreDir.c_str());

    auto loadPackaged = [](const wchar_t* relPath, const wchar_t* label) -> HMODULE {
        HMODULE m = LoadPackagedLibrary(relPath, 0);
        if (!m) {
            WriteLaunchLogF(L"LoadPackagedLibrary(%s) failed err=%u", label, GetLastError());
        } else {
            WriteLaunchLogF(L"LoadPackagedLibrary(%s) OK", label);
        }
        return m;
    };
    // uwp loader does not resolve jre transitive deps for jvm.dll
    loadPackaged(L"jre\\bin\\vcruntime140.dll", L"vcruntime140.dll");
    loadPackaged(L"jre\\bin\\vcruntime140_1.dll", L"vcruntime140_1.dll");
    loadPackaged(L"jre\\bin\\msvcp140.dll", L"msvcp140.dll");
    loadPackaged(L"jre\\bin\\jli.dll", L"jli.dll");
    loadPackaged(L"jre\\bin\\java.dll", L"java.dll");

    HMODULE jvm = loadPackaged(L"jre\\bin\\server\\jvm.dll", L"jvm.dll");
    loadPackaged(L"jre\\bin\\awt.dll", L"awt.dll");
    return jvm;
}

bool SetupStdinPipe() {
    HANDLE readPipe = INVALID_HANDLE_VALUE;
    HANDLE writePipe = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return false;
    }
    SetHandleInformation(writePipe, HANDLE_FLAG_INHERIT, 0);

    int fdRead = _open_osfhandle((intptr_t)readPipe, _O_RDONLY | _O_BINARY);
    if (fdRead == -1) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }

    if (_dup2(fdRead, 0) != 0) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }
    _close(fdRead);
    // jvm reads the os stdin handle, not just the crt fd
    SetStdHandle(STD_INPUT_HANDLE, (HANDLE)_get_osfhandle(0));

    g_stdinWritePipe = writePipe;
    return true;
}

bool WriteToStdin(const std::string& text) {
    if (g_stdinWritePipe == INVALID_HANDLE_VALUE) return false;
    static std::mutex stdinMutex;
    std::lock_guard<std::mutex> lk(stdinMutex);
    DWORD written;
    return WriteFile(g_stdinWritePipe, text.c_str(), (DWORD)text.size(), &written, nullptr) != 0;
}

void CloseStdinPipe() {
    if (g_stdinWritePipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_stdinWritePipe);
        g_stdinWritePipe = INVALID_HANDLE_VALUE;
    }
}

static void AppendUtf8File(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        WriteLaunchLogF(L"Lifecycle: CreateFile(%s) failed err=%u", path.c_str(), GetLastError());
        return;
    }

    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void PersistLifecycleState(const wchar_t* state) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    wchar_t line[512];
    swprintf_s(line,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u state=%s serverRunning=%d suspending=%d background=%d shutdownRequested=%d\r\n",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        state ? state : L"(null)",
        g_serverRunning.load() ? 1 : 0,
        g_appSuspending.load() ? 1 : 0,
        g_appInBackground.load() ? 1 : 0,
        g_shutdownRequested.load() ? 1 : 0);

    AppendUtf8File(ResolveLocalStateDir() + L"\\lifecycle_state.log", WideToUtf8(line));
}

static void SaveServerForLifecycleTransition(const wchar_t* reason) {
    if (!g_serverRunning.load()) {
        WriteLaunchLogF(L"Lifecycle: %s, server not running", reason);
        return;
    }

    if (WriteToStdin("save-all flush\n")) {
        WriteLaunchLogF(L"Lifecycle: %s, issued save-all flush", reason);
    } else {
        WriteLaunchLogF(L"Lifecycle: %s, failed to write save-all flush", reason);
    }
}

void HandleSuspending(ABI::Windows::ApplicationModel::ISuspendingEventArgs* args) {
    WriteLaunchLog(L"!! UWP Suspending event fired !!");
    g_appSuspending = true;
    PersistLifecycleState(L"suspending");

    ComPtr<ABI::Windows::ApplicationModel::ISuspendingDeferral> deferral;
    if (args) {
        ComPtr<ABI::Windows::ApplicationModel::ISuspendingOperation> operation;
        HRESULT hr = args->get_SuspendingOperation(&operation);
        if (SUCCEEDED(hr) && operation) {
            hr = operation->GetDeferral(&deferral);
            if (FAILED(hr)) {
                WriteLaunchLogF(L"Lifecycle: GetDeferral failed hr=0x%08X", hr);
            }
        } else {
            WriteLaunchLogF(L"Lifecycle: get_SuspendingOperation failed hr=0x%08X", hr);
        }
    }

    SaveServerForLifecycleTransition(L"suspending");
    PersistLifecycleState(L"suspend-save-requested");

    if (deferral) {
        deferral->Complete();
        WriteLaunchLog(L"Lifecycle: suspending deferral completed");
    }
}

void HandleResuming() {
    WriteLaunchLog(L"!! UWP Resuming event fired !!");
    g_appSuspending = false;
    PersistLifecycleState(L"resuming");
    RequestForegroundExtendedExecution();
}

void HandleEnteredBackground(ABI::Windows::ApplicationModel::IEnteredBackgroundEventArgs* args) {
    WriteLaunchLog(L"!! UWP EnteredBackground event fired !!");
    g_appInBackground = true;
    PersistLifecycleState(L"entered-background");

    ComPtr<ABI::Windows::Foundation::IDeferral> deferral;
    if (args && SUCCEEDED(args->GetDeferral(&deferral))) {
        SaveServerForLifecycleTransition(L"entered-background");
        PersistLifecycleState(L"background-save-requested");
        deferral->Complete();
        WriteLaunchLog(L"Lifecycle: entered-background deferral completed");
    }
}

void HandleLeavingBackground(ABI::Windows::ApplicationModel::ILeavingBackgroundEventArgs* args) {
    WriteLaunchLog(L"!! UWP LeavingBackground event fired !!");
    g_appInBackground = false;
    PersistLifecycleState(L"leaving-background");

    ComPtr<ABI::Windows::Foundation::IDeferral> deferral;
    if (args && SUCCEEDED(args->GetDeferral(&deferral))) {
        RequestForegroundExtendedExecution();
        deferral->Complete();
        WriteLaunchLog(L"Lifecycle: leaving-background deferral completed");
    }
}
bool RedirectStdioToFiles(const std::wstring& stdoutPath, const std::wstring& stderrPath) {
    int fdOut = -1;
    if (_wsopen_s(&fdOut, stdoutPath.c_str(),
            _O_CREAT | _O_APPEND | _O_WRONLY | _O_TEXT,
            _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0 || fdOut < 0) {
        WriteLaunchLogF(L"RedirectStdio: _wsopen_s(stdout) failed errno=%d", errno);
        return false;
    }

    int fdErr = -1;
    if (_wsopen_s(&fdErr, stderrPath.c_str(),
            _O_CREAT | _O_APPEND | _O_WRONLY | _O_TEXT,
            _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0 || fdErr < 0) {
        WriteLaunchLogF(L"RedirectStdio: _wsopen_s(stderr) failed errno=%d", errno);
        _close(fdOut);
        return false;
    }

    if (_dup2(fdOut, 1) != 0) {
        WriteLaunchLogF(L"RedirectStdio: _dup2(stdout) failed errno=%d", errno);
        _close(fdOut); _close(fdErr);
        return false;
    }
    if (_dup2(fdErr, 2) != 0) {
        WriteLaunchLogF(L"RedirectStdio: _dup2(stderr) failed errno=%d", errno);
        _close(fdOut); _close(fdErr);
        return false;
    }
    _close(fdOut);
    _close(fdErr);
    // uwp crt file structs do not follow dup2 by themselves
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(1));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(2));
    FILE* out = _fdopen(1, "a");
    FILE* err = _fdopen(2, "a");
    if (!out || !err) {
        WriteLaunchLog(L"RedirectStdio: _fdopen failed");
        return false;
    }
    *stdout = *out;
    *stderr = *err;
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    WriteLaunchLog(L"RedirectStdio: stdout/stderr wired to log files");
    return true;
}

static bool WinHttpGet(const std::wstring& url, std::string* outBody,
                       const std::wstring& outFile, std::atomic<int>* progressPct) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}; wchar_t path[4096] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        WriteLaunchLogF(L"WinHttpGet: crack url failed err=%u", GetLastError());
        return false;
    }
    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET ses = WinHttpOpen(L"BanditVault-JavaServerUWP/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { WriteLaunchLogF(L"WinHttpGet: open failed err=%u", GetLastError()); return false; }

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = nullptr;
    bool ok = false;
    if (con) {
        req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    }
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {

        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        WriteLaunchLogF(L"WinHttpGet: status=%u url=%ls", status, url.c_str());

        if (status >= 200 && status < 300) {
            DWORD total = 0, tlen = sizeof(total);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &total, &tlen, WINHTTP_NO_HEADER_INDEX);

            HANDLE fh = INVALID_HANDLE_VALUE;
            if (!outFile.empty()) {
                fh = CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (fh == INVALID_HANDLE_VALUE)
                    WriteLaunchLogF(L"WinHttpGet: create out file failed err=%u", GetLastError());
            }
            ok = true;
            ULONGLONG received = 0;
            std::vector<char> buf(65536);
            DWORD avail = 0;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                if (avail == 0) break;
                DWORD want = avail < buf.size() ? avail : (DWORD)buf.size();
                DWORD read = 0;
                if (!WinHttpReadData(req, buf.data(), want, &read) || read == 0) break;
                received += read;
                if (outBody) outBody->append(buf.data(), read);
                if (fh != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(fh, buf.data(), read, &w, nullptr); }
                if (progressPct && total > 0) progressPct->store((int)((received * 100ULL) / total));
            } while (avail > 0);
            if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
            if (progressPct) progressPct->store(100);
        }
    } else {
        WriteLaunchLogF(L"WinHttpGet: send/recv failed err=%u url=%ls", GetLastError(), url.c_str());
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

bool HttpGetString(const std::wstring& url, std::string& out) {
    out.clear();
    return WinHttpGet(url, &out, L"", nullptr);
}

bool HttpDownloadToFile(const std::wstring& url, const std::wstring& outPath, std::atomic<int>* progressPct) {
    return WinHttpGet(url, nullptr, outPath, progressPct);
}

static std::string JsonStringAfter(const std::string& s, const char* key, size_t from) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k, from);
    if (p == std::string::npos) return "";
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < s.size() && (unsigned char)s[p] <= ' ') p++;
    if (p >= s.size() || s[p] != '"') return "";
    p++;
    std::string out;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) { p++; out += s[p]; }
        else out += s[p];
        p++;
    }
    return out;
}

static int JsonIntAfter(const std::string& s, const char* key, size_t from) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k, from);
    if (p == std::string::npos) return -1;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return -1;
    p++;
    while (p < s.size() && (unsigned char)s[p] <= ' ') p++;
    int sign = 1;
    if (p < s.size() && s[p] == '-') { sign = -1; p++; }
    long v = 0; bool any = false;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') { v = v * 10 + (s[p] - '0'); p++; any = true; }
    return any ? (int)(sign * v) : -1;
}
static std::string JsonValueSpan(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return "";
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < s.size() && (unsigned char)s[p] <= ' ') p++;
    if (p >= s.size()) return "";
    char open = s[p], close;
    if (open == '{') close = '}';
    else if (open == '[') close = ']';
    else return "";
    int depth = 0; bool inStr = false; size_t start = p;
    for (; p < s.size(); p++) {
        char c = s[p];
        if (inStr) { if (c == '\\') p++; else if (c == '"') inStr = false; }
        else if (c == '"') inStr = true;
        else if (c == open) depth++;
        else if (c == close) { if (--depth == 0) return s.substr(start, p - start + 1); }
    }
    return "";
}

std::vector<std::wstring> FetchPaperVersions() {
    std::string json;
    if (!HttpGetString(L"https://fill.papermc.io/v3/projects/paper", json)) {
        WriteLaunchLog(L"FetchPaperVersions: http failed");
        return {};
    }
    std::string span = JsonValueSpan(json, "versions");
    if (span.empty()) span = json;
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < span.size()) {
        if (span[i] == '"') {
            size_t j = i + 1; std::string tok;
            while (j < span.size() && span[j] != '"') { if (span[j] == '\\' && j + 1 < span.size()) j++; tok += span[j]; j++; }
            i = j + 1;
            if (!tok.empty() && tok[0] >= '0' && tok[0] <= '9' && tok.find('.') != std::string::npos) {
                std::wstring w = Utf8ToWide(tok);
                if (std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
            }
        } else i++;
    }
    WriteLaunchLogF(L"FetchPaperVersions: %zu versions", out.size());
    return out;
}

// paper build order is not the contract
static bool ResolveStableBuildUrl(const std::string& buildsJson, std::wstring& url, int& buildId) {
    size_t arr = buildsJson.find('[');
    if (arr == std::string::npos) return false;
    int depth = 0; bool inStr = false; size_t elemStart = std::string::npos;
    std::vector<std::string> elems;
    for (size_t p = arr; p < buildsJson.size(); p++) {
        char c = buildsJson[p];
        if (inStr) { if (c == '\\') p++; else if (c == '"') inStr = false; continue; }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') { if (depth == 0) elemStart = p; depth++; }
        else if (c == '}') { if (--depth == 0 && elemStart != std::string::npos) { elems.push_back(buildsJson.substr(elemStart, p - elemStart + 1)); elemStart = std::string::npos; } }
        else if (c == ']' && depth == 0) break;
    }
    const std::string* chosen = nullptr;
    for (auto& e : elems) { if (JsonStringAfter(e, "channel", 0) == "STABLE") { chosen = &e; break; } }
    if (!chosen && !elems.empty()) chosen = &elems.front();
    if (!chosen) return false;
    size_t sd = chosen->find("\"server:default\"");
    if (sd == std::string::npos) return false;
    std::string u = JsonStringAfter(*chosen, "url", sd);
    if (u.empty()) return false;
    url = Utf8ToWide(u);
    buildId = JsonIntAfter(*chosen, "id", 0);
    return true;
}

static bool WriteSmallFileUtf8(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; WriteFile(h, content.data(), (DWORD)content.size(), &w, nullptr);
    CloseHandle(h);
    return true;
}

bool DownloadPaperVersion(const std::wstring& localStateDir, const std::wstring& ver,
                          std::atomic<int>* progressPct, std::wstring& outMessage) {
    if (progressPct) progressPct->store(0);
    std::string buildsJson;
    std::wstring buildsUrl = L"https://fill.papermc.io/v3/projects/paper/versions/" + ver + L"/builds";
    if (!HttpGetString(buildsUrl, buildsJson)) { outMessage = L"Could not reach Paper API"; return false; }

    std::wstring url; int buildId = -1;
    if (!ResolveStableBuildUrl(buildsJson, url, buildId)) { outMessage = L"No build found for " + ver; return false; }
    WriteLaunchLogF(L"DownloadPaperVersion: %ls build %d url=%ls", ver.c_str(), buildId, url.c_str());

    std::wstring dir = localStateDir + L"\\server-jars\\" + ver;
    EnsureDirRecursive(dir);
    std::wstring jar = dir + L"\\paper.jar";
    if (!HttpDownloadToFile(url, jar, progressPct)) { outMessage = L"Download failed"; return false; }

    std::string label = "Paper " + WideToUtf8(ver);
    if (buildId >= 0) label += " build " + std::to_string(buildId);
    WriteSmallFileUtf8(dir + L"\\version.txt", label);
    WriteSmallFileUtf8(dir + L"\\mainclass.txt", "io.papermc.paperclip.Main");

    outMessage = L"Downloaded " + Utf8ToWide(label);
    return true;
}
