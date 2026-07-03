#include <cstring>
#include <cstdio>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cinttypes>
#include <cerrno>
#include "hack.h"
#include "zygisk.hpp"
#include "game.h"
#include "log.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

// Write a marker file to `path`, logging success/failure (with errno) either way.
static bool write_marker(const char *path, const char *tag) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LOGW("[%s] marker write FAILED at %s (errno=%d %s)", tag, path, errno, strerror(errno));
        return false;
    }
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "loaded pid=%d uid=%d\n", getpid(), getuid());
    write(fd, buf, (size_t)len);
    close(fd);
    LOGI("[%s] marker OK at %s", tag, path);
    return true;
}

// Read our own package/process name from /proc/self/cmdline.
static std::string self_process_name() {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return {};
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return {};
    return std::string(buf); // cmdline is NUL-separated; first token is argv[0]
}

// Constructor: runs the moment the .so is loaded into memory.
// This fires for EVERY process Zygisk injects into (not just the game) —
// preAppSpecialize decides later whether to keep hacking. Useful to tell apart
// "Zygisk never injects at all" from "Zygisk injects, but writes are blocked".
__attribute__((constructor))
void module_loaded() {
    auto proc = self_process_name();
    LOGI("module .so loaded into process '%s' pid=%d uid=%d", proc.c_str(), getpid(), getuid());

    // 1) Original location — often blocked by SELinux for the untrusted_app
    //    domain (shell_data_file), independent of whether injection worked.
    write_marker("/data/local/tmp/mixmod_loaded.txt", "tmp");

    // 2) App's own private data dir — always writable by the app's own
    //    process regardless of SELinux/scoped-storage restrictions on
    //    external paths. If THIS one is missing, injection itself isn't
    //    happening in this process.
    if (!proc.empty()) {
        std::string path = "/data/data/" + proc + "/mixmod_loaded.txt";
        write_marker(path.c_str(), "app_data");
    }
}

class MyModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        auto package_name = env->GetStringUTFChars(args->nice_name, nullptr);
        auto app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        preSpecialize(package_name, app_data_dir);
        env->ReleaseStringUTFChars(args->nice_name, package_name);
        env->ReleaseStringUTFChars(args->app_data_dir, app_data_dir);
    }

    void postAppSpecialize(const AppSpecializeArgs *) override {
        if (enable_hack) {
            std::thread hack_thread(hack_prepare, game_data_dir, data, length);
            hack_thread.detach();
        }
    }

private:
    Api *api;
    JNIEnv *env;
    bool enable_hack;
    char *game_data_dir;
    void *data;
    size_t length;

    void preSpecialize(const char *package_name, const char *app_data_dir) {
        if (strcmp(package_name, GamePackageName) == 0) {
            LOGI("detect game: %s", package_name);
            enable_hack = true;
            game_data_dir = new char[strlen(app_data_dir) + 1];
            strcpy(game_data_dir, app_data_dir);

#if defined(__i386__)
            auto path = "zygisk/armeabi-v7a.so";
#endif
#if defined(__x86_64__)
            auto path = "zygisk/arm64-v8a.so";
#endif
#if defined(__i386__) || defined(__x86_64__)
            int dirfd = api->getModuleDir();
            int fd = openat(dirfd, path, O_RDONLY);
            if (fd != -1) {
                struct stat sb{};
                fstat(fd, &sb);
                length = sb.st_size;
                data = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
            } else {
                LOGW("Unable to open arm file");
            }
#endif
        } else {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }
};

REGISTER_ZYGISK_MODULE(MyModule)