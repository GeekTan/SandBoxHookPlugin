//
// Created by hluwa on 2018/8/19.
//

#include <sys/ptrace.h>
#include <linux/ptrace.h>
#include <cstdio>
#include <sys/wait.h>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <sys/uio.h>
#include <elf.h>
#include "sotool.h"

#include "Inject.h"
#include "fake_dlfcn.h"


Inject::Inject(pid_t pid) : pid(pid) {
    this->dlopen_addr = 0;
    this->dlsym_addr = 0;
    this->remote_buf = 0;
    this->buf_cursor = 0;
    this->handles_cursor = 0;
    memset(this->handles, 0, HANDLES_LIST_SIZE * sizeof(void *));
    this->attach();
    void *malloc_addr = this->get_remote_addr(LIBC_PATH, (void *) malloc);
    void *args[1] = {
            (void *) REMOTE_STR_BUF_SIZE
    };
    this->remote_buf = this->call_addr(malloc_addr, args, 1);
}

Inject::~Inject() {
    if (this->remote_buf) {
        void *free_addr = this->get_remote_addr(LIBC_PATH, (void *) free);
        void *args[1] = {
                this->remote_buf
        };
        this->call_addr(free_addr, args, 1);
    }
    if (this->handles) {
        for (int i = 0; i < HANDLES_LIST_SIZE; i++) {
            if (!this->dlclose_addr) {
                this->dlclose_addr = this->get_remote_addr(LINKER_PATH, (void *) dlclose);
            }
            if (this->handles[i]) {
                void *args[1] = {
                        this->handles[i]
                };
                this->call_addr(this->dlclose_addr, args, 1);
            }

        }
    }
    this->detach();
}


int Inject::detach() {
    if (ptrace(PTRACE_DETACH, this->pid, NULL, 0) < 0) {
        this->status = HLUWA_STATUS_FAILD;
        return -1;
    }
    this->status = HLUWA_STATUS_SUCCESS;
    return 0;
}

int Inject::attach() {
    if (ptrace(PTRACE_ATTACH, this->pid, NULL, 0) < 0) {
        this->status = HLUWA_STATUS_FAILD;
        LOGD("attach %s", "failure");
        return -1;
    }
    LOGD("attach %s", "success");
    this->status = HLUWA_STATUS_SUCCESS;
    return 0;
}

int Inject::getregs(struct pt_regs *regs) {
#if defined (__aarch64__)
    int regset = NT_PRSTATUS;
    struct iovec ioVec;

    ioVec.iov_base = regs;
    ioVec.iov_len = sizeof(*regs);
    if (ptrace(PTRACE_GETREGSET, pid, (void *) regset, &ioVec) < 0) {
        this->status = HLUWA_STATUS_FAILD;
        return -1;
    }
    this->status = HLUWA_STATUS_SUCCESS;
    return 0;
#else
    int res = ptrace(PTRACE_GETREGS, this->pid, NULL, regs);
    if (res < 0) {
        this->status = HLUWA_STATUS_FAILD;
        return -1;
    }
    this->status = HLUWA_STATUS_SUCCESS;
    return res;

#endif
}

int Inject::setregs(struct pt_regs *regs) {
#if defined (__aarch64__)
    int regset = NT_PRSTATUS;
    struct iovec ioVec;

    ioVec.iov_base = regs;
    ioVec.iov_len = sizeof(*regs);
    int res = ptrace(PTRACE_SETREGSET, pid, (void *) regset, &ioVec);
    if (res < 0) {
        this->status = HLUWA_STATUS_FAILD;
        return -1;
    }
    this->status = HLUWA_STATUS_SUCCESS;
    return res;
#else
    int res = ptrace(PTRACE_SETREGS, this->pid, NULL, regs);
    if (res < 0) {
        this->status = HLUWA_STATUS_FAILD;
        return -1;
    }
    this->status = HLUWA_STATUS_SUCCESS;
    return res;

#endif
}

int Inject::cont() {
    int res = ptrace(PTRACE_CONT, this->pid, NULL, 0);
    if (res < 0) {
        return -1;
    }

    return res;
}

