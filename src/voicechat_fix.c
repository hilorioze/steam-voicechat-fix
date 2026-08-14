#define _GNU_SOURCE  // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

#include <dlfcn.h>
#include <elf.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int AddressInImage(const struct dl_phdr_info* info, uintptr_t address, size_t size) {
  for (Elf32_Half i = 0; i < info->dlpi_phnum; ++i) {
    const Elf32_Phdr* phdr = &info->dlpi_phdr[i];

    if (phdr->p_type != PT_LOAD)
      continue;

    if (phdr->p_vaddr > UINTPTR_MAX - info->dlpi_addr)
      continue;

    const uintptr_t begin = info->dlpi_addr + phdr->p_vaddr;

    if (phdr->p_memsz > UINTPTR_MAX - begin)
      continue;

    const uintptr_t end = begin + phdr->p_memsz;

    if (address >= begin && address < end && size <= end - address)
      return 1;
  }

  return 0;
}

static uintptr_t DynamicAddress(const struct dl_phdr_info* info, Elf32_Addr value) {
  const uintptr_t raw = (uintptr_t)value;

  if (AddressInImage(info, raw, 1))
    return raw;

  if (raw > UINTPTR_MAX - info->dlpi_addr)
    return 0;

  const uintptr_t rebased = info->dlpi_addr + raw;

  if (AddressInImage(info, rebased, 1))
    return rebased;

  return 0;
}

enum PatchResult {
  PATCH_RESULT_APPLIED,
  PATCH_RESULT_ALREADY_APPLIED,
  PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT,
  PATCH_RESULT_MEMMOVE_RESOLUTION_FAILED,
  PATCH_RESULT_MEMMOVE_RELOCATION_NOT_FOUND,
  PATCH_RESULT_PLT_JUMP_NOT_FOUND,
  PATCH_RESULT_AMBIGUOUS_PLT_JUMP,
  PATCH_RESULT_PLT_JUMP_UNALIGNED,
  PATCH_RESULT_UNEXPECTED_DIRECT_JUMP_TARGET,
  PATCH_RESULT_PAGE_SIZE_FAILED,
  PATCH_RESULT_MEMORY_PROTECTION_FAILED,
  PATCH_RESULT_CODE_CHANGED_DURING_PATCH,
};

static void WriteMessage(const char* message) {
  const ssize_t written = write(STDERR_FILENO, message, strlen(message));

  (void)written;
}

static const char* PatchResultMessage(enum PatchResult result) {
  switch (result) {
    case PATCH_RESULT_APPLIED:
      return "steam-voicechat-fix: patch applied\n";
    case PATCH_RESULT_ALREADY_APPLIED:
      return "steam-voicechat-fix: patch already applied\n";
    case PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT:
      return "steam-voicechat-fix: unsupported steamclient.so ELF layout\n";
    case PATCH_RESULT_MEMMOVE_RESOLUTION_FAILED:
      return "steam-voicechat-fix: failed to resolve memmove\n";
    case PATCH_RESULT_MEMMOVE_RELOCATION_NOT_FOUND:
      return "steam-voicechat-fix: memmove relocation not found\n";
    case PATCH_RESULT_PLT_JUMP_NOT_FOUND:
      return "steam-voicechat-fix: memmove PLT jump not found\n";
    case PATCH_RESULT_AMBIGUOUS_PLT_JUMP:
      return "steam-voicechat-fix: ambiguous memmove PLT jump\n";
    case PATCH_RESULT_PLT_JUMP_UNALIGNED:
      return "steam-voicechat-fix: memmove PLT jump is not 8-byte aligned\n";
    case PATCH_RESULT_UNEXPECTED_DIRECT_JUMP_TARGET:
      return "steam-voicechat-fix: direct memmove PLT jump has an unexpected target\n";
    case PATCH_RESULT_PAGE_SIZE_FAILED:
      return "steam-voicechat-fix: failed to get the system page size\n";
    case PATCH_RESULT_MEMORY_PROTECTION_FAILED:
      return "steam-voicechat-fix: failed to change memory protection\n";
    case PATCH_RESULT_CODE_CHANGED_DURING_PATCH:
      return "steam-voicechat-fix: memmove PLT code changed during patching\n";
  }

  return "steam-voicechat-fix: unknown patch result\n";
}

