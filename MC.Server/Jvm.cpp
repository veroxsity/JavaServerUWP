#include "Server.h"

std::vector<std::string> BuildJvmOptions(const ServerThreadContext& ctx) {
    std::vector<std::string> opts;

    opts.push_back("-Xmx" + std::string(RuntimeConfig::JVM_XMX));
    opts.push_back("-Xms" + std::string(RuntimeConfig::JVM_XMS));

    opts.push_back("--enable-native-access=ALL-UNNAMED");
    opts.push_back("-Djava.security.manager=allow");

    std::string javaHome = WideToUtf8(ctx.exeDir + L"\\jre");
    opts.push_back("-Djava.home=" + javaHome);
    // full override avoids uwp registry probes in the bundled jre security file
    std::string secProps = WideToUtf8(ctx.exeDir + L"\\jre\\conf\\security\\xbox.properties");
    opts.push_back("-Djava.security.properties==" + secProps);

    opts.push_back("-Djava.security.egd=file:/dev/./urandom");

    std::string tmpDir = WideToUtf8(ctx.localStateDir + L"\\tmp");
    opts.push_back("-Djava.io.tmpdir=" + tmpDir);
    opts.push_back("-XX:ErrorFile=" + WideToUtf8(ctx.localStateDir + L"\\hs_err_pid%p.log"));
    std::wstring libsDir = ctx.exeDir + L"\\libraries";
    std::vector<std::wstring> jars;
    CollectJars(libsDir, jars);
    std::wstring compatJar = ctx.exeDir + L"\\bundled-mods\\banditvault-xbox-compat-1.0.0.jar";
    if (GetFileAttributesW(compatJar.c_str()) != INVALID_FILE_ATTRIBUTES) {
        jars.push_back(compatJar);
    } else {
        WriteLaunchLogF(L"BuildJvmOptions: compat jar not found at %ls", compatJar.c_str());
    }
    std::wstring serverJarPath = ctx.exeDir + L"\\server\\server.jar";
    jars.push_back(serverJarPath);
    std::wstring cp;
    for (size_t i = 0; i < jars.size(); ++i) {
        if (i > 0) cp += L";";
        cp += jars[i];
    }
    opts.push_back("-Djava.class.path=" + WideToUtf8(cp));

    std::string serverJar = WideToUtf8(serverJarPath);
    opts.push_back("-Dfabric.gameJarPath=" + serverJar);

    std::string userDir = WideToUtf8(ctx.localStateDir);
    opts.push_back("-Duser.dir=" + userDir);
    // leading dot cache paths fail mid-write in localstate
    std::string fabricCache = WideToUtf8(ctx.localStateDir + L"\\fabric-cache");
    opts.push_back("-Dfabric.cacheDir=" + fabricCache);
    opts.push_back("-Dfabric.log.level=DEBUG");
    opts.push_back("-Dfabric.debug.deobfuscateWithSourceFileNames=true");

    std::string logConfig = WideToUtf8(ctx.exeDir + L"\\log_configs\\server-uwp.xml");
    opts.push_back("-Dlog4j.configurationFile=" + logConfig);
    opts.push_back("-Dbanditvault.launchLog=" + WideToUtf8(ResolveLaunchLogPath()));

    return opts;
}
static void LogJavaException(JNIEnv* env, const wchar_t* stage) {
    jthrowable ex = env->ExceptionOccurred();
    if (!ex) return;
    env->ExceptionClear();

    auto callGetter = [&](jclass cls, jobject obj, const char* method) -> std::string {
        jmethodID mid = env->GetMethodID(cls, method, "()Ljava/lang/String;");
        if (!mid) { env->ExceptionClear(); return {}; }
        jstring js = (jstring)env->CallObjectMethod(obj, mid);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
        if (!js) return {};
        const char* c = env->GetStringUTFChars(js, nullptr);
        std::string s = c ? c : "";
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
        return s;
    };

    jclass exClass = env->GetObjectClass(ex);
    jclass classClass = env->FindClass("java/lang/Class");
    std::string typeName = exClass && classClass ? callGetter(classClass, exClass, "getName") : "<unknown>";
    std::string message = callGetter(exClass, ex, "getMessage");

    WriteLaunchLogF(L"[java] %s threw %S: %S", stage, typeName.c_str(), message.c_str());

    jmethodID getCauseMid = env->GetMethodID(exClass, "getCause", "()Ljava/lang/Throwable;");
    jthrowable cause = getCauseMid ? (jthrowable)env->CallObjectMethod(ex, getCauseMid) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    int depth = 0;
    while (cause && depth < 6) {
        jclass cClass = env->GetObjectClass(cause);
        std::string cType = cClass && classClass ? callGetter(classClass, cClass, "getName") : "<?>";
        std::string cMsg = callGetter(cClass, cause, "getMessage");
        WriteLaunchLogF(L"  caused by %S: %S", cType.c_str(), cMsg.c_str());
        jmethodID gcm = env->GetMethodID(cClass, "getCause", "()Ljava/lang/Throwable;");
        jthrowable next = gcm ? (jthrowable)env->CallObjectMethod(cause, gcm) : nullptr;
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(cause);
        env->DeleteLocalRef(cClass);
        cause = next;
        depth++;
    }

    jclass stringWriterClass = env->FindClass("java/io/StringWriter");
    jclass printWriterClass = env->FindClass("java/io/PrintWriter");
    jclass throwableClass = env->FindClass("java/lang/Throwable");
    if (stringWriterClass && printWriterClass && throwableClass && !env->ExceptionCheck()) {
        jmethodID swCtor = env->GetMethodID(stringWriterClass, "<init>", "()V");
        jmethodID pwCtor = env->GetMethodID(printWriterClass, "<init>", "(Ljava/io/Writer;)V");
        jmethodID printStackTrace = env->GetMethodID(throwableClass, "printStackTrace", "(Ljava/io/PrintWriter;)V");
        jmethodID toStringMid = env->GetMethodID(stringWriterClass, "toString", "()Ljava/lang/String;");
        if (swCtor && pwCtor && printStackTrace && toStringMid && !env->ExceptionCheck()) {
            jobject sw = env->NewObject(stringWriterClass, swCtor);
            jobject pw = sw ? env->NewObject(printWriterClass, pwCtor, sw) : nullptr;
            if (sw && pw && !env->ExceptionCheck()) {
                env->CallVoidMethod(ex, printStackTrace, pw);
                if (!env->ExceptionCheck()) {
                    jstring traceString = (jstring)env->CallObjectMethod(sw, toStringMid);
                    if (traceString && !env->ExceptionCheck()) {
                        const char* traceChars = env->GetStringUTFChars(traceString, nullptr);
                        if (traceChars) {
                            std::istringstream lines(traceChars);
                            std::string line;
                            int lineCount = 0;
                            while (std::getline(lines, line) && lineCount < 80) {
                                if (!line.empty() && line.back() == '\r') {
                                    line.pop_back();
                                }
                                if (line.size() > 500) {
                                    line.resize(500);
                                }
                                WriteLaunchLogF(L"  stack: %S", line.c_str());
                                lineCount++;
                            }
                            env->ReleaseStringUTFChars(traceString, traceChars);
                        }
                        env->DeleteLocalRef(traceString);
                    }
                }
            }
            if (pw) env->DeleteLocalRef(pw);
            if (sw) env->DeleteLocalRef(sw);
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (throwableClass) env->DeleteLocalRef(throwableClass);
    if (printWriterClass) env->DeleteLocalRef(printWriterClass);
    if (stringWriterClass) env->DeleteLocalRef(stringWriterClass);

    if (classClass) env->DeleteLocalRef(classClass);
    if (exClass) env->DeleteLocalRef(exClass);
    env->DeleteLocalRef(ex);
}

#ifdef SERVER_TYPE_PAPER

static bool WritePaperEula(const std::wstring& dir) {
    std::wstring path = dir + L"\\eula.txt";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        WriteLaunchLogF(L"WritePaperEula: create failed err=%u", GetLastError());
        return false;
    }
    const char* body = "eula=true\n";
    DWORD written = 0;
    WriteFile(h, body, (DWORD)strlen(body), &written, nullptr);
    CloseHandle(h);
    WriteLaunchLogF(L"WritePaperEula: wrote %ls", path.c_str());
    return true;
}

static std::wstring FindPaperJar(const std::wstring& exeDir) {
    std::wstring preferred = exeDir + L"\\server\\paper.jar";
    if (GetFileAttributesW(preferred.c_str()) != INVALID_FILE_ATTRIBUTES) return preferred;
    std::vector<std::wstring> jars;
    CollectJars(exeDir + L"\\server", jars);
    return jars.empty() ? std::wstring() : jars[0];
}

static std::string ReadServerMainClass(const std::wstring& exeDir) {
    std::wstring path = exeDir + L"\\server\\mainclass.txt";
    std::ifstream f(path.c_str());
    std::string name;
    if (f) std::getline(f, name);
    while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' ' || name.back() == '\t')) name.pop_back();
    size_t start = name.find_first_not_of(" \t");
    if (start != std::string::npos) name = name.substr(start);
    if (name.empty()) name = "io.papermc.paperclip.Main";
    for (auto& c : name) if (c == '.') c = '/';
    return name;
}

