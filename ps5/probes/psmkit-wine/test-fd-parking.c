#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <wine/list.h>

#define FILE_DISPOSITION_DELETE 0x00000001u
#define FILE_DELETE_ON_CLOSE    0x00001000u
#define PARK_BATCH 32
#define PARK_MIN_IDLE 2000

enum park_block_reason
{
    BLOCK_RECENT,
    BLOCK_LOCKED,
    BLOCK_CLOSED,
    BLOCK_DELETE,
    BLOCK_ASYNC,
    BLOCK_POLLED,
    BLOCK_SHARED,
    BLOCK_NONE,
    BLOCK_REASONS
};

struct async_queue { int queued; };
struct device { dev_t dev; };
struct closed_fd
{
    struct list entry;
    int unix_fd;
    unsigned int disp_flags;
    char *unix_name;
};
struct inode
{
    struct device *device;
    ino_t ino;
    struct list open;
    struct list locks;
    struct list closed;
};
struct fd
{
    struct inode *inode;
    struct list inode_entry;
    struct closed_fd *closed;
    struct list locks;
    unsigned int options;
    char *unix_name;
    int unix_fd;
    int poll_index;
    struct async_queue read_q, write_q, wait_q;
    void *completion;
    struct list park_entry;
    unsigned long long park_used;
    off_t park_pos;
    int reopen_flags;
    unsigned int parkable :1;
    unsigned int park_member :1;
    unsigned int parked :1;
    unsigned int desc_id;
    unsigned int cacheable :1;
};

static struct pollfd pollfd[8];
static struct list park_lru = LIST_INIT( park_lru );
static unsigned int park_live, park_parked;
static unsigned long park_parks, park_unparks, park_failures;
static unsigned long park_group_parks, park_group_unparks, park_group_fds;

static int async_queued(const struct async_queue *queue) { return queue->queued; }
static unsigned int park_make_room(void) { return 0; }
static void park_balance(void) {}
static void inprocess_trace(const char *format, ...)
{
    (void)format;
}

#include "fd-parking-functions.inc"

static int failures;

static void check(int condition, const char *name)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static void init_member(struct fd *fd, struct closed_fd *closed, struct inode *inode,
                        int unix_fd, const char *path, unsigned int desc_id, int leader)
{
    memset(fd, 0, sizeof(*fd));
    memset(closed, 0, sizeof(*closed));
    list_init(&fd->inode_entry);
    list_init(&fd->locks);
    list_init(&fd->park_entry);
    list_init(&closed->entry);
    fd->inode = inode;
    fd->closed = closed;
    fd->unix_fd = unix_fd;
    fd->unix_name = strdup(path);
    assert(fd->unix_name);
    closed->unix_fd = unix_fd;
    closed->unix_name = fd->unix_name;
    fd->poll_index = -1;
    fd->reopen_flags = O_RDWR;
    fd->desc_id = desc_id;
    fd->parkable = leader;
    fd->park_member = leader || desc_id == 77;
    fd->park_used = park_now() - PARK_MIN_IDLE - 100;
    list_add_tail(&inode->open, &fd->inode_entry);
    if (leader) list_add_tail(&park_lru, &fd->park_entry);
}