int Inject::read_data(uint8_t *src, uint8_t *buf, size_t size) {
    uint32_t i, j, remain;
    uint8_t *laddr;

    union u {
        long val;
        char chars[sizeof(long)];
    } d;

    j = size / 4;
    remain = size % 4;

    laddr = buf;

    for (i = 0; i < j; i++) {
        d.val = ptrace(PTRACE_PEEKTEXT, this->pid, src, 0);
        memcpy(laddr, d.chars, 4);
        src += 4;
        laddr += 4;
    }

    if (remain > 0) {
        d.val = ptrace(PTRACE_PEEKTEXT, this->pid, src, 0);
        memcpy(laddr, d.chars, remain);
    }

    return 0;
}

int Inject::read_string(uint8_t *remote_addr, uint8_t *dest, size_t max_len) {
    bool eos = false;
    while (!eos) {
        union u {
            long val;
            char chars[sizeof(long)];
        } d;

        d.val = ptrace(PTRACE_PEEKTEXT, this->pid, remote_addr, 0);
        remote_addr += 4;
        for (int i = 0; i < 4; i++) {
            if (d.chars[i] == 0x0 || max_len <= 1) {
                *dest = 0;
                eos = true;
                break;
            }
            *dest = d.chars[i];
            dest++;
            max_len--;
        }
    }
    return 0;
}

int Inject::write_data(uint8_t *dest, uint8_t *data, size_t size) {
    uint32_t i, j, remain;
    uint8_t *laddr;

    union u {
        long val;
        char chars[sizeof(long)];
    } d;

    j = size / 4;
    remain = size % 4;

    laddr = data;

    for (i = 0; i < j; i++) {
        memcpy(d.chars, laddr, 4);
        ptrace(PTRACE_POKETEXT, this->pid, dest, d.val);

        dest += 4;
        laddr += 4;
    }

    if (remain > 0) {
        d.val = ptrace(PTRACE_PEEKTEXT, this->pid, dest, 0);
        for (i = 0; i < remain; i++) {
            d.chars[i] = *laddr++;
        }

        ptrace(PTRACE_POKETEXT, this->pid, dest, d.val);
    }

    return 0;
}

void *Inject::write_string(char *str) {
    unsigned long result;
    int len = strlen(str) + 1;
    if (this->remote_buf && len + this->buf_cursor >= REMOTE_STR_BUF_SIZE) {
        this->status = HLUWA_STATUS_FAILD;
        return 0;
    }
    result = (unsigned long) this->remote_buf + (unsigned long) this->buf_cursor;
    this->write_data((uint8_t *) result, (uint8_t *) str, len);
    this->buf_cursor += len;
    this->status = HLUWA_STATUS_SUCCESS;
    printf("write_string: %s, to: %p\n", str, (void *) result);
    return (void *) result;
}

void *Inject::call_sym(char *module, char *sym, void **_args, int argc) {
    if (!this->dlsym_addr) {
        this->dlsym_addr = get_remote_addr(LINKER_PATH, (void *) dlsym);
    }
    void *soinfo = this->loadlibrary(module);
    if (soinfo) {
        void *args[2] = {
                soinfo,
                this->write_string(sym)
        };
        void *sym_addr = this->call_addr(this->dlsym_addr, args, 2);
        printf("entry sym_addr = %p\n", sym_addr);
        LOGD("call sym %s addr %lu", sym, sym_addr);
        if (sym_addr) {
            this->status = HLUWA_STATUS_SUCCESS;
            return this->call_addr(sym_addr, _args, argc);
        } else {
            char buf[256] = "";
            void *err_str = this->call_addr(this->get_remote_addr(LINKER_PATH, (void *) dlerror),
                                            NULL, 0);
            this->read_string((uint8_t *) err_str, (uint8_t *) buf, 256);
            printf("dlerror: %s\n", buf);
            LOGD("call sym %s %s", sym,"error");
            this->status = HLUWA_STATUS_DLSYM_ERROR;
            return 0;
        }

    }
    LOGD("call sym %s %s", sym,"success");
    this->status = HLUWA_STATUS_DLOPEN_ERROR;
    return 0;
}

