#define _GNU_SOURCE 1
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windef.h>
#include <winnt.h>

typedef unsigned int data_size_t;
#include "file-security-types.inc"

/* Only allocation and the current token boundary are supplied by the harness. */
static void *mem_alloc( size_t size ) { return malloc( size ); }
static void *mem_append( void *dst, const void *src, size_t size )
{
    memcpy( dst, src, size );
    return (char *)dst + size;
}
static struct { struct generic_map mapping; } file_type;
static struct { void *token; } process;
static struct { typeof(process) *process; } thread = { &process }, *current = &thread;
static int token_sid_present( void *token, const struct sid *sid, int deny )
{
    (void)token;
    (void)deny;
    return equal_sid( sid, &local_user_sid ) || equal_sid( sid, &builtin_admins_sid ) ||
           equal_sid( sid, &builtin_users_sid ) || equal_sid( sid, &interactive_sid );
}
static unsigned int map_access( unsigned int access, const struct generic_map *mapping )
{
    (void)mapping;
    assert( !(access & (GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL)) );
    return access;
}

#define mode_to_sd unix_mode_to_sd
#define sd_to_mode unix_sd_to_mode
#define file_access_to_mode unix_file_access_to_mode
#include "file-security-functions.inc"
#undef mode_to_sd
#undef sd_to_mode
#undef file_access_to_mode

#define __PROSPERO__ 1
static int inprocess_server;
#include "file-security-functions.inc"

static unsigned int failures, comparisons;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf( stderr, "%s:%u: %s\n", __FILE__, __LINE__, #condition ); failures++; \
} } while (0)

static size_t sd_size( const struct security_descriptor *sd )
{
    return sizeof(*sd) + sd->owner_len + sd->group_len + sd->sacl_len + sd->dacl_len;
}

static void check_ace( const struct ace *ace, const struct sid *sid, unsigned int mask,
                       unsigned char flags )
{
    CHECK( ace->type == ACCESS_ALLOWED_ACE_TYPE );
    CHECK( ace->flags == flags );
    CHECK( ace->mask == mask );
    CHECK( ace->size == sizeof(*ace) + sid_len( sid ) );
    CHECK( equal_sid( (const struct sid *)(ace + 1), sid ) );
}

static void test_title( mode_t type )
{
    static const struct sid admins = { 1, 2, {0,0,0,0,0,5}, {32,544} };
    static const struct sid users = { 1, 2, {0,0,0,0,0,5}, {32,545} };
    static const struct sid interactive = { 1, 1, {0,0,0,0,0,5}, {4} };
    static const struct sid packages = { 1, 2, {0,0,0,0,0,15}, {2,1} };
    struct security_descriptor *sd = mode_to_sd( type | 0777, &local_user_sid, &domain_users_sid );
    int present;
    const struct acl *dacl = sd_get_dacl( sd, &present );
    const struct ace *ace = (const struct ace *)(dacl + 1);
    unsigned char flags = type == S_IFDIR ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0;

    CHECK( present && sd->control == SE_DACL_PRESENT );
    CHECK( sd->owner_len == sid_len( &local_user_sid ) );
    CHECK( sd->group_len == sid_len( &domain_users_sid ) );
    CHECK( !memcmp( sd + 1, &local_user_sid, sd->owner_len ) );
    CHECK( !memcmp( (char *)(sd + 1) + sd->owner_len, &domain_users_sid, sd->group_len ) );
    CHECK( dacl->revision == ACL_REVISION && dacl->size == sd->dacl_len );
    CHECK( dacl->count == 7 );
    check_ace( ace, &local_system_sid, 0x001f01ff, flags );
    ace = ace_next( ace );
    check_ace( ace, &local_user_sid, 0x001f01ff, flags );
    if (dacl->count == 7)
    {
        ace = ace_next( ace );
        check_ace( ace, &admins, 0x001f01ff, flags );
        ace = ace_next( ace );
        check_ace( ace, &users, 0x001f01ff, flags );
        ace = ace_next( ace );
        check_ace( ace, &interactive, 0x001f01ff, flags );
        ace = ace_next( ace );
        check_ace( ace, &packages, 0x001200a9, flags );
    }
    ace = ace_next( ace );
    check_ace( ace, &world_sid, 0x001301ff, flags );
    CHECK( (const char *)ace_next( ace ) == (const char *)dacl + dacl->size );
    CHECK( sd_to_mode( sd, &local_user_sid ) == 0777 );
    if (dacl->count == 7)
    {
        struct acl *repair = (struct acl *)dacl;
        struct ace *first = (struct ace *)(repair + 1);
        size_t removed = first->size + ace_next( first )->size;

        /* SET_ACCESS repairs may omit Everyone; PS5 chmod must still keep 0777. */
        sd->dacl_len -= ace->size;
        ((struct acl *)dacl)->size -= ace->size;
        ((struct acl *)dacl)->count--;
        CHECK( sd_to_mode( sd, &local_user_sid ) == 0777 );
        /* The Agent can replace the whole DACL with just BA/BU/IU/AC. */
        memmove( first, (char *)first + removed, repair->size - sizeof(*repair) - removed );
        repair->size -= removed;
        repair->count -= 2;
        sd->dacl_len -= removed;
        CHECK( repair->count == 4 );
        CHECK( sd_to_mode( sd, &local_user_sid ) == 0777 );
    }
    free( sd );
}

static void test_unchanged( int title )
{
    mode_t mode;
    unsigned int type;
    inprocess_server = title;
    for (type = 0; type < 2; type++)
    {
        mode_t file_type = type ? S_IFDIR : S_IFREG;
        for (mode = 0; mode <= 0777; mode++)
        {
            struct security_descriptor *actual, *expected;
            if (title && (mode & S_IRWXO) == S_IRWXO) continue;
            actual = mode_to_sd( file_type | mode, &local_user_sid, &domain_users_sid );
            expected = unix_mode_to_sd( file_type | mode, &local_user_sid, &domain_users_sid );
            CHECK( sd_size( actual ) == sd_size( expected ) );
            if (sd_size( actual ) == sd_size( expected ))
                CHECK( !memcmp( actual, expected, sd_size( actual ) ) );
            free( actual );
            free( expected );
            comparisons++;
        }
    }
}

int main(void)
{
    struct security_descriptor *sd = unix_mode_to_sd( S_IFDIR | 0777, &local_user_sid, &domain_users_sid );
    int present;
    CHECK( sd_get_dacl( sd, &present )->count == 3 );
    CHECK( unix_sd_to_mode( sd, &local_user_sid ) == 0777 );
    free( sd );
    test_unchanged( 0 );
    test_unchanged( 1 );
    inprocess_server = 1;
    test_title( S_IFDIR );
    test_title( S_IFREG );
    printf( "file security: %u unchanged comparisons, %u failures\n", comparisons, failures );
    return failures != 0;
}