int main(void)
{
    static const unsigned char contents[] = "group parking keeps mapped and file data";
    char path[] = "/tmp/wine-fd-parking-XXXXXX";
    struct fd source, mapping, independent;
    struct closed_fd source_closed, mapping_closed, independent_closed;
    struct device device;
    struct inode inode;
    struct stat st;
    char moved[sizeof(path) + 8];
    struct list lock;
    unsigned char readback[sizeof(contents)];
    void *old_view, *new_view;
    int source_fd, mapping_fd, independent_fd;
    off_t position = 257;

    source_fd = mkstemp(path);
    assert(source_fd >= 0);
    assert(write(source_fd, contents, sizeof(contents)) == (ssize_t)sizeof(contents));
    assert(ftruncate(source_fd, 4096) == 0);
    assert(lseek(source_fd, position, SEEK_SET) == position);
    mapping_fd = dup(source_fd);
    assert(mapping_fd >= 0);
    independent_fd = open(path, O_RDWR);
    assert(independent_fd >= 0);
    old_view = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mapping_fd, 0);
    assert(old_view != MAP_FAILED);
    assert(fstat(source_fd, &st) == 0);

    device.dev = st.st_dev;
    inode.device = &device;
    inode.ino = st.st_ino;
    list_init(&inode.open);
    list_init(&inode.locks);
    list_init(&inode.closed);
    init_member(&source, &source_closed, &inode, source_fd, path, 77, 1);
    init_member(&mapping, &mapping_closed, &inode, mapping_fd, path, 77, 0);
    init_member(&independent, &independent_closed, &inode, independent_fd, path, 88, 0);
    independent.park_member = 0;
    park_live = 1;

    /* Every group member participates in the blockers, not only the LRU leader. */
    list_init(&lock);
    list_add_tail(&mapping.locks, &lock);
    check(park_idle(1) == 0, "locked mapping member stays live");
    check(source.unix_fd >= 0 && mapping.unix_fd >= 0, "locked group descriptors retained");
    list_remove(&lock);
    list_init(&lock);

    mapping.closed->disp_flags = FILE_DISPOSITION_DELETE;
    check(park_idle(1) == 0, "delete-pending mapping member stays live");
    check(source.unix_fd >= 0 && mapping.unix_fd >= 0, "delete-pending group descriptors retained");
    mapping.closed->disp_flags = 0;
    source.options = FILE_DELETE_ON_CLOSE;
    check(park_idle(1) == 0, "delete-on-close leader stays live");
    source.options = 0;

    /* This is the same operation park_make_room uses under fd pressure. */
    check(park_idle(1) == 1, "mapped description parked under pressure");
    check(source.parked && mapping.parked, "all duplicate members marked parked");
    check(source.unix_fd == -1 && mapping.unix_fd == -1, "all duplicate native fds closed");
    check(independent.unix_fd == independent_fd && !independent.parked,
          "independently opened description not merged");
    check(park_group_parks == 1 && park_group_fds == 2, "group parking counters");
    check(!memcmp(old_view, contents, sizeof(contents)), "existing mapped view survives parking");
    snprintf(moved, sizeof(moved), "%s.moved", path);
    check(!rename(path, moved), "move original while group parked");
    {
        static const char replacement[] = "replacement";
        int replacement_fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        assert(replacement_fd >= 0);
        assert(write(replacement_fd, replacement, sizeof(replacement)) == (ssize_t)sizeof(replacement));
        close(replacement_fd);
    }
    check(touch_fd(&mapping) == -1, "replacement inode is never reopened");
    check(source.parked && mapping.parked && source.unix_fd == -1 && mapping.unix_fd == -1,
          "failed exact-inode reopen leaves complete group parked");
    check(!unlink(path) && !rename(moved, path), "restore original pathname");


    /* Touching the mapping member reopens once and dups the same description. */
    check(touch_fd(&mapping) == 0, "mapping member touch reopens group");
    check(source.unix_fd >= 0 && mapping.unix_fd >= 0, "group native fds restored");
    check(source.desc_id == mapping.desc_id && source.desc_id != 77,
          "reopened duplicates receive one new description id");
    check(independent.desc_id == 88 && independent.unix_fd == independent_fd,
          "independent description unchanged after reopen");
    check(lseek(source.unix_fd, 0, SEEK_CUR) == position &&
          lseek(mapping.unix_fd, 0, SEEK_CUR) == position, "shared offset restored");
    check(lseek(mapping.unix_fd, 9, SEEK_CUR) == position + 9 &&
          lseek(source.unix_fd, 0, SEEK_CUR) == position + 9, "shared offset remains shared");
    check(pread(source.unix_fd, readback, sizeof(readback), 0) == (ssize_t)sizeof(readback) &&
          !memcmp(readback, contents, sizeof(contents)), "file read after reopen");
    new_view = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mapping.unix_fd, 0);
    check(new_view != MAP_FAILED, "new mapping view after reopen");
    if (new_view != MAP_FAILED)
    {
        check(!memcmp(new_view, contents, sizeof(contents)), "new mapping view data after reopen");
        munmap(new_view, 4096);
    }
    check(park_group_unparks == 1 && park_unparks == 1 && park_failures == 1,
          "group unpark and exact-inode failure counters");
    park_forget_fd(&source);
    check(!source.parkable && mapping.parkable && mapping.park_member,
          "LRU leadership transfers to surviving mapping member");
    check(park_live == 1 && park_parked == 0, "leader transfer preserves group accounting");

    munmap(old_view, 4096);
    close(source.unix_fd);
    close(mapping.unix_fd);
    close(independent.unix_fd);
    unlink(path);
    free(source.unix_name);
    free(mapping.unix_name);
    free(independent.unix_name);

    if (failures)
    {
        fprintf(stderr, "fd parking host failures=%d\n", failures);
        return 1;
    }
    puts("FD_PARKING_HOST_PASS");
    return 0;
}
