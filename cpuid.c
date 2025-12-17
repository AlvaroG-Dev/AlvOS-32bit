#include "cpuid.h"
#include "kernel.h"
#include "string.h"
#include "terminal.h"

// Global CPU information
cpu_info_t cpu_info = {0};

// ========================================================================
// LOW-LEVEL CPUID INSTRUCTION
// ========================================================================

bool cpuid_is_supported(void) {
  uint32_t eflags_before, eflags_after;

  // Try to flip ID bit (bit 21) in EFLAGS
  __asm__ volatile("pushfl\n\t"                  // Save original EFLAGS
                   "pushfl\n\t"                  // Save again for modification
                   "xorl $0x200000, (%%esp)\n\t" // Flip ID bit
                   "popfl\n\t"                   // Load modified flags
                   "pushfl\n\t"                  // Save modified flags
                   "popl %0\n\t"                 // Get modified flags
                   "popfl\n\t"                   // Restore original flags
                   : "=r"(eflags_after));

  __asm__ volatile("pushfl\n\t"
                   "popl %0\n\t"
                   : "=r"(eflags_before));

  // If ID bit can be changed, CPUID is supported
  return ((eflags_before ^ eflags_after) & 0x200000) != 0;
}

void cpuid(uint32_t function, uint32_t *eax, uint32_t *ebx, uint32_t *ecx,
           uint32_t *edx) {
  __asm__ volatile("pushl %%ebx\n\t" // Save EBX (required for PIC)
                   "cpuid\n\t"
                   "movl %%ebx, %%esi\n\t"
                   "popl %%ebx\n\t" // Restore EBX
                   : "=a"(*eax), "=S"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "a"(function), "c"(0));
}

void cpuid_ex(uint32_t function, uint32_t subfunc, uint32_t *eax, uint32_t *ebx,
              uint32_t *ecx, uint32_t *edx) {
  __asm__ volatile("pushl %%ebx\n\t"
                   "cpuid\n\t"
                   "movl %%ebx, %%esi\n\t"
                   "popl %%ebx\n\t"
                   : "=a"(*eax), "=S"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "a"(function), "c"(subfunc));
}

// ========================================================================
// CPU INFORMATION GATHERING
// ========================================================================

static void cpuid_get_vendor(void) {
  uint32_t eax, ebx, ecx, edx;

  cpuid(CPUID_GETVENDORSTRING, &eax, &ebx, &ecx, &edx);

  cpu_info.max_basic_cpuid = eax;

  // Vendor string is in EBX, EDX, ECX order
  memcpy(cpu_info.vendor + 0, &ebx, 4);
  memcpy(cpu_info.vendor + 4, &edx, 4);
  memcpy(cpu_info.vendor + 8, &ecx, 4);
  cpu_info.vendor[12] = '\0';
}

static void cpuid_get_features(void) {
  uint32_t eax, ebx, ecx, edx;

  if (cpu_info.max_basic_cpuid < CPUID_GETFEATURES) {
    return;
  }

  cpuid(CPUID_GETFEATURES, &eax, &ebx, &ecx, &edx);

  // Parse version information
  cpu_info.stepping = eax & 0xF;
  cpu_info.model = (eax >> 4) & 0xF;
  cpu_info.family = (eax >> 8) & 0xF;
  cpu_info.type = (eax >> 12) & 0x3;

  // Extended model and family
  uint32_t extended_model = (eax >> 16) & 0xF;
  uint32_t extended_family = (eax >> 20) & 0xFF;

  if (cpu_info.family == 0xF) {
    cpu_info.family += extended_family;
  }

  if (cpu_info.family == 0x6 || cpu_info.family == 0xF) {
    cpu_info.model += (extended_model << 4);
  }

  // Store feature flags
  cpu_info.features_ecx = ecx;
  cpu_info.features_edx = edx;

  // APIC info
  cpu_info.apic_id = (ebx >> 24) & 0xFF;
  cpu_info.logical_processors = (ebx >> 16) & 0xFF;
  cpu_info.cache_line_size = ((ebx >> 8) & 0xFF) * 8;
}

