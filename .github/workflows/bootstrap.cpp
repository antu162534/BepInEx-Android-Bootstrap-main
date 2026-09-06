#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr char kTag[] = "ModBootstrap";
constexpr char kPayloadVersion[] = "mod/payload.version";
constexpr char kPayloadManifest[] = "mod/manifest.txt";
constexpr char kNativeLibraries[] = "mod/native-libs.txt";

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)

using DobbyHookFn = int (*)(void *, void *, void **);
using DobbyPrepareFn = int (*)(void *, void *, void **);
using DobbyCommitFn = int (*)(void *);
using DobbyDestroyFn = int (*)(void *);
using Il2CppInitFn = void *(*)(const char *);
using JniOnLoadFn = jint (*)(JavaVM *, void *);
using CoreClrInitializeFn = int (*)(const char *, const char *, int, const char **,
                                    const char **, void **, unsigned int *);
using CoreClrCreateDelegateFn = int (*)(void *, unsigned int, const char *, const char *,
                                        const char *, void **);
using ManagedEntryFn = void (*)();

struct Runtime {
    AAssetManager *assets = nullptr;
    std::string root;
    std::string native_dir;
    std::string game_dir;
    std::string core_dir;
    std::string dotnet_dir;
    std::string package_name;
};

Runtime runtime;
JavaVM *java_vm = nullptr;
Il2CppInitFn original_il2cpp_init = nullptr;
std::atomic_bool coreclr_started{false};
DobbyHookFn dobby_hook = nullptr;
DobbyPrepareFn dobby_prepare = nullptr;
DobbyCommitFn dobby_commit = nullptr;
DobbyDestroyFn dobby_destroy = nullptr;
std::mutex dobby_mutex;

struct BridgeMapping {
    void *address = nullptr;
    size_t size = 0;
};

std::unordered_map<void *, BridgeMapping> return_buffer_bridges;
thread_local void *return_buffer = nullptr;

extern "C" void mod_return_buffer_bridge();

std::string join_path(const std::string &left, const std::string &right);