void *Inject::call_addr(void *remote_addr, void **args, int argc) {
    struct pt_regs return_regs = {0};
    printf("remote_addr = %p, argc = %d\n", remote_addr, argc);
    struct pt_regs orig_regs, regs = {0};
    this->status = HLUWA_STATUS_CALL_ADDR;
    this->getregs(&regs);
    memcpy(&orig_regs, &regs, sizeof(struct pt_regs));
    for (int i = 0; i < ARGS_REG_NUM; i++) {
        if (i < argc) {
            printf("arg[%d]: %p\n", i, args[i]);
            regs.uregs[i] = (unsigned long) args[i];
        }
    }
    if (argc > ARGS_REG_NUM) {
        regs.ARM_sp -= (argc - ARGS_REG_NUM) * sizeof(void *);
        this->write_data((uint8_t *) regs.ARM_sp, (uint8_t *) &args[ARGS_REG_NUM],
                         (argc - ARGS_REG_NUM) * sizeof(void *));
    }
    regs.ARM_lr = 0x11001010;
    regs.ARM_pc = (unsigned long) remote_addr;
    if (regs.ARM_pc & 1) {
        regs.ARM_pc &= (~1u);
        regs.ARM_cpsr |= CPSR_T_MASK;
    } else {
        regs.ARM_cpsr &= ~CPSR_T_MASK;
    }
    this->setregs(&regs);
    this->cont();
    int stat = 0;
    waitpid(this->pid, &stat, WUNTRACED);
    while (stat != 0xb7f && return_regs.ARM_lr != 0x11001010) {
        if (this->cont() == -1) {
            LOGD("call_addr %s", "failure");
            printf("error\n");
            this->status = HLUWA_STATUS_FAILD;
            return 0;
        }
        waitpid(this->pid, &stat, WUNTRACED);
        this->getregs(&return_regs);
        printf("pc = %p\n", (void *) return_regs.ARM_pc);
    }
    this->getregs(&return_regs);
    printf("result = %p\n", (void *) return_regs.uregs[0]);
    this->setregs(&orig_regs);
    this->status = HLUWA_STATUS_SUCCESS;
    LOGD("call_addr %s", "success");
    return (void *) return_regs.uregs[0];
}

void *Inject::loadlibrary(char *libfile) {
    if (!this->dlopen_addr) {
        if (SDK_INT >= ANDROID_N) {
            const char* name_dlopen_ext_N = "__dl__ZL10dlopen_extPKciPK17android_dlextinfoPv";
            const char* name_dlopen_ext_O = "__dl__ZL10dlopen_extPKciPK17android_dlextinfoPKv";
            const char* name_dlopen_ext_P = "__dl___loader_android_dlopen_ext";
            if (SDK_INT >= ANDROID_P) {
                this->dlopen_addr = this->get_remote_addr_spec(LINKER_PATH, name_dlopen_ext_P);
            } else if (SDK_INT >= ANDROID_O) {
                this->dlopen_addr = this->get_remote_addr_spec(LINKER_PATH, name_dlopen_ext_O);
            } else {
                this->dlopen_addr = this->get_remote_addr_spec(LINKER_PATH, name_dlopen_ext_N);
            }
        } else {
            this->dlopen_addr = this->get_remote_addr(LINKER_PATH, (void *) dlopen);
        }
    }

    LOGD("dlopen addr: %lu", this->dlopen_addr);

    void *remote_str = this->write_string(libfile);
    char buf[256] = {0};

    void *handle = nullptr;

    if (SDK_INT >= ANDROID_N) {
        void *args[4] = {
                remote_str,
                (void *) RTLD_NOW,
                0,
                reinterpret_cast<void *>((size_t)get_module_base(pid, ART_PATH) + 0x2000)
        };
        handle = this->call_addr(this->dlopen_addr, args, 4);
    } else {
        void *args[2] = {
                remote_str,
                (void *) (RTLD_NOW | RTLD_LOCAL)
        };
        handle = this->call_addr(this->dlopen_addr, args, 2);
    }

    if (!handle) {
        void *err_str = this->call_addr(this->get_remote_addr(LINKER_PATH, (void *) dlerror), NULL,
                                        0);
        this->read_string((uint8_t *) err_str, (uint8_t *) buf, 256);
        printf("dlerror: %s\n", buf);
        LOGD("load library %s %s", libfile,"error");
        this->status = HLUWA_STATUS_DLOPEN_ERROR;
    } else {
        this->handles[this->handles_cursor++] = handle;
        this->status = HLUWA_STATUS_SUCCESS;
        LOGD("load library %s %s", libfile,"success");
    }
    printf("loadLibrary: %s, to: %p\n", libfile, (void *) handle);
    return handle;
}