static void cpuid_get_extended_features(void) {
  uint32_t eax, ebx, ecx, edx;

  if (cpu_info.max_basic_cpuid < CPUID_GETEXTENDEDFEATURES) {
    return;
  }

  cpuid_ex(CPUID_GETEXTENDEDFEATURES, 0, &eax, &ebx, &ecx, &edx);

  cpu_info.extended_features_ebx = ebx;
  cpu_info.extended_features_ecx = ecx;
  cpu_info.extended_features_edx = edx;
}

static void cpuid_get_extended_info(void) {
  uint32_t eax, ebx, ecx, edx;

  // Check if extended functions are supported
  cpuid(CPUID_INTELEXTENDED, &eax, &ebx, &ecx, &edx);
  cpu_info.max_extended_cpuid = eax;

  if (cpu_info.max_extended_cpuid < CPUID_INTELFEATURES) {
    return;
  }

  // Get extended feature flags
  cpuid(CPUID_INTELFEATURES, &eax, &ebx, &ecx, &edx);
  cpu_info.ext_features_ecx = ecx;
  cpu_info.ext_features_edx = edx;
}

static void cpuid_get_brand_string(void) {
  uint32_t brand_data[12];

  if (cpu_info.max_extended_cpuid < CPUID_INTELBRANDSTRINGEND) {
    strcpy(cpu_info.brand, "Unknown CPU");
    return;
  }

  // Brand string is in 3 consecutive CPUID calls
  cpuid(CPUID_INTELBRANDSTRING, &brand_data[0], &brand_data[1], &brand_data[2],
        &brand_data[3]);
  cpuid(CPUID_INTELBRANDSTRINGMORE, &brand_data[4], &brand_data[5],
        &brand_data[6], &brand_data[7]);
  cpuid(CPUID_INTELBRANDSTRINGEND, &brand_data[8], &brand_data[9],
        &brand_data[10], &brand_data[11]);

  memcpy(cpu_info.brand, brand_data, 48);
  cpu_info.brand[48] = '\0';

  // Trim leading spaces
  char *p = cpu_info.brand;
  while (*p == ' ')
    p++;
  if (p != cpu_info.brand) {
    memmove(cpu_info.brand, p, strlen(p) + 1);
  }
}