static std::vector<std::string> BuildPaperJvmOptions(const ServerThreadContext& ctx, const std::wstring& paperJar) {
    std::vector<std::string> opts;
    std::wstring workDir = ctx.workDir.empty() ? ctx.localStateDir : ctx.workDir;
    // leaves headroom on series s dev mode memory
    opts.push_back("-Xmx2G");
    opts.push_back("-Xms1G");
    opts.push_back("--enable-native-access=ALL-UNNAMED");
    opts.push_back("-Djava.security.manager=allow");
    opts.push_back("-Djava.home=" + WideToUtf8(ctx.exeDir + L"\\jre"));
    // full override avoids uwp registry probes in the bundled jre security file
    opts.push_back("-Djava.security.properties==" + WideToUtf8(ctx.exeDir + L"\\jre\\conf\\security\\xbox.properties"));
    opts.push_back("-Djava.security.egd=file:/dev/./urandom");
    opts.push_back("-Djava.io.tmpdir=" + WideToUtf8(workDir + L"\\tmp"));
    opts.push_back("-Djava.awt.headless=true");
    // no native terminal exists in the container
    opts.push_back("-Dterminal.jline=false");
    // uwp blocks dll loads from writable temp dirs
    opts.push_back("-Djna.boot.library.path=" + WideToUtf8(ctx.exeDir + L"\\jna"));
    opts.push_back("-Djna.nounpack=true");
    opts.push_back("-Dlog4j.configurationFile=" + WideToUtf8(ctx.exeDir + L"\\log_configs\\server-uwp.xml"));
    // paperclip hits torealpath paths that uwp denies
    opts.push_back("--patch-module=jdk.zipfs=" + WideToUtf8(ctx.exeDir + L"\\jdk-patch\\jdk.zipfs"));
    opts.push_back("--patch-module=java.base=" + WideToUtf8(ctx.exeDir + L"\\jdk-patch\\java.base"));
    opts.push_back("-XX:ErrorFile=" + WideToUtf8(workDir + L"\\hs_err_pid%p.log"));
    // system.exit would kill the native host
    opts.push_back("-Xbootclasspath/a:" + WideToUtf8(ctx.exeDir + L"\\exittrap"));
    opts.push_back("-Djava.class.path=" + WideToUtf8(paperJar));
    opts.push_back("-Duser.dir=" + WideToUtf8(workDir));
    return opts;
}