static enum PatchResult PatchCode(uint8_t* jump, const uint8_t patch[6], int protection) {
  const long pageSize = sysconf(_SC_PAGESIZE);

  if (pageSize <= 0)
    return PATCH_RESULT_PAGE_SIZE_FAILED;

  // include the surrounding bytes in the 8-byte update
  uint8_t* word = jump - 1;

  if ((uintptr_t)word % sizeof(uint64_t) != 0)
    return PATCH_RESULT_PLT_JUMP_UNALIGNED;

  uint64_t expected = 0;
  uint64_t replacement = 0;

  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  memcpy(&expected, word, sizeof(expected));
  replacement = expected;
  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  memcpy((uint8_t*)&replacement + 1, patch, 6);

  const uintptr_t pageStart = (uintptr_t)word - ((uintptr_t)word % (uintptr_t)pageSize);
  const uintptr_t patchEnd = (uintptr_t)word + sizeof(replacement);
  const uintptr_t pageEnd = patchEnd + (uintptr_t)pageSize - 1;
  const uintptr_t writableEnd = pageEnd - (pageEnd % (uintptr_t)pageSize);

  if (mprotect((void*)pageStart, writableEnd - pageStart, protection | PROT_WRITE) != 0)
    return PATCH_RESULT_MEMORY_PROTECTION_FAILED;

  const int patched = __atomic_compare_exchange_n((uint64_t*)word, &expected, replacement, 0,
                                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

  if (mprotect((void*)pageStart, writableEnd - pageStart, protection) != 0)
    return PATCH_RESULT_MEMORY_PROTECTION_FAILED;

  if (memcmp(jump, patch, 6) == 0)
    return patched ? PATCH_RESULT_APPLIED : PATCH_RESULT_ALREADY_APPLIED;

  return PATCH_RESULT_CODE_CHANGED_DURING_PATCH;
}

enum PltJumpState {
  PLT_JUMP_NOT_FOUND,
  PLT_JUMP_EBX_RELATIVE,
  PLT_JUMP_DIRECT,
  PLT_JUMP_AMBIGUOUS,
};

static enum PltJumpState FindPltJump(const struct dl_phdr_info* info, size_t relocation_index,
                                     uint8_t** jump, int* protection) {
  // `.rel.plt` offset stored by `mov ecx, imm32`
  const uint32_t relocation_offset = (uint32_t)(relocation_index * sizeof(Elf32_Rel));
  enum PltJumpState state = PLT_JUMP_NOT_FOUND;

  for (Elf32_Half i = 0; i < info->dlpi_phnum; ++i) {
    const Elf32_Phdr* phdr = &info->dlpi_phdr[i];

    if (phdr->p_type != PT_LOAD || !(phdr->p_flags & PF_X))
      continue;

    if (phdr->p_vaddr > UINTPTR_MAX - info->dlpi_addr)
      continue;

    const uintptr_t begin_address = info->dlpi_addr + phdr->p_vaddr;

    if (phdr->p_memsz > UINTPTR_MAX - begin_address)
      continue;

    uint8_t* begin = (uint8_t*)begin_address;
    const size_t size = phdr->p_memsz;

    if (size < 16)
      continue;

    for (size_t offset = 0; offset <= size - 16; ++offset) {
      uint8_t* stub = begin + offset;

      // `steamclient.so` PLT:
      // `endbr32; mov ecx, relocation_offset; jmp [ebx + disp32]; int3`
      // `f3 0f 1e fb | b9 imm32 | ff a3 disp32 | cc`
      if (stub[0] != 0xF3 || stub[1] != 0x0F || stub[2] != 0x1E || stub[3] != 0xFB ||
          stub[4] != 0xB9 || stub[15] != 0xCC)
        continue;

      uint32_t encoded_relocation_offset = 0;

      // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
      memcpy(&encoded_relocation_offset, stub + 5, sizeof(encoded_relocation_offset));

      if (encoded_relocation_offset != relocation_offset)
        continue;

      enum PltJumpState candidate = PLT_JUMP_NOT_FOUND;

      if (stub[9] == 0xFF && stub[10] == 0xA3)
        candidate = PLT_JUMP_EBX_RELATIVE;
      else if (stub[9] == 0xE9 && stub[14] == 0x90)
        candidate = PLT_JUMP_DIRECT;
      else
        continue;

      if (state != PLT_JUMP_NOT_FOUND)
        return PLT_JUMP_AMBIGUOUS;

      state = candidate;

      *protection = PROT_EXEC;

      if (phdr->p_flags & PF_R)
        *protection |= PROT_READ;

      if (phdr->p_flags & PF_W)
        *protection |= PROT_WRITE;

      *jump = stub + 9;
    }
  }

  return state;
}

static enum PatchResult PatchSteamClient(const struct dl_phdr_info* info) {
  const Elf32_Dyn* dynamic = NULL;
  size_t dynamic_count = 0;