static void cpuid_detect_capabilities(void) {
  // Basic features (EDX)
  cpu_info.caps.has_fpu = (cpu_info.features_edx & CPUID_FEAT_EDX_FPU) != 0;
  cpu_info.caps.has_tsc = (cpu_info.features_edx & CPUID_FEAT_EDX_TSC) != 0;
  cpu_info.caps.has_msr = (cpu_info.features_edx & CPUID_FEAT_EDX_MSR) != 0;
  cpu_info.caps.has_pae = (cpu_info.features_edx & CPUID_FEAT_EDX_PAE) != 0;
  cpu_info.caps.has_cx8 = (cpu_info.features_edx & CPUID_FEAT_EDX_CX8) != 0;
  cpu_info.caps.has_apic = (cpu_info.features_edx & CPUID_FEAT_EDX_APIC) != 0;
  cpu_info.caps.has_sep = (cpu_info.features_edx & CPUID_FEAT_EDX_SEP) != 0;
  cpu_info.caps.has_mtrr = (cpu_info.features_edx & CPUID_FEAT_EDX_MTRR) != 0;
  cpu_info.caps.has_pge = (cpu_info.features_edx & CPUID_FEAT_EDX_PGE) != 0;
  cpu_info.caps.has_cmov = (cpu_info.features_edx & CPUID_FEAT_EDX_CMOV) != 0;
  cpu_info.caps.has_pat = (cpu_info.features_edx & CPUID_FEAT_EDX_PAT) != 0;
  cpu_info.caps.has_pse36 = (cpu_info.features_edx & CPUID_FEAT_EDX_PSE36) != 0;
  cpu_info.caps.has_pse = (cpu_info.features_edx & CPUID_FEAT_EDX_PSE) != 0;
  cpu_info.caps.has_clflush = (cpu_info.features_edx & CPUID_FEAT_EDX_CLF) != 0;
  cpu_info.caps.has_acpi = (cpu_info.features_edx & CPUID_FEAT_EDX_ACPI) != 0;
  cpu_info.caps.has_mmx = (cpu_info.features_edx & CPUID_FEAT_EDX_MMX) != 0;
  cpu_info.caps.has_fxsr = (cpu_info.features_edx & CPUID_FEAT_EDX_FXSR) != 0;
  cpu_info.caps.has_sse = (cpu_info.features_edx & CPUID_FEAT_EDX_SSE) != 0;
  cpu_info.caps.has_sse2 = (cpu_info.features_edx & CPUID_FEAT_EDX_SSE2) != 0;
  cpu_info.caps.has_htt = (cpu_info.features_edx & CPUID_FEAT_EDX_HTT) != 0;

  // Extended features (ECX)
  cpu_info.caps.has_sse3 = (cpu_info.features_ecx & CPUID_FEAT_ECX_SSE3) != 0;
  cpu_info.caps.has_ssse3 = (cpu_info.features_ecx & CPUID_FEAT_ECX_SSSE3) != 0;
  cpu_info.caps.has_sse4_1 =
      (cpu_info.features_ecx & CPUID_FEAT_ECX_SSE4_1) != 0;
  cpu_info.caps.has_sse4_2 =
      (cpu_info.features_ecx & CPUID_FEAT_ECX_SSE4_2) != 0;
  cpu_info.caps.has_x2apic =
      (cpu_info.features_ecx & CPUID_FEAT_ECX_x2APIC) != 0;
  cpu_info.caps.has_popcnt =
      (cpu_info.features_ecx & CPUID_FEAT_ECX_POPCNT) != 0;
  cpu_info.caps.has_aes = (cpu_info.features_ecx & CPUID_FEAT_ECX_AES) != 0;
  cpu_info.caps.has_xsave = (cpu_info.features_ecx & CPUID_FEAT_ECX_XSAVE) != 0;
  cpu_info.caps.has_osxsave =
      (cpu_info.features_ecx & CPUID_FEAT_ECX_OSXSAVE) != 0;
  cpu_info.caps.has_avx = (cpu_info.features_ecx & CPUID_FEAT_ECX_AVX) != 0;
  cpu_info.caps.has_rdrand =
      (cpu_info.features_ecx & CPUID_FEAT_ECX_RDRAND) != 0;
  cpu_info.caps.has_vmx = (cpu_info.features_ecx & CPUID_FEAT_ECX_VMX) != 0;

  // Extended features from function 0x07
  cpu_info.caps.has_avx2 =
      (cpu_info.extended_features_ebx & CPUID_FEAT_EXT_AVX2) != 0;
  cpu_info.caps.has_rdseed =
      (cpu_info.extended_features_ebx & CPUID_FEAT_EXT_RDSEED) != 0;
  cpu_info.caps.has_smep =
      (cpu_info.extended_features_ebx & CPUID_FEAT_EXT_SMEP) != 0;
  cpu_info.caps.has_smap =
      (cpu_info.extended_features_ebx & CPUID_FEAT_EXT_SMAP) != 0;

  // Extended features from function 0x80000001
  cpu_info.caps.has_syscall =
      (cpu_info.ext_features_edx & CPUID_FEAT_EXT_SYSCALL) != 0;
  cpu_info.caps.has_nx = (cpu_info.ext_features_edx & CPUID_FEAT_EXT_XD) != 0;
  cpu_info.caps.has_1gb_pages =
      (cpu_info.ext_features_edx & CPUID_FEAT_EXT_1GB_PAGE) != 0;
  cpu_info.caps.has_rdtscp =
      (cpu_info.ext_features_edx & CPUID_FEAT_EXT_RDTSCP) != 0;
  cpu_info.caps.has_long_mode =
      (cpu_info.ext_features_edx & CPUID_FEAT_EXT_64BIT) != 0;
  cpu_info.caps.has_lahf_lm =
      (cpu_info.ext_features_ecx & CPUID_FEAT_EXT_LAHF_LM) != 0;
  cpu_info.caps.has_svm = (cpu_info.ext_features_ecx & CPUID_FEAT_EXT_SVM) != 0;
}

