/* Synthetic real ELF files exercise the public loader on non-AMD64 hosts;
 * no foreign machine code is entered except explicit host lifecycle thunks. */
#include "elf-runtime.c"
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>

#define IMAGE_SIZE 0x20000
#define DATA_SIZE 0x4000
#define CODE_ADDRESS 0x10000
static unsigned char image[IMAGE_SIZE];
static char fixture[PW_ELF_PATH];
static unsigned checks;
static void check(int ok) { ++checks; assert(ok); }
static void make_image(void)
{
    memset(image, 0, sizeof(image));
    struct ehdr *h = (void *)image;
    memcpy(h->ident, "\177ELF\2\1\1", 7);
    h->type = 3; h->machine = 62; h->version = 1;
    h->ehsize = sizeof(*h); h->phoff = sizeof(*h); h->phsize = sizeof(struct phdr); h->phnum = 3;
    struct phdr *p = (void *)(image + h->phoff);
    p[0] = (struct phdr){PT_LOAD, PF_R | PF_W, 0, 0, 0, DATA_SIZE, DATA_SIZE, 0x1000};
    p[1] = (struct phdr){PT_DYNAMIC, PF_R | PF_W, 0x200, 0x200, 0,
                         12 * sizeof(struct dyn), 12 * sizeof(struct dyn), 8};
    p[2] = (struct phdr){PT_TLS, PF_R, 0x900, 0x900, 0, 8, 32, 16};
    struct dyn *d = (void *)(image + 0x200);
    struct dyn entries[] = {{4,0x400},{5,0x500},{10,32},{6,0x600},{11,24},
        {7,0x700},{8,5*24},{9,24},{0,0}};
    memcpy(d, entries, sizeof(entries));
    uint32_t hash[] = {1,3,1,0,2,0}; memcpy(image + 0x400, hash, sizeof(hash));
    memcpy(image + 0x500, "\0imported\0exported\0", 20);
    struct sym *s = (void *)(image + 0x600);
    s[1] = (struct sym){1,0x12,0,0,0,0};
    s[2] = (struct sym){10,0x11,0,1,0xa00,8};
    struct rela *rel = (void *)(image + 0x700);
    rel[0] = (struct rela){0x800,8,0xa00};
    rel[1] = (struct rela){0x808,((uint64_t)1<<32)|6,0};
    rel[2] = (struct rela){0x810,((uint64_t)1<<32)|7,0};
    rel[3] = (struct rela){0x818,((uint64_t)2<<32)|1,4};
    rel[4] = (struct rela){0x823,8,0xa00}; /* deliberately unaligned target */
    uint64_t tls = 0x12345678; memcpy(image + 0x900, &tls, 8);
}
static void save(void)
{
    int fd = open(fixture, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0); check(write(fd, image, sizeof(image)) == sizeof(image)); check(!close(fd));
}
static const struct pw_elf_import imports[] = {{"imported",0x12340000}};
static struct pw_elf_runtime runtime;
static struct pw_elf_module *open_image(void)
{
    check(!pw_elf_init(&runtime, imports, 1, NULL, 0));
    save(); return pw_elf_load(&runtime, fixture);
}
static void reject(const char *reason)
{
    check(!open_image());
    if (!strstr(runtime.error, reason)) fprintf(stderr, "expected %s got %s\n", reason, runtime.error);
    check(strstr(runtime.error, reason) != NULL);
    check(!runtime.count && runtime.key_created);
    pw_elf_destroy(&runtime);
    check(!runtime.count && !runtime.key_created);
}
static void add_verified_headers(void)
{
    long page = sysconf(_SC_PAGESIZE);
    check(page > 0 && (uint64_t)page <= DATA_SIZE);
    struct ehdr *h = (void *)image;
    struct phdr *p = (void *)(image + h->phoff);
    h->phnum = 5;
    p[3] = (struct phdr){PT_GNU_RELRO, PF_R, 0x200, 0x200, 0,
                         (uint64_t)page - 0x200, (uint64_t)page - 0x200, 1};
    p[4] = (struct phdr){PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 0, 1};
}
static void sha_identity_tests(void)
{
    static const uint8_t empty_digest[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    static const uint8_t abc_digest[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    uint8_t digest[32], wrong[32];
    pw_elf_sha256("", 0, digest); check(!memcmp(digest, empty_digest, sizeof(digest)));
    pw_elf_sha256("abc", 3, digest); check(!memcmp(digest, abc_digest, sizeof(digest)));

    make_image(); add_verified_headers(); save();
    pw_elf_sha256(image, sizeof(image), digest);
    check(!pw_elf_init(&runtime, imports, 1, NULL, 0));
    struct pw_elf_module *m =
        pw_elf_load_verified(&runtime, fixture, digest, sizeof(image));
    check(m && m->verified && m->verified_size == sizeof(image));
    check(!memcmp(m->verified_sha256, digest, sizeof(digest)));
    check(m->relro_size == runtime.page_size);
    pw_elf_destroy(&runtime);

    memcpy(wrong, digest, sizeof(wrong)); wrong[0] ^= 1;
    check(!pw_elf_init(&runtime, imports, 1, NULL, 0));
    check(!pw_elf_load_verified(&runtime, fixture, wrong, sizeof(image)));
    check(strstr(runtime.error, "SHA-256 mismatch") != NULL && !runtime.count);
    pw_elf_destroy(&runtime);

    check(!pw_elf_init(&runtime, imports, 1, NULL, 0));
    check(!pw_elf_load_verified(&runtime, fixture, digest, sizeof(image) - 1));
    check(strstr(runtime.error, "exact size") != NULL && !runtime.count);
    pw_elf_destroy(&runtime);

    char link[PW_ELF_PATH];
    snprintf(link, sizeof(link), "/tmp/pw-elf-link-%ld.so", (long)getpid());
    check(!symlink(fixture, link));
    check(!pw_elf_init(&runtime, imports, 1, NULL, 0));
    check(!pw_elf_load_verified(&runtime, link, digest, sizeof(image)));
    check(strstr(runtime.error, "verified open") != NULL && !runtime.count);
    pw_elf_destroy(&runtime);
    check(!unlink(link));
}
static void expect_write_fault(void *address)
{
    pid_t child = fork();
    check(child >= 0);
    if (!child) {
        *(volatile unsigned char *)address ^= 1;
        _exit(0);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) check(errno == EINTR);
    check(WIFSIGNALED(status) &&
          (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
}
static void *tls_thread(void *arg)
{
    struct pw_elf_runtime *r = arg;
    uint64_t *p = pw_elf_tls(r, 1, 0);
    check(p && *p == 0x12345678); *p = 0x5555;
    check(pw_elf_tls(r, 1, 0) == p);
    return NULL;
}
/* These synthetic lifecycle images contain native HOST instruction thunks,
 * not Wine/AMD64 instructions. They enter real C callbacks via the public
 * constructor/finalizer API on both ARM64 macOS and AMD64 test hosts. */
static void host_thunk(size_t offset, void (*callback)(void))
{
    uintptr_t address=(uintptr_t)callback;
#if defined(__aarch64__) || defined(__arm64__)
    uint32_t code[]={0x58000050,0xd61f0200}; /* ldr x16,+8; br x16 */
    memcpy(image+offset,code,sizeof(code)); memcpy(image+offset+8,&address,8);
#elif defined(__x86_64__)
    image[offset]=0x48; image[offset+1]=0xb8; /* movabs rax,address; jmp rax */
    memcpy(image+offset+2,&address,8); image[offset+10]=0xff; image[offset+11]=0xe0;
#else
#error lifecycle tests require ARM64 or AMD64 host thunks
#endif
}
static uintptr_t consumer_symbol(struct pw_elf_module *m,const char *name)
{
    check(!pthread_mutex_lock(&runtime.lock));
    uintptr_t address=pw_elf_symbol(&runtime,m,name);
    check(!pthread_mutex_unlock(&runtime.lock));
    return address;
}
static struct pw_elf_module *consumer_open(const char *path)
{
    check(!pthread_mutex_lock(&runtime.lock));
    struct pw_elf_module *m=pw_elf_load(&runtime,path);
    if (m && pw_elf_start(&runtime)) m=NULL;
    check(!pthread_mutex_unlock(&runtime.lock));
    return m;
}
static unsigned fini_tls, fini_log[2], fini_count, dependency_calls, ctor_calls;
static unsigned relro_ctor_calls, policy_ctor_calls;
static char reentry_path[PW_ELF_PATH];
static void relro_constructor(void)
{
    struct pw_elf_module *m=&runtime.modules[0];
    check(m->state==4);
    check(*(uint64_t *)(m->bias+0x800)==m->bias+0xa00);
    expect_write_fault((char *)m->mapping+0x800);
    ++relro_ctor_calls;
}
static void policy_constructor(void) { ++policy_ctor_calls; }
static void finalizer_using_tls(void)
{
    check(runtime.key_created);
    uint64_t *value=pw_elf_tls(&runtime,1,0);
    check(value && *value==0xabcdef); *value=0xfedcba; ++fini_tls;
}
static void dependency_function(void) { ++dependency_calls; }
static void finalizer_a(void)
{
    fini_log[fini_count++]=0;
    check(runtime.modules[0].mapping && runtime.modules[1].mapping);
    /* B has already finalized but must remain callable until all fini ends. */
    uintptr_t function=consumer_symbol(&runtime.modules[1],"exported");
    check(function != 0); ((void (*)(void))function)();
}
static void finalizer_b(void)
{
    fini_log[fini_count++]=1;
    check(runtime.modules[0].mapping && runtime.modules[1].mapping);
    uintptr_t function=consumer_symbol(&runtime.modules[0],"exported");
    check(function != 0); ((void (*)(void))function)();
}
static void reentrant_constructor(void)
{
    ++ctor_calls;
    check(ctor_calls==1); /* Visiting state must prevent recursive re-entry. */
    check(consumer_symbol(&runtime.modules[0],"exported") != 0);
    check(consumer_open(fixture)==&runtime.modules[0]);
    check(consumer_open(reentry_path) != NULL);
}
static void lifecycle_image(void (*init)(void),void (*fini)(void))
{
    make_image();
    long page=sysconf(_SC_PAGESIZE); check(page>0 && (uint64_t)page<=DATA_SIZE);
    struct ehdr *h=(void *)image;
    struct phdr *p=(void *)(image+h->phoff);
    h->phnum=6;
    p[3]=(struct phdr){PT_LOAD,PF_R|PF_X,CODE_ADDRESS,CODE_ADDRESS,0,0x1000,0x1000,0x1000};
    p[4]=(struct phdr){PT_GNU_RELRO,PF_R,0x200,0x200,0,
                       (uint64_t)page-0x200,(uint64_t)page-0x200,1};
    p[5]=(struct phdr){PT_GNU_STACK,PF_R|PF_W,0,0,0,0,0,1};
    struct dyn *d=(void *)(image+0x200);
    unsigned index=8;
    if (init) {
        d[index++]=(struct dyn){12,CODE_ADDRESS};
        host_thunk(CODE_ADDRESS,init);
    }
    if (fini) {
        d[index++]=(struct dyn){13,CODE_ADDRESS+0x20};
        host_thunk(CODE_ADDRESS+0x20,fini);
    }
    d[index]=(struct dyn){0,0};
    host_thunk(CODE_ADDRESS+0x40,dependency_function);
    struct sym *s=(void *)(image+0x600);
    s[2].info=0x12; s[2].value=CODE_ADDRESS+0x40; s[2].size=16;
}
static void write_image(const char *path)
{
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600); check(fd>=0);
    check(write(fd,image,sizeof(image))==sizeof(image)); check(!close(fd));
}
static void capacity_tests(void)
{
    char path[PW_ELF_PATH];
    uint8_t digest[32] = {0};
    check(PW_ELF_MODULES >= 18);
    make_image();
    check(!pw_elf_init(&runtime, imports, 1, NULL, 0));
    for (unsigned i = 0; i < PW_ELF_MODULES; i++)
    {
        snprintf(path, sizeof(path), "/tmp/pw-capacity-%ld-%u.so", (long)getpid(), i);
        write_image(path);
        struct pw_elf_module *module = pw_elf_load(&runtime, path);
        check(module != NULL && runtime.count == i + 1);
        check(pw_elf_load(&runtime, path) == module); /* Reuse needs no new slot. */
        check(!unlink(path));
    }
    check(!pw_elf_load(&runtime, "/nonexistent/pw-capacity-overflow.so"));
    check(strstr(runtime.error, "module/path capacity exceeded") != NULL);
    check(runtime.count == PW_ELF_MODULES);
    check(!pw_elf_load_verified(&runtime, "/nonexistent/pw-capacity-overflow.so",
                                digest, sizeof(image)));
    check(strstr(runtime.error, "module/path capacity exceeded") != NULL);
    check(runtime.count == PW_ELF_MODULES);
    pw_elf_destroy(&runtime);
    check(!runtime.count);
}
static void lifecycle_tests(void)
{
    lifecycle_image(relro_constructor,finalizer_using_tls);
    struct pw_elf_module *m=open_image(); check(m != NULL);
    check(m->segment_count==2 && m->relro_size==runtime.page_size);
    expect_write_fault((char *)m->mapping+0x800);
    expect_write_fault((char *)m->mapping+CODE_ADDRESS);
    check(!pw_elf_start(&runtime));
    check(relro_ctor_calls==1);
    uint64_t *value=pw_elf_tls(&runtime,1,0); check(value != NULL); *value=0xabcdef;
    pw_elf_destroy(&runtime);
    check(fini_tls==1 && !runtime.key_created && !runtime.lock_created);
    /* A bad later module must be rejected before an earlier valid constructor. */
    char invalid[PW_ELF_PATH];
    snprintf(invalid,sizeof(invalid),"/tmp/pw-invalid-lifecycle-%ld.so",(long)getpid());
    lifecycle_image(policy_constructor,NULL); m=open_image(); check(m != NULL);
    make_image(); ((struct dyn *)(image+0x200))[8]=(struct dyn){12,IMAGE_SIZE+8};
    write_image(invalid);
    check(pw_elf_load(&runtime,invalid) != NULL);
    check(pw_elf_start(&runtime)<0 && policy_ctor_calls==0);
    pw_elf_destroy(&runtime); check(!unlink(invalid));
    /* Late B depends on already initialized A: reverse initialization order,
     * not reverse/insertion guesses, with all mappings live for both fini. */
    lifecycle_image(NULL,finalizer_a); m=open_image(); check(m != NULL);
    check(!pw_elf_start(&runtime));
    snprintf(reentry_path,sizeof(reentry_path),"/tmp/pw-reentry-%ld.so",(long)getpid());
    lifecycle_image(NULL,finalizer_b);
    struct dyn *d=(void *)(image+0x200);
    d[2].value=128; d[9]=(struct dyn){1,32};
    strcpy((char *)image+0x520,strrchr(fixture,'/')+1);
    write_image(reentry_path);
    check(consumer_open(reentry_path) != NULL);
    check(runtime.initialized_count==2 && runtime.initialized[0]==0 && runtime.initialized[1]==1);
    pw_elf_destroy(&runtime);
    check(fini_count==2 && fini_log[0]==1 && fini_log[1]==0 && dependency_calls==2);
    check(!unlink(reentry_path));
    /* Exercise the same recursive context lock used by native open_module/
     * find_symbol while a real module constructor re-enters both APIs. */
    lifecycle_image(NULL,NULL); write_image(reentry_path);
    lifecycle_image(reentrant_constructor,NULL);
    check(!pw_elf_init(&runtime,imports,1,NULL,0)); save();
    alarm(10); /* A lock regression must fail boundedly, not hang the suite. */
    check(consumer_open(fixture) != NULL); alarm(0);
    check(ctor_calls==1 && runtime.count==2 && runtime.initialized_count==2);
    pw_elf_destroy(&runtime); check(!unlink(reentry_path));
}
static void verified_layout_tests(void)
{
    uint8_t digest[32];
    unsigned constructors_before = ctor_calls;
    lifecycle_image(reentrant_constructor,NULL);
    save(); pw_elf_sha256(image,sizeof(image),digest);
    check(!pw_elf_init(&runtime,imports,1,NULL,0));
    check(!pw_elf_load_verified(&runtime,fixture,digest,sizeof(image)));
    check(strstr(runtime.error,"verified module constructors") != NULL);
    check(ctor_calls==constructors_before && !runtime.count);
    pw_elf_destroy(&runtime);

    make_image(); add_verified_headers();
    struct dyn *d=(void *)(image+0x200);
    d[2].value=128; d[8]=(struct dyn){1,32}; d[9]=(struct dyn){0,0};
    strcpy((char *)image+0x520,"unmanifested.so");
    save(); pw_elf_sha256(image,sizeof(image),digest);
    check(!pw_elf_init(&runtime,imports,1,NULL,0));
    check(!pw_elf_load_verified(&runtime,fixture,digest,sizeof(image)));
    check(strstr(runtime.error,"verified module dependency unsupported") != NULL);
    check(!runtime.count);
    pw_elf_destroy(&runtime);
}

int main(void)
{
    snprintf(fixture, sizeof(fixture), "/tmp/pw-elf-%ld.so", (long)getpid());
    sha_identity_tests();
    make_image();
    struct pw_elf_module *m = open_image(); check(m != NULL);
    uint64_t *p = (void *)(m->bias + 0x800);
    check(p[0] == m->bias + 0xa00); check(p[1] == 0x12340000); check(p[2] == 0x12340000);
    check(p[3] == m->bias + 0xa04);
    uint64_t unaligned; memcpy(&unaligned, (void *)(m->bias + 0x823), 8);
    check(unaligned == m->bias + 0xa00);
    check(pw_elf_symbol(&runtime, m, "exported") == m->bias + 0xa00);
    uint64_t *tls = pw_elf_tls(&runtime, 1, 0); check(tls && *tls == 0x12345678);
    check(((uintptr_t)tls & 15) == 0); check(*(uint64_t *)pw_elf_tls(&runtime, 1, 16) == 0);
    *tls = 0xabcd;
    pthread_t thread; check(!pthread_create(&thread, NULL, tls_thread, &runtime));
    check(!pthread_join(thread, NULL)); check(*tls == 0xabcd);
    check(!pw_elf_tls(&runtime, 1, 32));
    pw_elf_destroy(&runtime);
    make_image(); ((struct ehdr *)image)->machine = 183; reject("header");
    make_image(); ((struct ehdr *)image)->phoff = UINT64_MAX - 8; reject("header");
    make_image(); ((struct ehdr *)image)->phnum = PW_ELF_SEGMENTS + 1; reject("header");
    make_image(); ((struct phdr *)(image + 64))[0].memsz = UINT64_MAX; reject("page overflow");
    make_image(); ((struct phdr *)(image + 64))[0].filesz = IMAGE_SIZE + 1; reject("extent");
    make_image(); ((struct phdr *)(image + 64))[0].align = 3; reject("alignment");
    make_image(); ((struct phdr *)(image + 64))[2].type = 1; reject("overlapping");
    make_image(); ((struct phdr *)(image + 64))[2].align = 0x100000; reject("alignment");
    make_image();
    ((struct phdr *)(image+64))[0].flags=PF_R|PF_W|PF_X;
    reject("writable/executable PT_LOAD");
    make_image();
    long page_size=sysconf(_SC_PAGESIZE); check(page_size>=0x1000);
    struct ehdr *shared_header=(void *)image;
    struct phdr *shared=(void *)(image+shared_header->phoff);
    shared_header->phnum=4;
    shared[0].filesz=shared[0].memsz=0xb00;
    uint64_t shared_code=(uint64_t)page_size-0x100;
    shared[3]=(struct phdr){PT_LOAD,PF_R|PF_X,shared_code,shared_code,0,0x80,0x80,1};
    reject("permission union is writable/executable");
    make_image();
    struct ehdr *stack_header=(void *)image;
    struct phdr *stack=(void *)(image+stack_header->phoff);
    stack_header->phnum=4;
    stack[3]=(struct phdr){PT_GNU_STACK,PF_R|PF_W|PF_X,0,0,0,0,0,1};
    reject("executable PT_GNU_STACK");
    make_image();
    struct ehdr *relro_header=(void *)image;
    struct phdr *relro=(void *)(image+relro_header->phoff);
    relro_header->phnum=4;
    relro[3]=(struct phdr){PT_GNU_RELRO,PF_R,0,
                           UINT64_MAX-(uint64_t)page_size/2,0,0,(uint64_t)page_size,1};
    reject("invalid/writable PT_GNU_RELRO");
    make_image();
    relro_header=(void *)image; relro=(void *)(image+relro_header->phoff);
    relro_header->phnum=4;
    relro[3]=(struct phdr){PT_GNU_RELRO,PF_R,0x200,0x200,0,
                           (uint64_t)page_size-0x100,(uint64_t)page_size-0x100,1};
    reject("end is not page-aligned");
    make_image();
    relro_header=(void *)image; relro=(void *)(image+relro_header->phoff);
    relro_header->phnum=4;
    relro[3]=(struct phdr){PT_GNU_RELRO,PF_R|PF_W,0,0,0,
                           (uint64_t)page_size,(uint64_t)page_size,1};
    reject("invalid/writable PT_GNU_RELRO");
    make_image(); ((struct dyn *)(image + 0x200))[0].value = 0x1fff; reject("hash");
    make_image(); ((struct dyn *)(image + 0x200))[2].value = UINT64_MAX; reject("tables");
    make_image(); ((struct rela *)(image + 0x700))[0].offset = IMAGE_SIZE - 4; reject("target");
    make_image(); ((struct rela *)(image + 0x700))[0].info = 18; reject("relocation: 18");
    make_image(); ((struct rela *)(image + 0x700))[0].addend = INT64_MIN; reject("RELATIVE");
    make_image(); ((struct rela *)(image + 0x700))[1].info = ((uint64_t)33<<32)|6; reject("symbol");
    make_image(); ((struct sym *)(image + 0x600))[1].name = 99; reject("symbol name");
    make_image(); memcpy(image + 0x501, "missing\0", 8); reject("unresolved import: missing");
    make_image(); ((struct dyn *)(image + 0x200))[7].value = 8; reject("RELAENT");
    make_image(); ((struct dyn *)(image + 0x200))[6].value = 25; reject("RELA table");
    make_image(); ((struct dyn *)(image + 0x200))[8].tag = 22; reject("dynamic relocation tag");
    make_image(); for (unsigned i=8; i<12; ++i) ((struct dyn *)(image + 0x200))[i].tag=21; reject("tables");
    make_image(); ((struct dyn *)(image + 0x200))[3].value = 0x601; reject("symbol table");
    make_image(); ((struct dyn *)(image + 0x200))[5].value = 0x701; reject("RELA table");
    make_image(); ((struct phdr *)(image + 64))[1].address = 0x201; reject("alignment");
    make_image(); ((struct dyn *)(image + 0x200))[8] = (struct dyn){1,1}; reject("open");
    /* Local dynamic TLS relocations bind module id and template-relative offset. */
    make_image();
    struct sym *s = (void *)(image + 0x600); s[2].info = 0x16; s[2].value = 8; s[2].size = 8;
    struct rela *rel = (void *)(image + 0x700);
    rel[3].info = ((uint64_t)2<<32)|16; rel[3].addend = 0;
    rel[4].info = ((uint64_t)2<<32)|17; rel[4].addend = 4;
    m = open_image(); check(m != NULL);
    memcpy(&unaligned, (void *)(m->bias + 0x823), 8); check(unaligned == 12);
    check(*(uint64_t *)(m->bias + 0x818) == 1); pw_elf_destroy(&runtime);
    rel[3].info = 16; rel[4].info = 17; rel[4].addend = 16;
    m = open_image(); check(m != NULL);
    memcpy(&unaligned, (void *)(m->bias + 0x823), 8); check(unaligned == 16);
    check(*(uint64_t *)(m->bias + 0x818) == 1); pw_elf_destroy(&runtime);
    make_image(); ((struct dyn *)(image + 0x200))[8] = (struct dyn){12,IMAGE_SIZE+8};
    m = open_image(); check(m != NULL); check(pw_elf_start(&runtime)<0);
    check(strstr(runtime.error,"DT_INIT") != NULL); pw_elf_destroy(&runtime);
    uint64_t arithmetic;
    check(signed_add(UINTPTR_MAX,1,&arithmetic)<0);
    check(signed_add(0,INT64_MIN,&arithmetic)<0);
    check(!signed_add(UINTPTR_MAX,-1,&arithmetic) && arithmetic==UINTPTR_MAX-1);
    /* A failed late dlopen does not unmap an already running module graph. */
    make_image(); m=open_image(); check(m != NULL); check(!pw_elf_start(&runtime));
    char late[PW_ELF_PATH]; snprintf(late,sizeof(late),"/tmp/pw-late-%ld.so",(long)getpid());
    int badfd=open(late,O_WRONLY|O_CREAT|O_TRUNC,0600); check(badfd>=0);
    check(write(badfd,"bad",3)==3); check(!close(badfd));
    check(!pw_elf_load(&runtime,late)); check(runtime.count==1 && m->state==3);
    check(pw_elf_symbol(&runtime,m,"exported")==m->bias+0xa00);
    check(!unlink(late)); pw_elf_destroy(&runtime);
    /* Recursive app-local dependencies, including an initialization cycle. */
    char dependency[PW_ELF_PATH];
    snprintf(dependency,sizeof(dependency),"/tmp/pw-dependency-%ld.so",(long)getpid());
    make_image();
    struct dyn *dynamic=(void *)(image+0x200);
    dynamic[2].value=128; dynamic[8]=(struct dyn){1,32};
    strcpy((char *)image+0x520,strrchr(fixture,'/')+1);
    int depfd=open(dependency,O_WRONLY|O_CREAT|O_TRUNC,0600); check(depfd>=0);
    check(write(depfd,image,sizeof(image))==sizeof(image)); check(!close(depfd));
    strcpy((char *)image+0x520,strrchr(dependency,'/')+1);
    m=open_image(); check(m != NULL); check(runtime.count==2);
    check(m->dependency_count==1 && runtime.modules[1].dependency_count==1);
    check(!pw_elf_start(&runtime)); check(m->state==3 && runtime.modules[1].state==3);
    check(!pw_elf_start(&runtime)); /* Constructors cannot be entered twice. */
    pw_elf_destroy(&runtime); check(!unlink(dependency));
    /* GNU hash path, including malformed bucket and unterminated chain. */
    make_image(); ((struct dyn *)(image + 0x200))[0].tag = 0x6ffffef5;
    uint32_t gnu[] = {1,1,1,5,0,0,1,0x100,0x101}; memcpy(image + 0x400,gnu,sizeof(gnu));
    m = open_image(); check(m != NULL); pw_elf_destroy(&runtime);
    ((uint32_t *)(image + 0x400))[6] = 0xffffffff; reject("chain");
    verified_layout_tests();
    lifecycle_tests();
    capacity_tests();
    check(!unlink(fixture));
    printf("ELF_RUNTIME_HOST_PASS checks=%u mapping=public relocation=AMD64 TLS=dynamic-thread-private foreign_execution=NOT_ENTERED lifecycle=HOST_NATIVE\n", checks);
    return 0;
}