bool resolve_dobby()
{
    if (dobby_hook && dobby_prepare && dobby_commit && dobby_destroy) return true;
    void *dobby = dlopen(join_path(runtime.native_dir, "libdobby.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!dobby) dobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!dobby) return false;

    dobby_hook = reinterpret_cast<DobbyHookFn>(dlsym(dobby, "DobbyHook"));
    dobby_prepare = reinterpret_cast<DobbyPrepareFn>(dlsym(dobby, "DobbyPrepare"));
    dobby_commit = reinterpret_cast<DobbyCommitFn>(dlsym(dobby, "DobbyCommit"));
    dobby_destroy = reinterpret_cast<DobbyDestroyFn>(dlsym(dobby, "DobbyDestroy"));
    return dobby_hook && dobby_prepare && dobby_commit && dobby_destroy;
}

BridgeMapping create_return_buffer_bridge(void *replacement)
{
#if defined(__aarch64__)
    const long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) return {};
    const size_t page_size = static_cast<size_t>(page_size_value);
    void *memory = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return {};

    auto *code = static_cast<uint32_t *>(memory);
    code[0] = 0xd503245f; // bti c
    code[1] = 0x580000b1; // ldr x17, replacement
    code[2] = 0x580000d0; // ldr x16, mod_return_buffer_bridge
    code[3] = 0xd61f0200; // br x16
    code[4] = 0xd503201f;
    code[5] = 0xd503201f;
    auto *literals = reinterpret_cast<uintptr_t *>(code + 6);
    literals[0] = reinterpret_cast<uintptr_t>(replacement);
    literals[1] = reinterpret_cast<uintptr_t>(&mod_return_buffer_bridge);

    __builtin___clear_cache(static_cast<char *>(memory),
                            static_cast<char *>(memory) + page_size);
    if (mprotect(memory, page_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(memory, page_size);
        return {};
    }
    return {memory, page_size};
#else
    (void) replacement;
    return {};
#endif
}

std::string join_path(const std::string &left, const std::string &right)
{
    if (left.empty()) return right;
    if (right.empty()) return left;
    return left.back() == '/' ? left + right : left + "/" + right;
}

bool mkdirs(const std::string &path)
{
    if (path.empty()) return false;
    std::string current;
    if (path[0] == '/') current = "/";
    for (size_t start = path[0] == '/' ? 1 : 0; start <= path.size();) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        if (end > start) {
            current = join_path(current, path.substr(start, end - start));
            if (mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
        if (end == path.size()) break;
        start = end + 1;
    }
    return true;
}

std::string parent_path(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool safe_relative(const std::string &path)
{
    return !path.empty() && path[0] != '/' && path.find("..") == std::string::npos;
}

JNIEnv *get_env()
{
    JNIEnv *env = nullptr;
    if (!java_vm || java_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
        return nullptr;
    return env;
}

jobject current_application(JNIEnv *env)
{
    jclass klass = env->FindClass("android/app/ActivityThread");
    if (!klass) return nullptr;
    jmethodID method = env->GetStaticMethodID(klass, "currentApplication",
                                               "()Landroid/app/Application;");
    if (!method) return nullptr;
    return env->CallStaticObjectMethod(klass, method);
}

std::string file_path(JNIEnv *env, jobject file)
{
    if (!file) return {};
    jclass klass = env->GetObjectClass(file);
    jmethodID method = env->GetMethodID(klass, "getAbsolutePath", "()Ljava/lang/String;");
    if (!method) return {};
    auto value = static_cast<jstring>(env->CallObjectMethod(file, method));
    if (!value) return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return result;
}

std::string java_string(JNIEnv *env, jstring value)
{
    if (!value) return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return result;
}

std::string application_file_dir(JNIEnv *env, jobject application)
{
    jclass klass = env->GetObjectClass(application);
    jmethodID method = env->GetMethodID(klass, "getFilesDir", "()Ljava/io/File;");
    return method ? file_path(env, env->CallObjectMethod(application, method)) : std::string();
}

std::string application_package_name(JNIEnv *env, jobject application)
{
    jclass klass = env->GetObjectClass(application);
    jmethodID method = env->GetMethodID(klass, "getPackageName", "()Ljava/lang/String;");
    if (!method) return {};
    return java_string(env, static_cast<jstring>(env->CallObjectMethod(application, method)));
}

#ifdef MOD_ROOT_EXTERNAL
std::string application_external_file_dir(JNIEnv *env, jobject application)
{
    jclass klass = env->GetObjectClass(application);
    jmethodID method = env->GetMethodID(
            klass, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
    if (!method) return {};
    jobject file = env->CallObjectMethod(application, method, nullptr);
    return file_path(env, file);
}

int android_sdk_int(JNIEnv *env)
{
    jclass version = env->FindClass("android/os/Build$VERSION");
    if (!version) return 0;
    jfieldID field = env->GetStaticFieldID(version, "SDK_INT", "I");
    if (!field) return 0;
    return env->GetStaticIntField(version, field);
}
#endif

std::string application_native_dir(JNIEnv *env, jobject application)
{
    jclass context = env->GetObjectClass(application);
    jmethodID get_info = env->GetMethodID(
            context, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    if (!get_info) return {};
    jobject info = env->CallObjectMethod(application, get_info);
    jclass info_class = env->FindClass("android/content/pm/ApplicationInfo");
    jfieldID field = info_class ? env->GetFieldID(info_class, "nativeLibraryDir", "Ljava/lang/String;") : nullptr;
    if (!field) return {};
    return java_string(env, static_cast<jstring>(env->GetObjectField(info, field)));
}

AAssetManager *application_assets(JNIEnv *env, jobject application)
{
    jclass context = env->GetObjectClass(application);
    jmethodID method = env->GetMethodID(
            context, "getAssets", "()Landroid/content/res/AssetManager;");
    if (!method) return nullptr;
    jobject assets = env->CallObjectMethod(application, method);
    return assets ? AAssetManager_fromJava(env, assets) : nullptr;
}

bool read_asset(const std::string &name, std::string &result)
{
    AAsset *asset = AAssetManager_open(runtime.assets, name.c_str(), AASSET_MODE_BUFFER);
    if (!asset) return false;
    const off_t length = AAsset_getLength(asset);
    result.resize(static_cast<size_t>(length));
    const int read = AAsset_read(asset, result.data(), static_cast<size_t>(length));
    AAsset_close(asset);
    return read == length;
}

bool copy_asset(const std::string &asset_name, const std::string &destination)
{
    if (!safe_relative(asset_name)) return false;
    AAsset *asset = AAssetManager_open(runtime.assets, asset_name.c_str(), AASSET_MODE_STREAMING);
    if (!asset) return false;
    if (!mkdirs(parent_path(destination))) {
        AAsset_close(asset);
        return false;
    }
    const int fd = open(destination.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        AAsset_close(asset);
        return false;
    }
    char buffer[64 * 1024];
    bool success = true;
    int count;
    while ((count = AAsset_read(asset, buffer, sizeof(buffer))) > 0) {
        ssize_t written = 0;
        while (written < count) {
            const ssize_t part = write(fd, buffer + written, static_cast<size_t>(count - written));
            if (part <= 0) {
                success = false;
                break;
            }
            written += part;
        }
        if (!success) break;
    }
    close(fd);
    AAsset_close(asset);
    return success && count == 0;
}

bool extract_payload()
{
    std::string version;
    if (!read_asset(kPayloadVersion, version)) {
        LOGE("payload version is missing");
        return false;
    }
    version.erase(version.find_last_not_of("\r\n") + 1);
    const std::string marker = join_path(runtime.root, ".payload.version");
    std::ifstream old_marker(marker);
    std::string old;
    std::getline(old_marker, old);
    const std::string probe = join_path(runtime.root, "BepInEx/core/BepInEx.Unity.IL2CPP.dll");
    const std::string metadata = join_path(runtime.root, "BepInEx/global-metadata.dat");
    const std::string game_data = join_path(runtime.root, "game/Data/globalgamemanagers");
    if (old == version && access(probe.c_str(), R_OK) == 0 &&
        access(metadata.c_str(), R_OK) == 0 && access(game_data.c_str(), R_OK) == 0)
        return true;

    std::string manifest;
    if (!read_asset(kPayloadManifest, manifest)) {
        LOGE("payload manifest is missing");
        return false;
    }
    std::istringstream lines(manifest);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) return false;
        const std::string asset_name = line.substr(0, tab);
        const std::string relative = line.substr(tab + 1);
        if (!safe_relative(asset_name) || !safe_relative(relative)) return false;
        if (!copy_asset(asset_name, join_path(runtime.root, relative))) {
            LOGE("failed to extract %s", asset_name.c_str());
            return false;
        }
    }
    mkdirs(join_path(runtime.root, "game/Data/Managed"));
    std::ofstream marker_out(marker, std::ios::trunc);
    marker_out << version << '\n';
    return marker_out.good();
}

std::vector<std::string> read_lines(const std::string &asset_name)
{
    std::string contents;
    if (!read_asset(asset_name, contents)) return {};
    std::istringstream input(contents);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) result.push_back(line);
    }
    return result;
}

bool load_runtime_libraries()
{
    for (const std::string &name : read_lines(kNativeLibraries)) {
        if (!safe_relative(name) || name.find('/') != std::string::npos) return false;
        const std::string path = join_path(runtime.native_dir, name);
        void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            LOGE("dlopen %s failed: %s", path.c_str(), dlerror());
            return false;
        }
        if (name == "libSystem.Security.Cryptography.Native.Android.so") {
            auto on_load = reinterpret_cast<JniOnLoadFn>(dlsym(handle, "JNI_OnLoad"));
            if (!on_load) {
                LOGE("JNI_OnLoad is missing from %s", name.c_str());
                return false;
            }
            const jint version = on_load(java_vm, nullptr);
            LOGI("JNI_OnLoad(%s)=0x%x", name.c_str(), version);
            if (version < 0) return false;
        }
    }
    return true;
}

std::string tpa_for(const std::string &directory)
{
    DIR *dir = opendir(directory.c_str());
    if (!dir) return {};
    std::vector<std::string> files;
    while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".dll")
            files.push_back(join_path(directory, name));
    }
    closedir(dir);
    std::sort(files.begin(), files.end());
    std::string result;
    for (const auto &file : files) {
        if (!result.empty()) result += ':';
        result += file;
    }
    return result;
}

