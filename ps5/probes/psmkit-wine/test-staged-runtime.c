/* Relocation-only proof for the actual staged AMD64 modules on an ARM host.
 * Import values are intentionally non-callable sentinels. Never execute. */
#include "elf-runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int file_identity(const char *path,uint8_t digest[32],uint64_t *size)
{
    struct stat st;
    if (stat(path,&st) || st.st_size<0 || (uint64_t)st.st_size>SIZE_MAX) return -1;
    FILE *file=fopen(path,"rb"); if (!file) return -1;
    size_t bytes=(size_t)st.st_size;
    unsigned char *contents=malloc(bytes ? bytes : 1);
    int result=-1;
    if (contents && fread(contents,1,bytes,file)==bytes && fgetc(file)==EOF && !ferror(file)) {
        pw_elf_sha256(contents,bytes,digest); *size=bytes; result=0;
    }
    free(contents); fclose(file); return result;
}
int main(int argc,char **argv)
{
    /* imports, ntdll, wineserver, win32u, then the profile's other modules */
    enum { MAX_IMPORTS = 1024 };
    if (argc<5 || argc-2>PW_ELF_MODULES) return 2;
    static struct pw_elf_import imports[MAX_IMPORTS]; static char names[MAX_IMPORTS][128]; size_t count=0;
    FILE *f=fopen(argv[1],"r"); if (!f) return 2;
    while (count<MAX_IMPORTS && fgets(names[count],sizeof(names[count]),f)) {
        names[count][strcspn(names[count],"\r\n")]=0;
        if (!names[count][0] || (count && strcmp(names[count-1],names[count])>=0)) return 2;
        imports[count]=(struct pw_elf_import){names[count],0x10000000+count*16}; ++count;
    }
    if (!feof(f)) return 2;
    fclose(f);
    static const char *const systems[]={"libkernel_web.sprx","libkernel_sys.sprx",
        "libSceLibcInternal.sprx","libSceNet.sprx"};
    uint8_t digests[PW_ELF_MODULES][32]; uint64_t sizes[PW_ELF_MODULES];
    for (int i=2;i<argc;++i)
        assert(!file_identity(argv[i],digests[i-2],&sizes[i-2]));
    struct pw_elf_runtime runtime;
    assert(!pw_elf_init(&runtime,imports,count,systems,4));
    for (int i=2;i<argc;++i) {
        struct pw_elf_module *m=
            pw_elf_load_verified(&runtime,argv[i],digests[i-2],sizes[i-2]);
        if (!m) { fprintf(stderr,"STAGED_ELF_FAIL %s\n",runtime.error); pw_elf_destroy(&runtime); return 1; }
        assert(m->state==2 && !m->tls_id && m->verified && m->relro_size);
        for (unsigned j=0;j<m->segment_count;++j)
            assert((m->segments[j].flags&3)!=3);
        printf("STAGED_ELF_RELOCATION_PASS path=%s mapped=%zu symbols=%llu tls=NONE identity=SHA256 execution=NOT_ENTERED\n",
               argv[i],m->mapping_size,(unsigned long long)m->symbol_count);
    }
    assert(pw_elf_symbol(&runtime,&runtime.modules[0],"__wine_main"));
    assert(pw_elf_symbol(&runtime,&runtime.modules[1],"wineserver_start_inprocess"));
    assert(pw_elf_symbol(&runtime,&runtime.modules[1],"wineserver_call_direct"));
    assert(pw_elf_symbol(&runtime,&runtime.modules[2],"__wine_unix_call_funcs"));
    /* Wine Unix modules export their call table; a library such as
     * libgnutls.so is only dlsym'd by them. */
    for (int i=5;i<argc;++i) {
        const char *base=strrchr(argv[i],'/'); base=base ? base+1 : argv[i];
        if (strncmp(base,"lib",3))
            assert(pw_elf_symbol(&runtime,&runtime.modules[i-2],"__wine_unix_call_funcs"));
    }
    pw_elf_destroy(&runtime);
    /* Admission rejects absent files and an altered immutable win32u digest,
     * before either module text or import resolution can be entered. */
    struct pw_elf_runtime identity_rejected;
    assert(!pw_elf_init(&identity_rejected,imports,count,systems,4));
    assert(!pw_elf_load_verified(&identity_rejected,"/nonexistent/pw-win32u.so",
                                 digests[2],sizes[2]));
    digests[2][0]^=1;
    assert(!pw_elf_load_verified(&identity_rejected,argv[4],digests[2],sizes[2]));
    digests[2][0]^=1;
    pw_elf_destroy(&identity_rejected);
    puts("STAGED_WIN32U_IDENTITY_REJECT missing=YES tampered=YES execution=NOT_ENTERED");
    struct pw_elf_runtime rejected;
    assert(!pw_elf_init(&rejected,NULL,0,systems,4));
    assert(!pw_elf_load_verified(&rejected,argv[2],digests[0],sizes[0]));
    assert(strstr(rejected.error,"unresolved import:") != NULL);
    printf("STAGED_ELF_UNKNOWN_IMPORT_REJECT path=%s detail=%s\n",argv[2],rejected.error);
    pw_elf_destroy(&rejected);
    return 0;
}