  for (Elf32_Half i = 0; i < info->dlpi_phnum; ++i) {
    const Elf32_Phdr* phdr = &info->dlpi_phdr[i];

    if (phdr->p_type == PT_DYNAMIC) {
      if (phdr->p_vaddr > UINTPTR_MAX - info->dlpi_addr || phdr->p_memsz % sizeof(*dynamic) != 0)
        return PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT;

      const uintptr_t dynamic_address = info->dlpi_addr + phdr->p_vaddr;

      if (!AddressInImage(info, dynamic_address, phdr->p_memsz))
        return PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT;

      dynamic = (const Elf32_Dyn*)dynamic_address;
      dynamic_count = phdr->p_memsz / sizeof(*dynamic);

      break;
    }
  }

  if (!dynamic || !dynamic_count)
    return PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT;

  const Elf32_Rel* jmprel = NULL;
  size_t jmprel_size = 0;
  size_t relocation_entry_size = 0;

  const Elf32_Sym* symtab = NULL;
  const char* strtab = NULL;
  size_t strtab_size = 0;
  size_t symbol_entry_size = 0;

  Elf32_Word plt_relocation_type = DT_NULL;
  int dynamic_terminated = 0;

  for (size_t i = 0; i < dynamic_count; ++i) {
    const Elf32_Dyn* entry = &dynamic[i];

    if (entry->d_tag == DT_NULL) {
      dynamic_terminated = 1;

      break;
    }

    switch (entry->d_tag) {
      case DT_JMPREL:
        jmprel = (const Elf32_Rel*)DynamicAddress(info, entry->d_un.d_ptr);

        break;

      case DT_PLTRELSZ:
        jmprel_size = entry->d_un.d_val;

        break;

      case DT_PLTREL:
        plt_relocation_type = entry->d_un.d_val;

        break;

      case DT_RELENT:
        relocation_entry_size = entry->d_un.d_val;

        break;

      case DT_SYMTAB:
        symtab = (const Elf32_Sym*)DynamicAddress(info, entry->d_un.d_ptr);

        break;

      case DT_SYMENT:
        symbol_entry_size = entry->d_un.d_val;

        break;

      case DT_STRTAB:
        strtab = (const char*)DynamicAddress(info, entry->d_un.d_ptr);

        break;

      case DT_STRSZ:
        strtab_size = entry->d_un.d_val;

        break;

      default:
        break;
    }
  }

  if (!dynamic_terminated || !jmprel || !jmprel_size || !symtab || !strtab || !strtab_size ||
      plt_relocation_type != DT_REL || relocation_entry_size != sizeof(Elf32_Rel) ||
      symbol_entry_size != sizeof(Elf32_Sym) || jmprel_size % relocation_entry_size != 0)
    return PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT;

  if (!AddressInImage(info, (uintptr_t)jmprel, jmprel_size) ||
      !AddressInImage(info, (uintptr_t)strtab, strtab_size))
    return PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT;

  const size_t count = jmprel_size / sizeof(Elf32_Rel);