void set_environment()
{
    // Paths.SetExecutablePath() removes the final extension from the path. Android package
    // names contain dots, so add a synthetic extension to preserve the complete package name.
    const std::string process_name = runtime.package_name.empty() ? "Game" : runtime.package_name + ".exe";
    const std::string process = join_path(runtime.game_dir, process_name);
    setenv("DOORSTOP_PROCESS_PATH", process.c_str(), 1);
    setenv("DOORSTOP_INVOKE_DLL_PATH",
           join_path(runtime.core_dir, "BepInEx.Unity.IL2CPP.dll").c_str(), 1);
    setenv("DOORSTOP_MANAGED_FOLDER_DIR", join_path(runtime.game_dir, "Data/Managed").c_str(), 1);
    setenv("DOORSTOP_DLL_SEARCH_DIRS", (runtime.core_dir + ":" + runtime.dotnet_dir).c_str(), 1);
    setenv("BEPINEX_GAME_ASSEMBLY_PATH", join_path(runtime.native_dir, "libil2cpp.so").c_str(), 1);
    setenv("DOTNET_EnableDiagnostics", "0", 1);
    setenv("DOTNET_ReadyToRun", "0", 1);
    setenv("DOTNET_EnableWriteXorExecute", "0", 1);
    const std::string temp = join_path(runtime.root, "tmp");
    mkdirs(temp);
    setenv("TMPDIR", temp.c_str(), 1);
}

