#ifndef CPUID_H
#define CPUID_H

#include "terminal.h"
#include <stdbool.h>
#include <stdint.h>


// ========================================================================
// CPUID FUNCTION NUMBERS
// ========================================================================

#define CPUID_GETVENDORSTRING 0x00
#define CPUID_GETFEATURES 0x01
#define CPUID_GETCACHE 0x02
#define CPUID_GETSERIAL 0x03
#define CPUID_GETTLB 0x04
#define CPUID_GETEXTENDEDFEATURES 0x07
#define CPUID_INTELEXTENDED 0x80000000
#define CPUID_INTELFEATURES 0x80000001
#define CPUID_INTELBRANDSTRING 0x80000002
#define CPUID_INTELBRANDSTRINGMORE 0x80000003
#define CPUID_INTELBRANDSTRINGEND 0x80000004

// ========================================================================
// FEATURE FLAGS - ECX (Function 0x01)
// ========================================================================

#define CPUID_FEAT_ECX_SSE3 (1 << 0)
#define CPUID_FEAT_ECX_PCLMUL (1 << 1)
#define CPUID_FEAT_ECX_DTES64 (1 << 2)
#define CPUID_FEAT_ECX_MONITOR (1 << 3)
#define CPUID_FEAT_ECX_DS_CPL (1 << 4)
#define CPUID_FEAT_ECX_VMX (1 << 5)
#define CPUID_FEAT_ECX_SMX (1 << 6)
#define CPUID_FEAT_ECX_EST (1 << 7)
#define CPUID_FEAT_ECX_TM2 (1 << 8)
#define CPUID_FEAT_ECX_SSSE3 (1 << 9)
#define CPUID_FEAT_ECX_CID (1 << 10)
#define CPUID_FEAT_ECX_FMA (1 << 12)
#define CPUID_FEAT_ECX_CX16 (1 << 13)
#define CPUID_FEAT_ECX_ETPRD (1 << 14)
#define CPUID_FEAT_ECX_PDCM (1 << 15)
#define CPUID_FEAT_ECX_PCIDE (1 << 17)
#define CPUID_FEAT_ECX_DCA (1 << 18)
#define CPUID_FEAT_ECX_SSE4_1 (1 << 19)
#define CPUID_FEAT_ECX_SSE4_2 (1 << 20)
#define CPUID_FEAT_ECX_x2APIC (1 << 21)
#define CPUID_FEAT_ECX_MOVBE (1 << 22)
#define CPUID_FEAT_ECX_POPCNT (1 << 23)
#define CPUID_FEAT_ECX_AES (1 << 25)
#define CPUID_FEAT_ECX_XSAVE (1 << 26)
#define CPUID_FEAT_ECX_OSXSAVE (1 << 27)
#define CPUID_FEAT_ECX_AVX (1 << 28)
#define CPUID_FEAT_ECX_F16C (1 << 29)
#define CPUID_FEAT_ECX_RDRAND (1 << 30)

// ========================================================================
// FEATURE FLAGS - EDX (Function 0x01)
// ========================================================================

#define CPUID_FEAT_EDX_FPU (1 << 0)
#define CPUID_FEAT_EDX_VME (1 << 1)
#define CPUID_FEAT_EDX_DE (1 << 2)
#define CPUID_FEAT_EDX_PSE (1 << 3)
#define CPUID_FEAT_EDX_TSC (1 << 4)
#define CPUID_FEAT_EDX_MSR (1 << 5)
#define CPUID_FEAT_EDX_PAE (1 << 6)
#define CPUID_FEAT_EDX_MCE (1 << 7)
#define CPUID_FEAT_EDX_CX8 (1 << 8)
#define CPUID_FEAT_EDX_APIC (1 << 9)
#define CPUID_FEAT_EDX_SEP (1 << 11)
#define CPUID_FEAT_EDX_MTRR (1 << 12)
#define CPUID_FEAT_EDX_PGE (1 << 13)
#define CPUID_FEAT_EDX_MCA (1 << 14)
#define CPUID_FEAT_EDX_CMOV (1 << 15)
#define CPUID_FEAT_EDX_PAT (1 << 16)
#define CPUID_FEAT_EDX_PSE36 (1 << 17)
#define CPUID_FEAT_EDX_PSN (1 << 18)
#define CPUID_FEAT_EDX_CLF (1 << 19)
#define CPUID_FEAT_EDX_DTES (1 << 21)
#define CPUID_FEAT_EDX_ACPI (1 << 22)
#define CPUID_FEAT_EDX_MMX (1 << 23)
#define CPUID_FEAT_EDX_FXSR (1 << 24)
#define CPUID_FEAT_EDX_SSE (1 << 25)
#define CPUID_FEAT_EDX_SSE2 (1 << 26)
#define CPUID_FEAT_EDX_SS (1 << 27)
#define CPUID_FEAT_EDX_HTT (1 << 28)
#define CPUID_FEAT_EDX_TM1 (1 << 29)
#define CPUID_FEAT_EDX_IA64 (1 << 30)
#define CPUID_FEAT_EDX_PBE (1 << 31)

