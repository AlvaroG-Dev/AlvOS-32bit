#include "sse.h"
#include "cpuid.h"
#include "log.h"

void sse_init(void) {
  // 1. Verificar soporte SSE en CPUID
  if (!cpu_info.caps.has_sse) {
    log_message(LOG_WARN, "SSE no soportado por la CPU");
    return;
  }

  // 2. Habilitar FPU y SSE
  // CR0: Limpiar bit 2 (EM - Emulation), establecer bit 1 (MP - Monitor
  // Coprocessor) CR0: Establecer bit 5 (NE - Numeric Error) para excepciones
  // FPU nativas
  uint32_t cr0;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1 << 2); // EM = 0
  cr0 |= (1 << 1);  // MP = 1
  cr0 |= (1 << 5);  // NE = 1
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

  // CR4: Establecer bit 9 (OSFXSR - OS support for fxsave/fxrstor)
  // CR4: Establecer bit 10 (OSXMMEXCPT - OS support for unmasked SIMD floating
  // point exceptions)
  uint32_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1 << 9);
  cr4 |= (1 << 10);
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

  // Inicializar FPU
  __asm__ volatile("fninit");

  log_message(LOG_INFO, "SSE y FPU habilitados correctamente");
}

// Implementación de memcpy usando SSE (MOVAPS / MOVUPS)
// Nota: MOVAPS requiere alineación de 16 bytes. MOVUPS no, pero es algo más
// lenta. Esta implementación usa MOVUPS para máxima compatibilidad, pero rep
// movsl para el resto.
void *sse_memcpy(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  // Si el tamaño es pequeño o no estamos alineados, usar rep movsb/movsl
  if (n < 64) {
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dest;
  }

  // Copiar bloques de 16 bytes usando SSE (XMM resisters)
  size_t blocks = n / 16;
  size_t rem = n % 16;

  for (size_t i = 0; i < blocks; i++) {
    __asm__ volatile("movups (%0), %%xmm0\n"
                     "movups %%xmm0, (%1)\n"
                     :
                     : "r"(s), "r"(d)
                     : "xmm0", "memory");
    s += 16;
    d += 16;
  }

  // Copiar el resto
  if (rem > 0) {
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(rem) : : "memory");
  }

  return dest;
}

// Memset optimizado con SSE
void *sse_memset(void *dest, int val, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  uint8_t v = (uint8_t)val;

  if (n < 64) {
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
    return dest;
  }

  // Crear un registro XMM lleno con el valor
  uint32_t v32 = (v << 24) | (v << 16) | (v << 8) | v;
  __asm__ volatile("movd %0, %%xmm0\n"
                   "pshufd $0, %%xmm0, %%xmm0\n"
                   :
                   : "r"(v32)
                   : "xmm0");

  size_t blocks = n / 16;
  size_t rem = n % 16;

  for (size_t i = 0; i < blocks; i++) {
    __asm__ volatile("movups %%xmm0, (%0)" : : "r"(d) : "memory");
    d += 16;
  }

  if (rem > 0) {
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(rem) : "a"(v) : "memory");
  }

  return dest;
}

void *sse_memset32(void *dest, uint32_t val, size_t n) {
  uint32_t *d = (uint32_t *)dest;

  if (n < 16) {
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(val) : "memory");
    return dest;
  }

  __asm__ volatile("movd %0, %%xmm0\n"
                   "pshufd $0, %%xmm0, %%xmm0\n"
                   :
                   : "r"(val)
                   : "xmm0");

  size_t blocks = n / 4;
  size_t rem = n % 4;

  for (size_t i = 0; i < blocks; i++) {
    __asm__ volatile("movups %%xmm0, (%0)" : : "r"(d) : "memory");
    d += 4;
  }

  if (rem > 0) {
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(rem) : "a"(val) : "memory");
  }

  return dest;
}