bool start_coreclr()
{
    if (coreclr_started.exchange(true)) return true;
    void *coreclr = dlopen(join_path(runtime.native_dir, "libcoreclr.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!coreclr) {
        LOGE("libcoreclr.so is not loaded: %s", dlerror());
        return false;
    }
    auto initialize = reinterpret_cast<CoreClrInitializeFn>(dlsym(coreclr, "coreclr_initialize"));
    auto create_delegate = reinterpret_cast<CoreClrCreateDelegateFn>(dlsym(coreclr, "coreclr_create_delegate"));
    if (!initialize || !create_delegate) {
        LOGE("CoreCLR hosting exports are missing");
        return false;
    }

    const std::string tpa = tpa_for(runtime.dotnet_dir) + ":" + tpa_for(runtime.core_dir);
    const std::string app_paths = runtime.dotnet_dir + ":" + runtime.core_dir;
    const std::string native_paths = runtime.native_dir;
    const char *keys[] = {
            "TRUSTED_PLATFORM_ASSEMBLIES",
            "APP_PATHS",
            "APP_CONTEXT_BASE_DIRECTORY",
            "NATIVE_DLL_SEARCH_DIRECTORIES",
    };
    const char *values[] = {
            tpa.c_str(),
            app_paths.c_str(),
            runtime.core_dir.c_str(),
            native_paths.c_str(),
    };
    void *host = nullptr;
    unsigned int domain = 0;
    const std::string host_path = join_path(runtime.native_dir, "libmodbootstrap.so");
    int status = initialize(host_path.c_str(), "ModBootstrap", 4, keys, values, &host, &domain);
    LOGI("coreclr_initialize status=%d", status);
    if (status < 0) return false;

    ManagedEntryFn entry = nullptr;
    status = create_delegate(host, domain, "BepInEx.Unity.IL2CPP", "Doorstop.Entrypoint", "Start",
                             reinterpret_cast<void **>(&entry));
    LOGI("coreclr_create_delegate status=%d", status);
    if (status < 0 || !entry) return false;
    entry();
    LOGI("BepInEx entrypoint returned");
    return true;
}

void *il2cpp_init_hook(const char *domain)
{
    LOGI("il2cpp_init called");
    void *result = original_il2cpp_init ? original_il2cpp_init(domain) : nullptr;
    if (result) start_coreclr();
    else LOGE("original il2cpp_init failed");
    return result;
}

bool install_hook()
{
    void *il2cpp = dlopen(join_path(runtime.native_dir, "libil2cpp.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!il2cpp) il2cpp = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!il2cpp) {
        LOGE("libil2cpp.so is not loaded: %s", dlerror());
        return false;
    }
    void *target = dlsym(il2cpp, "il2cpp_init");
    if (!target || !resolve_dobby()) {
        LOGE("cannot resolve il2cpp_init or DobbyHook");
        return false;
    }
    const int status = dobby_hook(target, reinterpret_cast<void *>(&il2cpp_init_hook),
                                  reinterpret_cast<void **>(&original_il2cpp_init));
    LOGI("DobbyHook(il2cpp_init) status=%d", status);
    return status == 0 && original_il2cpp_init != nullptr;
}

} // namespace

extern "C" __attribute__((visibility("default"))) void mod_store_return_buffer(void *buffer)
{
    return_buffer = buffer;
}

extern "C" __attribute__((visibility("default"))) void *mod_get_return_buffer()
{
    void *result = return_buffer;
    return_buffer = nullptr;
    return result;
}

extern "C" __attribute__((visibility("default"))) int mod_dobby_prepare(
        void *target, void *replacement, int special_return_buffer, void **original_call)
{
    if (!target || !replacement || !original_call) return -1;
    std::lock_guard<std::mutex> lock(dobby_mutex);
    if (!resolve_dobby()) return -2;
    if (return_buffer_bridges.find(target) != return_buffer_bridges.end()) return -3;

    BridgeMapping bridge;
    void *effective_replacement = replacement;
    if (special_return_buffer != 0) {
        bridge = create_return_buffer_bridge(replacement);
        if (!bridge.address) return -4;
        effective_replacement = bridge.address;
    }

    const int status = dobby_prepare(target, effective_replacement, original_call);
    if (status != 0) {
        if (bridge.address) munmap(bridge.address, bridge.size);
        return status;
    }
    if (bridge.address) return_buffer_bridges.emplace(target, bridge);
    return 0;
}

extern "C" __attribute__((visibility("default"))) int mod_dobby_commit(void *target)
{
    if (!target) return -1;
    std::lock_guard<std::mutex> lock(dobby_mutex);
    return resolve_dobby() ? dobby_commit(target) : -2;
}

extern "C" __attribute__((visibility("default"))) int mod_dobby_destroy(void *target)
{
    if (!target) return -1;
    std::lock_guard<std::mutex> lock(dobby_mutex);
    if (!resolve_dobby()) return -2;
    const int status = dobby_destroy(target);
    if (status != 0) return status;

    const auto bridge = return_buffer_bridges.find(target);
    if (bridge != return_buffer_bridges.end()) {
        munmap(bridge->second.address, bridge->second.size);
        return_buffer_bridges.erase(bridge);
    }
    return 0;
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *)
{
    java_vm = vm;
    JNIEnv *env = get_env();
    if (!env) return JNI_VERSION_1_6;
    jobject application = current_application(env);
    if (!application) {
        LOGE("Application is not available");
        return JNI_VERSION_1_6;
    }
#ifdef MOD_ROOT_EXTERNAL
    // 应用专属外部目录（/storage/emulated/0/Android/data/<pkg>/files）在 Android 10+
    // 无需存储权限；低版本或外部存储不可用时回退到内部 files 目录。
    std::string root_dir;
    if (android_sdk_int(env) >= 29) {
        root_dir = application_external_file_dir(env, application);
    }
    if (root_dir.empty()) {
        root_dir = application_file_dir(env, application);
    }
    runtime.root = root_dir + "/mod";
#else
    runtime.root = application_file_dir(env, application) + "/mod";
#endif
    LOGI("mod root: %s", runtime.root.c_str());
    runtime.native_dir = application_native_dir(env, application);
    runtime.game_dir = join_path(runtime.root, "game");
    runtime.core_dir = join_path(runtime.root, "BepInEx/core");
    runtime.dotnet_dir = join_path(runtime.root, "dotnet");
    runtime.package_name = application_package_name(env, application);
    runtime.assets = application_assets(env, application);
    if (!runtime.assets || runtime.native_dir.empty() || runtime.root.empty()) {
        LOGE("failed to resolve Android paths");
        return JNI_VERSION_1_6;
    }
    if (!extract_payload()) {
        LOGE("payload extraction failed");
        return JNI_VERSION_1_6;
    }
    set_environment();
    if (!load_runtime_libraries()) {
        LOGE("runtime library loading failed");
        return JNI_VERSION_1_6;
    }
    if (!install_hook()) LOGE("il2cpp_init hook was not installed");
    else LOGI("bootstrap ready");
    return JNI_VERSION_1_6;
}