static bool InstallExitTrap(JNIEnv* env) {
    jclass cls = env->FindClass("banditvault/xboxcompat/ExitTrap");
    if (!cls || env->ExceptionCheck()) {
        env->ExceptionClear();
        WriteLaunchLog(L"ExitTrap: class not on boot classpath, /stop will kill the host");
        return false;
    }
    jmethodID install = env->GetStaticMethodID(cls, "install", "()V");
    if (!install) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        WriteLaunchLog(L"ExitTrap: install() missing");
        return false;
    }
    env->CallStaticVoidMethod(cls, install);
    bool ok = !env->ExceptionCheck();
    if (!ok) { LogJavaException(env, L"ExitTrap.install"); env->ExceptionClear(); }
    else WriteLaunchLog(L"ExitTrap: installed");
    env->DeleteLocalRef(cls);
    return ok;
}
static bool ConsumeExitTrap(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    jclass trapCls = env->FindClass("banditvault/xboxcompat/ExitTrap$ExitTrappedException");
    bool trapped = trapCls && env->IsInstanceOf(ex, trapCls);
    if (trapped) {
        WriteLaunchLog(L"paper main: System.exit trapped, server stopped cleanly");
    } else {
        env->Throw(ex);
        LogJavaException(env, L"paper main");
        env->ExceptionClear();
    }
    if (trapCls) env->DeleteLocalRef(trapCls);
    env->DeleteLocalRef(ex);
    return trapped;
}
static bool ExitTrapWasTriggered(JNIEnv* env) {
    jclass cls = env->FindClass("banditvault/xboxcompat/ExitTrap");
    if (!cls) { env->ExceptionClear(); return false; }
    jmethodID m = env->GetStaticMethodID(cls, "wasTriggered", "()Z");
    bool r = m && env->CallStaticBooleanMethod(cls, m) == JNI_TRUE;
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return r;
}