void *Inject::get_remote_addr(const char *module_name, void *local_addr) {
    void *local_handle, *remote_handle;

    local_handle = get_module_base(-1, module_name);
    remote_handle = get_module_base(this->pid, module_name);
    LOGD("module_name = %s, local_handle = %p, remote_handle = %p, local_addr = %p\n",
           module_name, local_handle, remote_handle, local_addr);
    void *ret_addr = (void *) ((unsigned long) local_addr + (unsigned long) remote_handle -
                               (unsigned long) local_handle);
    return ret_addr;
}

void *Inject::get_remote_addr(const char *module_name, const char *sym_name) {
    void* local_addr = nullptr;
    void* handle = nullptr;
    if (SDK_INT >= ANDROID_N) {
        handle = fake_dlopen(module_name, RTLD_NOW);
        local_addr = fake_dlsym(handle, sym_name);
        LOGD("fake dlopen sym addr %s: handle %lu sym %lu", sym_name, handle, local_addr);
    } else {
        handle = dlopen(module_name, RTLD_NOW);
        local_addr = dlsym(handle, sym_name);
    }
    return get_remote_addr(module_name, local_addr);
}

void *get_module_base(pid_t pid, const char *module_name) {
    FILE *fp;
    long addr = 0;
    char *pch;
    char filename[32];
    char line[1024];

    if (pid < 0) {
        snprintf(filename, sizeof(filename), "/proc/self/maps");
    } else {
        snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    }
    fp = fopen(filename, "r");
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, module_name)) {
                pch = strtok(line, "-");
                addr = strtoul(pch, NULL, 16);
                break;
            }
        }
        fclose(fp);
    }

    LOGD("get module base %s: %lu", module_name, addr);

    return (void *) addr;
}

int Inject::find_sym_offset_spec(const char* elfpath, const char* sym) {
    FILE *fp = NULL;
    if (!(fp = fopen(elfpath, "rb")))  {
        LOGD("[init_dlopen_ext_offset]Unable to open %s\n", elfpath);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buffer = (char*)malloc(size);
    if (fread(buffer, 1, size, fp) != size) {
        LOGD("fread error\n");
        return -1;
    }
    fclose(fp);

    unsigned long symstr_off = 0, symtab_off = 0, symtab_size = 0;
    unsigned long symtab_entsize = 0, symtab_count = 0;
    const elf_header_t* eh  = (elf_header_t*)buffer;
    const elf_sheader_t* esh = (elf_sheader_t*)(buffer + eh->shoff);
    char* section_str = esh[eh->shstrndx].sh_offset + buffer;

    for (int i = 0; i < eh->shnum; i++) {
        char* sname = esh[i].sh_name + section_str;
        if (strcmp(sname, ".symtab") == 0) {
            symtab_off = esh[i].sh_offset;
            symtab_size = esh[i].sh_size;
            symtab_entsize = esh[i].sh_entsize;
            symtab_count = symtab_size / symtab_entsize;
            LOGD("[init_dlopen_ext_offset]: symtab offset = %lx, count=%lx, index= %d\n",
                        symtab_off, symtab_count, i);
        }
        if (strcmp(sname, ".strtab") == 0) {
            symstr_off = esh[i].sh_offset;
            LOGD("[init_dlopen_ext_offset] symstr offset = %lx, index = %d\n", symstr_off, i);
        }

    }

    if(!symtab_off) {
        LOGD("[init_dlopen_ext_offset] can't find symtab from sections\n");
    }

    elf_sym_t* edt = (elf_sym_t*)(buffer + symtab_off);

    int _offset = -1;

    for(int i = 0 ; i < symtab_count; i++) {
        uint8_t st_type = ELF32_ST_TYPE(edt[i].info);
        char* st_name = buffer + symstr_off + edt[i].name;
        // DEBUG_PRINT("[init_dlopen_ext_offset] walk sym name:%s, value:%x\n", st_name, edt[i].value);
        if (st_type == STT_FUNC && edt[i].size) {
            if(strcmp(st_name, sym) == 0) {
                _offset = edt[i].value;
                LOGD("[init_dlopen_ext_offset] find %s: %x\n", elfpath ,_offset);
                break;
            }
        }
    }
    free(buffer);

    return _offset;
}

void *Inject::get_remote_addr_spec(const char *elfpath, const char* sym) {
    int offset = find_sym_offset_spec(elfpath, sym);
    size_t addr = reinterpret_cast<size_t>(get_module_base(pid, elfpath));
    if (offset >= 0) {
        return reinterpret_cast<void *>(addr + offset);
    } else {
        return nullptr;
    }
}