// ========================================================================
// EXTENDED FEATURE FLAGS - EBX (Function 0x07, ECX=0)
// ========================================================================

#define CPUID_FEAT_EXT_FSGSBASE (1 << 0)
#define CPUID_FEAT_EXT_TSC_ADJUST (1 << 1)
#define CPUID_FEAT_EXT_SGX (1 << 2)
#define CPUID_FEAT_EXT_BMI1 (1 << 3)
#define CPUID_FEAT_EXT_HLE (1 << 4)
#define CPUID_FEAT_EXT_AVX2 (1 << 5)
#define CPUID_FEAT_EXT_SMEP (1 << 7)
#define CPUID_FEAT_EXT_BMI2 (1 << 8)
#define CPUID_FEAT_EXT_ERMS (1 << 9)
#define CPUID_FEAT_EXT_INVPCID (1 << 10)
#define CPUID_FEAT_EXT_RTM (1 << 11)
#define CPUID_FEAT_EXT_MPX (1 << 14)
#define CPUID_FEAT_EXT_AVX512F (1 << 16)
#define CPUID_FEAT_EXT_RDSEED (1 << 18)
#define CPUID_FEAT_EXT_ADX (1 << 19)
#define CPUID_FEAT_EXT_SMAP (1 << 20)
#define CPUID_FEAT_EXT_CLFLUSHOPT (1 << 23)
#define CPUID_FEAT_EXT_CLWB (1 << 24)
#define CPUID_FEAT_EXT_SHA (1 << 29)

// ========================================================================
// EXTENDED FEATURE FLAGS - ECX (Function 0x80000001)
// ========================================================================

#define CPUID_FEAT_EXT_LAHF_LM (1 << 0) // LAHF/SAHF in long mode
#define CPUID_FEAT_EXT_CMP_LEGACY (1 << 1)
#define CPUID_FEAT_EXT_SVM (1 << 2) // AMD SVM
#define CPUID_FEAT_EXT_EXTAPIC (1 << 3)
#define CPUID_FEAT_EXT_CR8_LEGACY (1 << 4)
#define CPUID_FEAT_EXT_ABM (1 << 5) // Advanced bit manipulation
#define CPUID_FEAT_EXT_SSE4A (1 << 6)
#define CPUID_FEAT_EXT_MISALIGNSSE (1 << 7)
#define CPUID_FEAT_EXT_3DNOWPREFETCH (1 << 8)
#define CPUID_FEAT_EXT_OSVW (1 << 9)
#define CPUID_FEAT_EXT_IBS (1 << 10)
#define CPUID_FEAT_EXT_XOP (1 << 11)
#define CPUID_FEAT_EXT_SKINIT (1 << 12)
#define CPUID_FEAT_EXT_WDT (1 << 13)
#define CPUID_FEAT_EXT_LWP (1 << 15)
#define CPUID_FEAT_EXT_FMA4 (1 << 16)
#define CPUID_FEAT_EXT_TBM (1 << 21)

// ========================================================================
// EXTENDED FEATURE FLAGS - EDX (Function 0x80000001)
// ========================================================================

#define CPUID_FEAT_EXT_SYSCALL (1 << 11)
#define CPUID_FEAT_EXT_XD (1 << 20) // Execute Disable
#define CPUID_FEAT_EXT_1GB_PAGE (1 << 26)
#define CPUID_FEAT_EXT_RDTSCP (1 << 27)
#define CPUID_FEAT_EXT_64BIT (1 << 29) // Long mode (x86-64)

