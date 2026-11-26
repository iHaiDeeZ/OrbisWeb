/* Wisp: the app's GPU address space, given to the web process (from WinePS4 port/ps4_wine_port.c).
 *
 * A PS4 app has one GPU address space (vm#1), and the kernel gives it to the app's first process:
 * here the UI, which draws on the CPU. The web process's RADV (Mesa on Zink) can open the GPU only
 * once it is free ("gc_open. Can't open GPU VA space. ret:0x804c0003" otherwise). A vfork child that
 * leaves the app's budget (budget_set 571 with 0) and ends without an exec ends on the UI's address
 * space, and the kernel frees the GPU address space with it; the UI's screen goes on (the display
 * reads its buffers without it). The app's end then logs "no vm space" and gvmsw_close errors,
 * which do no harm. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

void wisp_log(const char *format, ...);

void ps4_release_gpu_space(void)
{
    static long result[4];  /* budget_set's result and failure, then budget_getid's */
    sigset_t all, old;
    long ret, flag;

    memset(result, 0, sizeof(result));
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    __asm__ volatile("syscall\n\t"
                     "jnc 1f\n\t"
                     "movq $-1,%%rdx\n\t"          /* failed: rax = errno */
                     "jmp 9f\n"
                     "1:\ttestq %%rdx,%%rdx\n\t"   /* rdx: 0 in the parent, 1 in the child */
                     "jz 9f\n\t"
                     /* the child: budget_set( 0 ), budget_getid(), exit( 0 ) */
                     "xorl %%edi,%%edi\n\t"
                     "movl $571,%%eax\n\t"
                     "syscall\n\t"
                     "movq %%rax,0(%%rbx)\n\t"
                     "sbbq %%rax,%%rax\n\t"
                     "movq %%rax,8(%%rbx)\n\t"
                     "movl $609,%%eax\n\t"
                     "syscall\n\t"
                     "movq %%rax,16(%%rbx)\n\t"
                     "sbbq %%rax,%%rax\n\t"
                     "movq %%rax,24(%%rbx)\n\t"
                     "xorl %%edi,%%edi\n\t"
                     "movl $1,%%eax\n\t"
                     "syscall\n"
                     "9:"
                     : "=a"(ret), "=d"(flag)
                     : "a"(66 /* SYS_vfork */), "b"(result)
                     : "rcx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (flag < 0) {
        wisp_log("gpu: vfork refused (errno %ld): the UI keeps the app's GPU address space; GPU rendering may fail", ret);
        return;
    }
    wisp_log("gpu: the app's GPU address space freed for the web process (child %s its budget: %s %ld)",
        result[1] ? "could not leave" : "left", result[1] ? "errno" : "result", result[0]);
}