static int RunEmbeddedPaperServer(const ServerThreadContext& ctx) {
    WriteLaunchLog(L"RunEmbeddedPaperServer: begin");

    EnsureDirRecursive(ctx.localStateDir + L"\\tmp");
    WritePaperEula(ctx.localStateDir);

    std::wstring paperJar = ctx.serverJar.empty() ? FindPaperJar(ctx.exeDir) : ctx.serverJar;
    if (paperJar.empty()) {
        WriteLaunchLog(L"RunEmbeddedPaperServer: no server jar selected and none under \\server");
        return -10;
    }
    WriteLaunchLogF(L"RunEmbeddedPaperServer: jar=%ls", paperJar.c_str());

    HMODULE jvm = LoadJvmLibrary(ctx.exeDir);
    if (!jvm) { WriteLaunchLog(L"RunEmbeddedPaperServer: jvm.dll load FAILED"); return -1; }

    typedef jint (JNICALL *CreateJavaVMFunc)(JavaVM**, void**, void*);
    auto CreateJavaVM = (CreateJavaVMFunc)GetProcAddress(jvm, "JNI_CreateJavaVM");
    if (!CreateJavaVM) { WriteLaunchLog(L"RunEmbeddedPaperServer: no JNI_CreateJavaVM"); return -2; }

    auto optStrs = BuildPaperJvmOptions(ctx, paperJar);
    WriteLaunchLogF(L"RunEmbeddedPaperServer: %zu JVM options", optStrs.size());
    for (size_t i = 0; i < optStrs.size(); ++i) WriteLaunchLogF(L"  opt[%zu] = %S", i, optStrs[i].c_str());

    JavaVMOption* options = new JavaVMOption[optStrs.size()];
    for (size_t i = 0; i < optStrs.size(); ++i) {
        options[i].optionString = const_cast<char*>(optStrs[i].c_str());
        options[i].extraInfo = nullptr;
    }

    JavaVMInitArgs vmArgs;
    vmArgs.version = JNI_VERSION_21;
    vmArgs.nOptions = (jint)optStrs.size();
    vmArgs.options = options;
    vmArgs.ignoreUnrecognized = JNI_TRUE;
    std::wstring paperCwd = ctx.workDir.empty() ? ctx.localStateDir : ctx.workDir;
    if (SetCurrentDirectoryW(paperCwd.c_str())) {
        WriteLaunchLogF(L"SetCurrentDirectory: cwd -> %ls", paperCwd.c_str());
    } else {
        WriteLaunchLogF(L"SetCurrentDirectory failed err=%u", GetLastError());
    }

    JavaVM* jvmPtr = nullptr;
    JNIEnv* env = nullptr;
    WriteLaunchLog(L"RunEmbeddedPaperServer: calling JNI_CreateJavaVM");
    jint result = CreateJavaVM(&jvmPtr, (void**)&env, &vmArgs);
    delete[] options;
    WriteLaunchLogF(L"RunEmbeddedPaperServer: JNI_CreateJavaVM returned %d", result);
    if (result != JNI_OK) return -3;

    InstallExitTrap(env);

    std::string mainClassName = ctx.serverMainClass.empty() ? ReadServerMainClass(ctx.exeDir) : ctx.serverMainClass;
    WriteLaunchLogF(L"RunEmbeddedPaperServer: main class = %S", mainClassName.c_str());

    jclass mainClass = env->FindClass(mainClassName.c_str());
    if (env->ExceptionCheck() || !mainClass) {
        LogJavaException(env, L"FindClass(paper main)");
        jvmPtr->DestroyJavaVM();
        return -4;
    }
    jmethodID mainMethod = env->GetStaticMethodID(mainClass, "main", "([Ljava/lang/String;)V");
    if (!mainMethod) {
        LogJavaException(env, L"GetStaticMethodID(paper main)");
        jvmPtr->DestroyJavaVM();
        return -6;
    }
    jobjectArray args = env->NewObjectArray(1, env->FindClass("java/lang/String"), env->NewStringUTF(""));
    env->SetObjectArrayElement(args, 0, env->NewStringUTF("nogui"));

    WriteLaunchLog(L"RunEmbeddedPaperServer: calling paper main");
    env->CallStaticVoidMethod(mainClass, mainMethod, args);

    if (env->ExceptionCheck()) {
        ConsumeExitTrap(env);
        WriteLaunchLog(L"RunEmbeddedPaperServer: main returned with exception, server stopped");
    } else {
        WriteLaunchLog(L"RunEmbeddedPaperServer: paper main returned, server running");
        while (!ExitTrapWasTriggered(env) && !g_shutdownRequested) {
            Sleep(200);
        }
        WriteLaunchLogF(L"RunEmbeddedPaperServer: server stopped (trap=%d shutdown=%d)",
            ExitTrapWasTriggered(env) ? 1 : 0, g_shutdownRequested ? 1 : 0);
    }
    return 0;
}
#endif