// ========================================================================
// CPU INFO STRUCTURE
// ========================================================================

typedef struct {
  // Vendor string (12 chars + null terminator)
  char vendor[13];

  // Brand string (48 chars + null terminator)
  char brand[49];

  // Basic info
  uint32_t max_basic_cpuid;
  uint32_t max_extended_cpuid;

  // Family, Model, Stepping
  uint32_t family;
  uint32_t model;
  uint32_t stepping;
  uint32_t type;

  // Features from function 0x01
  uint32_t features_ecx;
  uint32_t features_edx;

  // Extended features from function 0x07
  uint32_t extended_features_ebx;
  uint32_t extended_features_ecx;
  uint32_t extended_features_edx;

  // Extended features from function 0x80000001
  uint32_t ext_features_ecx;
  uint32_t ext_features_edx;

  // Cache info
  uint32_t cache_line_size;
  uint32_t cache_count;

  // APIC info
  uint32_t apic_id;
  uint32_t logical_processors;

  // Capabilities (derived from features)
  struct {
    bool has_fpu;
    bool has_tsc;
    bool has_msr;
    bool has_apic;
    bool has_cx8; // CMPXCHG8B
    bool has_sep; // SYSENTER/SYSEXIT
    bool has_cmov;
    bool has_pat;
    bool has_pse36; // 36-bit PSE
    bool has_clflush;
    bool has_mmx;
    bool has_fxsr; // FXSAVE/FXRSTOR
    bool has_sse;
    bool has_sse2;
    bool has_sse3;
    bool has_ssse3;
    bool has_sse4_1;
    bool has_sse4_2;
    bool has_htt; // Hyper-Threading
    bool has_pae; // Physical Address Extension
    bool has_pse; // Page Size Extension
    bool has_pge; // Page Global Enable
    bool has_mtrr;
    bool has_acpi;
    bool has_x2apic;
    bool has_popcnt;
    bool has_aes;
    bool has_avx;
    bool has_avx2;
    bool has_rdrand;
    bool has_rdseed;
    bool has_xsave;
    bool has_osxsave;
    bool has_syscall; // SYSCALL/SYSRET
    bool has_nx;      // Execute Disable
    bool has_1gb_pages;
    bool has_rdtscp;
    bool has_long_mode; // 64-bit support
    bool has_lahf_lm;   // LAHF/SAHF in long mode
    bool has_vmx;       // Intel VT-x
    bool has_svm;       // AMD-V
    bool has_smep;      // Supervisor Mode Execution Protection
    bool has_smap;      // Supervisor Mode Access Prevention
  } caps;

} cpu_info_t;

// ========================================================================
// GLOBAL CPU INFO
// ========================================================================

extern cpu_info_t cpu_info;

// ========================================================================
// FUNCTION PROTOTYPES
// ========================================================================

// Initialize CPU detection
void cpuid_init(void);

// Low-level CPUID instruction wrapper
void cpuid(uint32_t function, uint32_t *eax, uint32_t *ebx, uint32_t *ecx,
           uint32_t *edx);
void cpuid_ex(uint32_t function, uint32_t subfunc, uint32_t *eax, uint32_t *ebx,
              uint32_t *ecx, uint32_t *edx);

// Check if CPUID is supported
bool cpuid_is_supported(void);

// Feature detection helpers
bool cpu_has_feature(uint32_t feature_bit, uint32_t register_id);
const char *cpu_get_vendor(void);
const char *cpu_get_brand(void);

// Print CPU information
void cpuid_print_info(void);
void cpuid_print_features(void);
void cmd_cpuinfo(Terminal *term, const char *args);
void cmd_cpuinfo_detailed(Terminal *term, const char *args);
// Register IDs for cpu_has_feature
#define CPU_REG_ECX 0
#define CPU_REG_EDX 1
#define CPU_REG_EXT_EBX 2
#define CPU_REG_EXT_ECX 3
#define CPU_REG_EXT_EDX 4
#define CPU_REG_EXT81_ECX 5
#define CPU_REG_EXT81_EDX 6

#endif // CPUID_H