  for (size_t i = 0; i < count; ++i) {
    const Elf32_Rel* relocation = &jmprel[i];

    if (ELF32_R_TYPE(relocation->r_info) != R_386_JMP_SLOT)
      continue;

    const size_t symbol_index = ELF32_R_SYM(relocation->r_info);

    if (symbol_index > (UINTPTR_MAX - (uintptr_t)symtab) / sizeof(*symtab))
      return PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT;

    const uintptr_t symbol_address = (uintptr_t)symtab + (symbol_index * sizeof(*symtab));

    if (!AddressInImage(info, symbol_address, sizeof(*symtab)))
      return PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT;

    const Elf32_Sym* symbol = (const Elf32_Sym*)symbol_address;
    const size_t name_offset = symbol->st_name;

    if (name_offset >= strtab_size || strtab_size - name_offset < sizeof("memmove") ||
        memcmp(strtab + name_offset, "memmove", sizeof("memmove")) != 0)
      continue;

    void* memmove_target = dlsym(RTLD_DEFAULT, "memmove");

    if (!memmove_target)
      return PATCH_RESULT_MEMMOVE_RESOLUTION_FAILED;

    uint8_t* plt_jump = NULL;
    int protection = 0;
    const enum PltJumpState jump_state = FindPltJump(info, i, &plt_jump, &protection);

    if (jump_state == PLT_JUMP_NOT_FOUND)
      return PATCH_RESULT_PLT_JUMP_NOT_FOUND;

    if (jump_state == PLT_JUMP_AMBIGUOUS)
      return PATCH_RESULT_AMBIGUOUS_PLT_JUMP;

    if (jump_state == PLT_JUMP_DIRECT) {
      int32_t rel32 = 0;

      // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
      memcpy(&rel32, plt_jump + 1, sizeof(rel32));

      const uintptr_t patched_target = (uintptr_t)plt_jump + 5 + (uintptr_t)(uint32_t)rel32;

      return patched_target == (uintptr_t)memmove_target
                 ? PATCH_RESULT_ALREADY_APPLIED
                 : PATCH_RESULT_UNEXPECTED_DIRECT_JUMP_TARGET;
    }

    // `jmp [ebx + disp32]` -> `jmp rel32; nop`
    uint8_t patch[6] = {0xE9, 0, 0, 0, 0, 0x90};

    // `rel32` is relative to the next instruction
    const uint32_t rel32 = (uint32_t)((uintptr_t)memmove_target - ((uintptr_t)plt_jump + 5));

    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memcpy(patch + 1, &rel32, sizeof(rel32));

    return PatchCode(plt_jump, patch, protection);
  }

  return PATCH_RESULT_MEMMOVE_RELOCATION_NOT_FOUND;
}

static int PatchSteamClientCallback(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;

  if (!info->dlpi_name)
    return 0;

  const char* name = strrchr(info->dlpi_name, '/');
  name = name ? name + 1 : info->dlpi_name;

  if (strcmp(name, "steamclient.so") != 0)
    return 0;

  enum PatchResult* result = data;
  *result = PatchSteamClient(info);

  return 1;
}

static void* PatchThread(void* data) {
  (void)data;

  WriteMessage("steam-voicechat-fix: waiting for steamclient.so\n");

  for (;;) {
    enum PatchResult result = PATCH_RESULT_UNSUPPORTED_ELF_LAYOUT;

    if (dl_iterate_phdr(PatchSteamClientCallback, &result)) {
      WriteMessage(PatchResultMessage(result));

      return NULL;
    }

    const struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 100000000,  // poll for `steamclient.so` every 100 ms
    };

    nanosleep(&delay, NULL);
  }
}

static int IsSteamProcess(void) {
  char executable[PATH_MAX];
  const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);

  if (length < 0)
    return 0;

  executable[length] = '\0';

  const char* name = strrchr(executable, '/');

  return name && strcmp(name + 1, "steam") == 0;
}

__attribute__((constructor)) static void StartPatchThread(void) {
  if (!IsSteamProcess())
    return;

  pthread_t thread = 0;

  if (pthread_create(&thread, NULL, PatchThread, NULL) != 0) {
    WriteMessage("steam-voicechat-fix: failed to start patch thread\n");

    return;
  }

  pthread_detach(thread);
}
