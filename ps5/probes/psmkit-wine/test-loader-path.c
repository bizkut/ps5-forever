/* Exercise Wine's actual directory helper with the existing SDK resolver.
 * The native-title refusal model is explicit; no Wine/PE code is executed. */
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ps5platform/libc.h>
#include "main.h"

static char *title_native_realpath(const char *path,char *resolved)
{
    (void)path; (void)resolved;
    errno=EPERM;
    return NULL; /* documented title-native API restriction, not a runtime fallback */
}
int main(void)
{
    char base[]="/tmp/psm-loader-path-XXXXXX",directory[PATH_MAX],loader[PATH_MAX],input[PATH_MAX];
    assert(mkdtemp(base));
    assert(snprintf(directory,sizeof(directory),"%s/x86_64-unix",base)>0);
    assert(!mkdir(directory,0700));
    assert(snprintf(loader,sizeof(loader),"%s/wine",directory)>0);
    int fd=open(loader,O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);assert(!close(fd));
    assert(snprintf(input,sizeof(input),"%s/x86_64-unix/../x86_64-unix//./wine",base)>0);
    errno=0;
    assert(!wine_loader_realpath_dirname(input,title_native_realpath) && errno==EPERM);
    char *resolved=wine_loader_realpath_dirname(input,ps5_realpath);
    assert(resolved && !strcmp(resolved,directory));free(resolved);
    char missing[PATH_MAX];assert(snprintf(missing,sizeof(missing),"%s/missing",directory)>0);
    errno=0;assert(!wine_loader_realpath_dirname(missing,ps5_realpath) && errno==ENOENT);
    assert(!unlink(loader));assert(!rmdir(directory));assert(!rmdir(base));
    puts("LOADER_PATH_HOST_PASS native_refusal=EPERM public_resolver=SDK_STAT_NORMALIZATION actual_Wine_directory_helper=1 Wine_PE=NOT_RUN");
    return 0;
}
