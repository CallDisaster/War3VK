#include "war3_jass_native_invoke_x86.h"

namespace dxvk::war3::hooks {

bool IsCdeclPackedInvokeSupported() {
#if defined(_MSC_VER) && defined(_M_IX86)
  return true;
#elif defined(__GNUC__) && defined(__i386__)
  return true;
#else
  return false;
#endif
}

int InvokeCdeclPacked(void *fn, const uint32_t *args, uint32_t count) {
  if (!fn)
    return 0;

#if defined(_MSC_VER) && defined(_M_IX86)
  // 仅在 x86+MSVC 下启用内联汇编调用器。
  // 与原版 ASM 对齐：调用后直接恢复到调用前 ESP，
  // 避免 cdecl/stdcall 差异导致的潜在栈失衡。
  int result = 0;
  void *targetFn = fn;
  const uint32_t *argArray = args;
  uint32_t argCount = count;
  void *savedEsp = nullptr;

  __asm {
    mov savedEsp, esp
    mov ecx, argCount
    mov esi, argArray
    test ecx, ecx
    jz do_call
push_loop:
    push dword ptr [esi + ecx*4 - 4]
    dec ecx
    jnz push_loop
do_call:
    call dword ptr [targetFn]
    mov result, eax
    mov esp, savedEsp
  }

  return result;
#elif defined(__GNUC__) && defined(__i386__)
  // MinGW/GCC i386 调用器：
  // - 按参数数组逆序压栈；
  // - call 后统一恢复到调用前 ESP；
  // - 与原版 ExecuteNativeFunction 的“栈指针复位”语义一致。
  int result = 0;
  const uint32_t *argArray = args;
  uint32_t argCount = count;
  uintptr_t savedEsp = 0;

  __asm__ __volatile__(
      "movl %%esp, %[saved]\n\t"
      "movl %[count], %%ecx\n\t"
      "movl %[args], %%esi\n\t"
      "test %%ecx, %%ecx\n\t"
      "jz 2f\n\t"
      "1:\n\t"
      "pushl -4(%%esi,%%ecx,4)\n\t"
      "decl %%ecx\n\t"
      "jnz 1b\n\t"
      "2:\n\t"
      "call *%[target]\n\t"
      "movl %[saved], %%esp\n\t"
      : "=&a"(result), [saved] "=&r"(savedEsp)
      : [target] "r"(fn), [args] "r"(argArray), [count] "r"(argCount)
      : "ecx", "esi", "memory", "cc");

  return result;
#else
  (void)args;
  (void)count;
  return 0;
#endif
}

} // namespace dxvk::war3::hooks
