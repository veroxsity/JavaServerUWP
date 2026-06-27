#pragma once

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "WindowsApp.lib")
#pragma comment(lib, "windowscodecs.lib")

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.applicationmodel.core.h>
#include <windows.applicationmodel.h>
#include <windows.applicationmodel.extendedexecution.foreground.h>
#include <windows.ui.core.h>
#include <windows.foundation.h>
#include <windows.storage.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <fstream>
#include <sstream>
#include <algorithm>

#include <jni.h>

#include "runtime_config.h"
MIDL_INTERFACE("45D64A29-A63B-4948-AE11-979AC0A4C806")
ICoreWindowInterop : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND* hwnd) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MessageHandled(unsigned char value) = 0;
};

using Microsoft::WRL::ComPtr;

using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::ApplicationModel::Core;
using namespace ABI::Windows::ApplicationModel::ExtendedExecution::Foreground;
using namespace ABI::Windows::UI::Core;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Storage;

extern std::atomic<bool> g_shutdownRequested;
extern std::atomic<bool> g_serverRunning;
extern std::atomic<bool> g_appSuspending;
extern std::atomic<bool> g_appInBackground;
extern std::atomic<bool> g_extendedExecutionRequested;
extern HANDLE g_stdinWritePipe;
extern std::wstring g_localStateDir;
extern std::mutex g_extendedExecutionMutex;
extern ComPtr<IExtendedExecutionForegroundSession> g_extendedExecutionSession;

struct ServerVersion {
    std::wstring id;
    std::wstring label;
    std::wstring jarPath;
    std::string mainClass;
    bool bundled;
};

struct ServerInstance {
    std::wstring id;
    std::wstring label;
    std::wstring dir;
    std::wstring jarPath;
    std::string mainClass;
};

struct ServerThreadContext {
    std::wstring localStateDir;
    std::wstring workDir;
    std::wstring exeDir;
    std::wstring stdoutLogPath;
    std::wstring stderrLogPath;
    std::wstring serverJar;
    std::string serverMainClass;
    HANDLE shutdownEvent;
    bool* pSuccess;
};

DWORD WINAPI ServerThreadEntry(LPVOID param);

std::vector<ServerVersion> DiscoverServerVersions(const std::wstring& localStateDir, const std::wstring& exeDir);
bool DeleteServerVersion(const std::wstring& localStateDir, const std::wstring& id);

std::vector<ServerInstance> DiscoverInstances(const std::wstring& localStateDir);
bool CreateInstance(const std::wstring& localStateDir, const std::wstring& displayName,
                    const ServerVersion& source, std::wstring& outId);
bool DeleteInstance(const std::wstring& localStateDir, const std::wstring& id);

namespace textentry {
    void Show(const std::wstring& initial, void* coreWindowAbi);
    void Hide();
    bool Active();
    std::wstring GetText();
    bool ConsumeSubmit();
}

namespace appcontrol {
    void RequestRestart();
}

namespace webpanel {
    void Start(const std::wstring& rootDir);
    void Stop();
    bool Running();
    std::wstring Url();
    void SetStatusProvider(std::function<bool()> isRunning);
}

bool HttpGetString(const std::wstring& url, std::string& out);
bool HttpDownloadToFile(const std::wstring& url, const std::wstring& outPath, std::atomic<int>* progressPct);
std::vector<std::wstring> FetchPaperVersions();
bool DownloadPaperVersion(const std::wstring& localStateDir, const std::wstring& ver,
                          std::atomic<int>* progressPct, std::wstring& outMessage);

std::wstring ResolveLocalStateDir();
void EnsureDirectoryTree(const std::wstring& path);
const std::wstring& ResolveLaunchLogPath();
void WriteLaunchLog(const wchar_t* msg);
void WriteLaunchLogF(const wchar_t* fmt, ...);
void InstallProcessDiagnostics();
void RequestForegroundExtendedExecution();
bool EnsureDirRecursive(const std::wstring& path);
void SeedFabricCache(const std::wstring& exeDir, const std::wstring& localStateDir);
void CollectJars(const std::wstring& root, std::vector<std::wstring>& jars);
std::wstring BuildClasspath(const std::wstring& libsDir);
std::wstring GetModuleDir();
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
std::wstring GetEnvVarString(const wchar_t* name);
HMODULE LoadJvmLibrary(const std::wstring& exeDir);
bool SetupStdinPipe();
bool WriteToStdin(const std::string& text);
void CloseStdinPipe();
void PersistLifecycleState(const wchar_t* state);
bool RedirectStdioToFiles(const std::wstring& stdoutPath, const std::wstring& stderrPath);
void HandleSuspending(ABI::Windows::ApplicationModel::ISuspendingEventArgs* args);
void HandleResuming();
void HandleEnteredBackground(ABI::Windows::ApplicationModel::IEnteredBackgroundEventArgs* args);
void HandleLeavingBackground(ABI::Windows::ApplicationModel::ILeavingBackgroundEventArgs* args);
