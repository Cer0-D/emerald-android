/*
 * The 3DS's two mounts on Android. Game and port code open "romfs:/..." (the
 * data the 3DSX carries) and "sdmc:/..." (the SD card: saves, settings, log)
 * with plain stdio; on Android both are directories under the app's external
 * files dir. Every libc call that takes a path is wrapped at link time
 * (android.mk: -Wl,--wrap=fopen ...) and translates the two prefixes, so the
 * shared sources keep their paths.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "android_host.h"
#include "3ds_platform.h"

#define ROMFS_PREFIX "romfs:/"
#define SDMC_PREFIX "sdmc:/"

/* NULL when the path needs no translation. */
static const char *Translate(const char *path, char *out, size_t size)
{
    const char *rest, *dir;

    if (!path)
        return NULL;
    if (strncmp(path, ROMFS_PREFIX, sizeof(ROMFS_PREFIX) - 1) == 0)
    {
        rest = path + sizeof(ROMFS_PREFIX) - 1;
        dir = AndroidHost_RomfsDir();
    }
    else if (strncmp(path, SDMC_PREFIX, sizeof(SDMC_PREFIX) - 1) == 0)
    {
        rest = path + sizeof(SDMC_PREFIX) - 1;
        dir = "sdmc";
    }
    else
        return NULL;
    if (snprintf(out, size, "%s/%s/%s", AndroidHost_FilesDir(), dir, rest) >= (int)size)
        return NULL;
    return out;
}

#define MAPPED(path) \
    char mapped_##path[768]; \
    const char *t_##path = Translate(path, mapped_##path, sizeof(mapped_##path)); \
    if (t_##path) path = t_##path

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode)
{
    MAPPED(path);
    return __real_fopen(path, mode);
}

int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{
    MAPPED(from);
    MAPPED(to);
    return __real_rename(from, to);
}

int __real_remove(const char *path);
int __wrap_remove(const char *path)
{
    MAPPED(path);
    return __real_remove(path);
}

int __real_unlink(const char *path);
int __wrap_unlink(const char *path)
{
    MAPPED(path);
    return __real_unlink(path);
}

int __real_mkdir(const char *path, mode_t mode);
int __wrap_mkdir(const char *path, mode_t mode)
{
    MAPPED(path);
    return __real_mkdir(path, mode);
}

int __real_stat(const char *path, struct stat *st);
int __wrap_stat(const char *path, struct stat *st)
{
    MAPPED(path);
    return __real_stat(path, st);
}

int __real_access(const char *path, int mode);
int __wrap_access(const char *path, int mode)
{
    MAPPED(path);
    return __real_access(path, mode);
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;

    if (flags & O_CREAT)
    {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    MAPPED(path);
    return __real_open(path, flags, mode);
}

/* ── CtrFs (3ds_fs.c's API) ──────────────────────────────────────────── */
static bool RelativePathValid(const char *path)
{
    if (!path || !*path || *path == '/' || strchr(path, ':') || strchr(path, '\\') || strstr(path, ".."))
        return false;
    return true;
}

static FILE *Open(const char *base, const char *path, const char *mode)
{
    char full[512];

    if (!RelativePathValid(path) || !mode)
    {
        errno = EINVAL;
        return NULL;
    }
    if (snprintf(full, sizeof(full), "%s%s", base, path) >= (int)sizeof(full))
    {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return fopen(full, mode);
}

bool CtrFs_Init(void)
{
    char dir[600];

    snprintf(dir, sizeof(dir), "%s/sdmc", AndroidHost_FilesDir());
    __real_mkdir(dir, 0777);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/emerald3ds", 0777);
    FILE *file = CtrFs_OpenAsset("boot.txt");
    if (!file)
    {
        CtrLog_Write(CTR_LOG_ERROR, "romfs: boot.txt missing under %s/%s", AndroidHost_FilesDir(), AndroidHost_RomfsDir());
        return true; /* the data check (CtrData_Init) explains what is missing */
    }
    char contents[64] = {0};
    size_t n = fread(contents, 1, sizeof(contents) - 1, file);
    fclose(file);
    while (n > 0 && (contents[n - 1] == '\n' || contents[n - 1] == '\r'))
        contents[--n] = 0;
    CtrLog_Write(CTR_LOG_FS, "romfs mounted, boot.txt: %s", contents);
    return true;
}

void CtrFs_Shutdown(void) {}

FILE *CtrFs_OpenAsset(const char *relativePath) { return Open(ROMFS_PREFIX, relativePath, "rb"); }

FILE *CtrFs_OpenData(const char *relativePath, const char *mode)
{
    return Open(SDMC_PREFIX "3ds/emerald3ds/", relativePath, mode);
}