// ========================================================================
// PUBLIC API
// ========================================================================

void cpuid_init(void) {
  memset(&cpu_info, 0, sizeof(cpu_info_t));

  terminal_puts(&main_terminal, "Detecting CPU features...\r\n");

  // Check if CPUID is supported
  if (!cpuid_is_supported()) {
    terminal_puts(&main_terminal, "CPUID: Not supported on this CPU!\r\n");
    strcpy(cpu_info.vendor, "Unknown");
    strcpy(cpu_info.brand, "Pre-Pentium CPU");
    return;
  }

  // Get vendor string
  cpuid_get_vendor();
  terminal_printf(&main_terminal, "CPUID: Vendor: %s\r\n", cpu_info.vendor);

  // Get features
  cpuid_get_features();

  // Get extended features
  cpuid_get_extended_features();

  // Get extended info (including 64-bit support)
  cpuid_get_extended_info();

  // Get brand string
  cpuid_get_brand_string();
  terminal_printf(&main_terminal, "CPUID: CPU: %s\r\n", cpu_info.brand);

  // Detect capabilities
  cpuid_detect_capabilities();

  terminal_printf(&main_terminal, "CPUID: Family=%u, Model=%u, Stepping=%u\r\n",
                  cpu_info.family, cpu_info.model, cpu_info.stepping);

  terminal_puts(&main_terminal, "CPUID: Detection complete\r\n");
}

bool cpu_has_feature(uint32_t feature_bit, uint32_t register_id) {
  switch (register_id) {
  case CPU_REG_ECX:
    return (cpu_info.features_ecx & feature_bit) != 0;
  case CPU_REG_EDX:
    return (cpu_info.features_edx & feature_bit) != 0;
  case CPU_REG_EXT_EBX:
    return (cpu_info.extended_features_ebx & feature_bit) != 0;
  case CPU_REG_EXT_ECX:
    return (cpu_info.extended_features_ecx & feature_bit) != 0;
  case CPU_REG_EXT_EDX:
    return (cpu_info.extended_features_edx & feature_bit) != 0;
  case CPU_REG_EXT81_ECX:
    return (cpu_info.ext_features_ecx & feature_bit) != 0;
  case CPU_REG_EXT81_EDX:
    return (cpu_info.ext_features_edx & feature_bit) != 0;
  default:
    return false;
  }
}

const char *cpu_get_vendor(void) { return cpu_info.vendor; }

const char *cpu_get_brand(void) { return cpu_info.brand; }

// ========================================================================
// PRINTING FUNCTIONS
// ========================================================================

void cpuid_print_info(void) {
  terminal_puts(&main_terminal, "\r\n=== CPU Information ===\r\n");
  terminal_printf(&main_terminal, "Vendor: %s\r\n", cpu_info.vendor);
  terminal_printf(&main_terminal, "Brand: %s\r\n", cpu_info.brand);
  terminal_printf(&main_terminal, "Family: %u, Model: %u, Stepping: %u\r\n",
                  cpu_info.family, cpu_info.model, cpu_info.stepping);

  if (cpu_info.caps.has_htt) {
    terminal_printf(&main_terminal, "Logical processors: %u\r\n",
                    cpu_info.logical_processors);
  }

  terminal_printf(&main_terminal, "Cache line size: %u bytes\r\n",
                  cpu_info.cache_line_size);

  terminal_puts(&main_terminal, "\r\n");
}