int RunEmbeddedServer(const ServerThreadContext& ctx) {
#ifdef SERVER_TYPE_PAPER
    return RunEmbeddedPaperServer(ctx);
#endif
    WriteLaunchLog(L"RunEmbeddedServer: begin");

    HMODULE jvm = LoadJvmLibrary(ctx.exeDir);
    if (!jvm) {
        WriteLaunchLog(L"RunEmbeddedServer: jvm.dll load FAILED");
        return -1;
    }
    WriteLaunchLog(L"RunEmbeddedServer: jvm.dll loaded");

    typedef jint (JNICALL *CreateJavaVMFunc)(JavaVM**, void**, void*);
    auto CreateJavaVM = (CreateJavaVMFunc)GetProcAddress(jvm, "JNI_CreateJavaVM");
    if (!CreateJavaVM) {
        WriteLaunchLogF(L"RunEmbeddedServer: GetProcAddress(JNI_CreateJavaVM) failed err=%u", GetLastError());
        return -2;
    }

    auto optStrs = BuildJvmOptions(ctx);
    WriteLaunchLogF(L"RunEmbeddedServer: built %zu JVM options", optStrs.size());
    for (size_t i = 0; i < optStrs.size(); ++i) {
        WriteLaunchLogF(L"  opt[%zu] = %S", i, optStrs[i].c_str());
    }

    JavaVMOption* options = new JavaVMOption[optStrs.size()];
    for (size_t i = 0; i < optStrs.size(); ++i) {
        options[i].optionString = const_cast<char*>(optStrs[i].c_str());
        options[i].extraInfo = nullptr;
    }

    JavaVMInitArgs vmArgs;
    vmArgs.version = JNI_VERSION_21;
    vmArgs.nOptions = (jint)optStrs.size();
    vmArgs.options = options;
    vmArgs.ignoreUnrecognized = JNI_TRUE;
    // fabric deletes a good cache jar if a stale tmp is present
    SeedFabricCache(ctx.exeDir, ctx.localStateDir);
    // some jdk native io ignores user.dir and uses the process cwd
    std::wstring fabricCwd = ctx.workDir.empty() ? ctx.localStateDir : ctx.workDir;
    if (SetCurrentDirectoryW(fabricCwd.c_str())) {
        WriteLaunchLogF(L"SetCurrentDirectory: cwd -> %ls", fabricCwd.c_str());
    } else {
        WriteLaunchLogF(L"SetCurrentDirectory failed err=%u", GetLastError());
    }

    JavaVM* jvmPtr = nullptr;
    JNIEnv* env = nullptr;
    WriteLaunchLog(L"RunEmbeddedServer: calling JNI_CreateJavaVM");
    jint result = CreateJavaVM(&jvmPtr, (void**)&env, &vmArgs);
    delete[] options;

    WriteLaunchLogF(L"RunEmbeddedServer: JNI_CreateJavaVM returned %d", result);
    if (result != JNI_OK) {
        return -3;
    }

    jclass exitTrapClass = env->FindClass("banditvault/xboxcompat/ExitTrap");
    if (env->ExceptionCheck()) {
        LogJavaException(env, L"FindClass(ExitTrap)");
    } else if (exitTrapClass) {
        jmethodID installExitTrap = env->GetStaticMethodID(exitTrapClass, "install", "()V");
        if (env->ExceptionCheck()) {
            LogJavaException(env, L"GetStaticMethodID(ExitTrap.install)");
        } else if (installExitTrap) {
            env->CallStaticVoidMethod(exitTrapClass, installExitTrap);
            if (env->ExceptionCheck()) {
                LogJavaException(env, L"ExitTrap.install");
            } else {
                WriteLaunchLog(L"RunEmbeddedServer: ExitTrap installed");
            }
        }
        env->DeleteLocalRef(exitTrapClass);
    }

    jclass knotServerClass = env->FindClass("net/fabricmc/loader/impl/launch/knot/KnotServer");
    if (env->ExceptionCheck()) {
        LogJavaException(env, L"FindClass(KnotServer)");
        jvmPtr->DestroyJavaVM();
        return -4;
    }

    if (!knotServerClass) {
        WriteLaunchLog(L"RunEmbeddedServer: KnotServer class not found");
        jvmPtr->DestroyJavaVM();
        return -5;
    }

    jmethodID mainMethod = env->GetStaticMethodID(knotServerClass, "main",
        "([Ljava/lang/String;)V");
    if (!mainMethod) {
        LogJavaException(env, L"GetStaticMethodID(KnotServer.main)");
        jvmPtr->DestroyJavaVM();
        return -6;
    }
    jobjectArray args = env->NewObjectArray(1,
        env->FindClass("java/lang/String"), env->NewStringUTF(""));
    env->SetObjectArrayElement(args, 0, env->NewStringUTF("--nogui"));

    WriteLaunchLog(L"RunEmbeddedServer: calling KnotServer.main");
    env->CallStaticVoidMethod(knotServerClass, mainMethod, args);

    if (env->ExceptionCheck()) {
        LogJavaException(env, L"KnotServer.main");
    }

    WriteLaunchLog(L"RunEmbeddedServer: server main exited");

    jvmPtr->DestroyJavaVM();
    return 0;
}
static int RunEmbeddedServerSEH(const ServerThreadContext& ctx, bool& success) {
    __try {
        int result = RunEmbeddedServer(ctx);
        success = (result == 0);
        if (result != 0) {
            WCHAR buf[128];
            swprintf_s(buf, 128, L"[MC.Server] JVM exited with code %d", result);
            OutputDebugStringW(buf);
        }
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringW(L"[MC.Server] SEH exception in JVM thread");
        success = false;
        return -99;
    }
}

DWORD WINAPI ServerThreadEntry(LPVOID param) {
    ServerThreadContext* ctx = (ServerThreadContext*)param;
    WriteLaunchLog(L"ServerThreadEntry: started");

    if (!SetupStdinPipe()) {
        WriteLaunchLogF(L"ServerThreadEntry: SetupStdinPipe FAILED err=%u", GetLastError());
        *(ctx->pSuccess) = false;
        SetEvent(ctx->shutdownEvent);
        return 1;
    }
    WriteLaunchLog(L"ServerThreadEntry: stdin pipe ready");

    if (!RedirectStdioToFiles(ctx->stdoutLogPath, ctx->stderrLogPath)) {
        WriteLaunchLog(L"ServerThreadEntry: RedirectStdioToFiles FAILED");
        *(ctx->pSuccess) = false;
        SetEvent(ctx->shutdownEvent);
        return 2;
    }

    std::wstring tmpDir = ctx->localStateDir + L"\\tmp";
    EnsureDirectoryTree(tmpDir);
    WriteLaunchLogF(L"ServerThreadEntry: tmpDir=%s", tmpDir.c_str());

    g_serverRunning = true;
    WriteLaunchLog(L"ServerThreadEntry: invoking RunEmbeddedServerSEH");

    RunEmbeddedServerSEH(*ctx, *(ctx->pSuccess));

    WriteLaunchLogF(L"ServerThreadEntry: RunEmbeddedServerSEH returned, success=%d", *(ctx->pSuccess) ? 1 : 0);
    g_serverRunning = false;
    CloseStdinPipe();
    SetEvent(ctx->shutdownEvent);

    return 0;
}