void cpuid_print_features(void) {
  terminal_puts(&main_terminal, "\r\n=== CPU Features ===\r\n");

  // Basic features
  terminal_puts(&main_terminal, "Basic: ");
  if (cpu_info.caps.has_fpu)
    terminal_puts(&main_terminal, "FPU ");
  if (cpu_info.caps.has_tsc)
    terminal_puts(&main_terminal, "TSC ");
  if (cpu_info.caps.has_msr)
    terminal_puts(&main_terminal, "MSR ");
  if (cpu_info.caps.has_cx8)
    terminal_puts(&main_terminal, "CX8 ");
  if (cpu_info.caps.has_sep)
    terminal_puts(&main_terminal, "SEP ");
  if (cpu_info.caps.has_cmov)
    terminal_puts(&main_terminal, "CMOV ");
  if (cpu_info.caps.has_clflush)
    terminal_puts(&main_terminal, "CLFLUSH ");
  terminal_puts(&main_terminal, "\r\n");

  // SIMD features
  terminal_puts(&main_terminal, "SIMD: ");
  if (cpu_info.caps.has_mmx)
    terminal_puts(&main_terminal, "MMX ");
  if (cpu_info.caps.has_sse)
    terminal_puts(&main_terminal, "SSE ");
  if (cpu_info.caps.has_sse2)
    terminal_puts(&main_terminal, "SSE2 ");
  if (cpu_info.caps.has_sse3)
    terminal_puts(&main_terminal, "SSE3 ");
  if (cpu_info.caps.has_ssse3)
    terminal_puts(&main_terminal, "SSSE3 ");
  if (cpu_info.caps.has_sse4_1)
    terminal_puts(&main_terminal, "SSE4.1 ");
  if (cpu_info.caps.has_sse4_2)
    terminal_puts(&main_terminal, "SSE4.2 ");
  if (cpu_info.caps.has_avx)
    terminal_puts(&main_terminal, "AVX ");
  if (cpu_info.caps.has_avx2)
    terminal_puts(&main_terminal, "AVX2 ");
  terminal_puts(&main_terminal, "\r\n");

  // Memory features
  terminal_puts(&main_terminal, "Memory: ");
  if (cpu_info.caps.has_pae)
    terminal_puts(&main_terminal, "PAE ");
  if (cpu_info.caps.has_pse)
    terminal_puts(&main_terminal, "PSE ");
  if (cpu_info.caps.has_pse36)
    terminal_puts(&main_terminal, "PSE-36 ");
  if (cpu_info.caps.has_pge)
    terminal_puts(&main_terminal, "PGE ");
  if (cpu_info.caps.has_pat)
    terminal_puts(&main_terminal, "PAT ");
  if (cpu_info.caps.has_mtrr)
    terminal_puts(&main_terminal, "MTRR ");
  if (cpu_info.caps.has_nx)
    terminal_puts(&main_terminal, "NX ");
  if (cpu_info.caps.has_1gb_pages)
    terminal_puts(&main_terminal, "1GB-Pages ");
  terminal_puts(&main_terminal, "\r\n");

  // Security features
  terminal_puts(&main_terminal, "Security: ");
  if (cpu_info.caps.has_smep)
    terminal_puts(&main_terminal, "SMEP ");
  if (cpu_info.caps.has_smap)
    terminal_puts(&main_terminal, "SMAP ");
  terminal_puts(&main_terminal, "\r\n");

  // Virtualization
  terminal_puts(&main_terminal, "Virtualization: ");
  if (cpu_info.caps.has_vmx)
    terminal_puts(&main_terminal, "VT-x ");
  if (cpu_info.caps.has_svm)
    terminal_puts(&main_terminal, "AMD-V ");
  terminal_puts(&main_terminal, "\r\n");

  // Other features
  terminal_puts(&main_terminal, "Other: ");
  if (cpu_info.caps.has_apic)
    terminal_puts(&main_terminal, "APIC ");
  if (cpu_info.caps.has_x2apic)
    terminal_puts(&main_terminal, "x2APIC ");
  if (cpu_info.caps.has_acpi)
    terminal_puts(&main_terminal, "ACPI ");
  if (cpu_info.caps.has_htt)
    terminal_puts(&main_terminal, "HTT ");
  if (cpu_info.caps.has_syscall)
    terminal_puts(&main_terminal, "SYSCALL ");
  if (cpu_info.caps.has_rdtscp)
    terminal_puts(&main_terminal, "RDTSCP ");
  if (cpu_info.caps.has_rdrand)
    terminal_puts(&main_terminal, "RDRAND ");
  if (cpu_info.caps.has_rdseed)
    terminal_puts(&main_terminal, "RDSEED ");
  if (cpu_info.caps.has_popcnt)
    terminal_puts(&main_terminal, "POPCNT ");
  if (cpu_info.caps.has_aes)
    terminal_puts(&main_terminal, "AES-NI ");
  if (cpu_info.caps.has_long_mode)
    terminal_puts(&main_terminal, "x86-64 ");
  terminal_puts(&main_terminal, "\r\n");

  terminal_puts(&main_terminal, "\r\n");
}

// ========================================================================
// COMMAND FUNCTION FOR TERMINAL
// ========================================================================

void cmd_cpuinfo(Terminal *term, const char *args) {
  (void)args; // No usamos argumentos

  if (!term) {
    return;
  }

  // Primero verificar si se ha inicializado CPUID
  if (cpu_info.max_basic_cpuid == 0) {
    terminal_puts(
        term, "CPU information not initialized. Run cpuid_init() first.\r\n");
    return;
  }

  // Mostrar información completa del CPU
  terminal_puts(term, "\r\n");
  terminal_puts(term, "              CPU INFORMATION                 \r\n");
  terminal_puts(term, "\r\n");

  // Información básica
  terminal_printf(term, "Vendor:          %s\r\n", cpu_info.vendor);
  terminal_printf(term, "Brand String:    %s\r\n", cpu_info.brand);
  terminal_printf(term, "Family:          %u (0x%X)\r\n", cpu_info.family,
                  cpu_info.family);
  terminal_printf(term, "Model:           %u (0x%X)\r\n", cpu_info.model,
                  cpu_info.model);
  terminal_printf(term, "Stepping:        %u (0x%X)\r\n", cpu_info.stepping,
                  cpu_info.stepping);
  terminal_printf(term, "Type:            %u\r\n", cpu_info.type);

  // Información de procesadores
  if (cpu_info.logical_processors > 0) {
    terminal_printf(term, "Logical CPUs:    %u\r\n",
                    cpu_info.logical_processors);
  }
  if (cpu_info.apic_id != 0) {
    terminal_printf(term, "APIC ID:         %u\r\n", cpu_info.apic_id);
  }
  if (cpu_info.cache_line_size > 0) {
    terminal_printf(term, "Cache Line:      %u bytes\r\n",
                    cpu_info.cache_line_size);
  }

  // Versiones CPUID
  terminal_printf(term, "Max Basic CPUID: 0x%08X\r\n",
                  cpu_info.max_basic_cpuid);
  terminal_printf(term, "Max Extended:    0x%08X\r\n",
                  cpu_info.max_extended_cpuid);

  terminal_puts(term, "\r\n");
  terminal_puts(term, "               CPU FEATURES                   \r\n");
  terminal_puts(term, "\r\n");

  // Características principales
  terminal_puts(term, "Architecture Features:\r\n");
  terminal_printf(term, "  x86-64 (Long Mode):     %s\r\n",
                  cpu_info.caps.has_long_mode ? "Yes" : "No");
  terminal_printf(term, "  NX (Execute Disable):   %s\r\n",
                  cpu_info.caps.has_nx ? "Yes" : "No");
  terminal_printf(term, "  SMEP:                   %s\r\n",
                  cpu_info.caps.has_smep ? "Yes" : "No");
  terminal_printf(term, "  SMAP:                   %s\r\n",
                  cpu_info.caps.has_smap ? "Yes" : "No");
  terminal_printf(term, "  PAE:                    %s\r\n",
                  cpu_info.caps.has_pae ? "Yes" : "No");

  terminal_puts(term, "\r\nVirtualization:\r\n");
  terminal_printf(term, "  VT-x (Intel):           %s\r\n",
                  cpu_info.caps.has_vmx ? "Yes" : "No");
  terminal_printf(term, "  AMD-V (SVM):            %s\r\n",
                  cpu_info.caps.has_svm ? "Yes" : "No");

  terminal_puts(term, "\r\nSIMD Extensions:\r\n");
  terminal_printf(term, "  SSE:                    %s\r\n",
                  cpu_info.caps.has_sse ? "Yes" : "No");
  terminal_printf(term, "  SSE2:                   %s\r\n",
                  cpu_info.caps.has_sse2 ? "Yes" : "No");
  terminal_printf(term, "  SSE3:                   %s\r\n",
                  cpu_info.caps.has_sse3 ? "Yes" : "No");
  terminal_printf(term, "  SSE4.1:                 %s\r\n",
                  cpu_info.caps.has_sse4_1 ? "Yes" : "No");
  terminal_printf(term, "  SSE4.2:                 %s\r\n",
                  cpu_info.caps.has_sse4_2 ? "Yes" : "No");
  terminal_printf(term, "  AVX:                    %s\r\n",
                  cpu_info.caps.has_avx ? "Yes" : "No");
  terminal_printf(term, "  AVX2:                   %s\r\n",
                  cpu_info.caps.has_avx2 ? "Yes" : "No");

  terminal_puts(term, "\r\nOther Features:\r\n");
  terminal_printf(term, "  RDRAND:                 %s\r\n",
                  cpu_info.caps.has_rdrand ? "Yes" : "No");
  terminal_printf(term, "  RDSEED:                 %s\r\n",
                  cpu_info.caps.has_rdseed ? "Yes" : "No");
  terminal_printf(term, "  AES-NI:                 %s\r\n",
                  cpu_info.caps.has_aes ? "Yes" : "No");
  terminal_printf(term, "  RDTSCP:                 %s\r\n",
                  cpu_info.caps.has_rdtscp ? "Yes" : "No");
  terminal_printf(term, "  Hyper-Threading:        %s\r\n",
                  cpu_info.caps.has_htt ? "Yes" : "No");
  terminal_printf(term, "  x2APIC:                 %s\r\n",
                  cpu_info.caps.has_x2apic ? "Yes" : "No");

  terminal_puts(term, "\r\n");
  terminal_puts(term, "               CPU FEATURE BITS               \r\n");
  terminal_puts(term, "\r\n");

  // Mostrar bits de características
  terminal_printf(term, "CPUID 0x01 EDX: 0x%08X\r\n", cpu_info.features_edx);
  terminal_printf(term, "CPUID 0x01 ECX: 0x%08X\r\n", cpu_info.features_ecx);

  if (cpu_info.extended_features_ebx != 0) {
    terminal_printf(term, "CPUID 0x07 EBX: 0x%08X\r\n",
                    cpu_info.extended_features_ebx);
  }

  if (cpu_info.ext_features_edx != 0) {
    terminal_printf(term, "CPUID 0x80000001 EDX: 0x%08X\r\n",
                    cpu_info.ext_features_edx);
  }

  terminal_puts(term, "\r\n");
  terminal_puts(term, "Type 'cpuinfo detailed' for full feature list.\r\n");
}

// Versión detallada de cpuinfo
void cmd_cpuinfo_detailed(Terminal *term, const char *args) {
  (void)args;

  if (!term) {
    return;
  }

  // Primero mostrar la información básica
  cmd_cpuinfo(term, "");

  terminal_puts(term, "\r\n");
  terminal_puts(term, "             FULL FEATURE LIST                \r\n");
  terminal_puts(term, "\r\n");

  // Llamar a la función existente que imprime todas las características
  cpuid_print_features();
}