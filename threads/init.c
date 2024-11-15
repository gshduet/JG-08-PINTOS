#include "threads/init.h"
#include "devices/input.h"
#include "devices/kbd.h"
#include "devices/serial.h"
#include "devices/timer.h"
#include "devices/vga.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include "threads/thread.h"
#include <console.h>
#include <debug.h>
#include <limits.h>
#include <random.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef USERPROG
#include "userprog/exception.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#endif
#include "tests/threads/tests.h"
#ifdef VM
#include "vm/vm.h"
#endif
#ifdef FILESYS
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "filesys/fsutil.h"
#endif

/* Page-map-level-4 with kernel mappings only. */
uint64_t *base_pml4;

#ifdef FILESYS
/* -f: Format the file system? */
static bool format_filesys;
#endif

/* -q: Power off after kernel tasks complete? */
bool power_off_when_done;

bool thread_tests;

static void bss_init(void);
static void paging_init(uint64_t mem_end);

static char **read_command_line(void);
static char **parse_options(char **argv);
static void run_actions(char **argv);
static void usage(void);

static void print_stats(void);

int main(void) NO_RETURN;

/* Pintos main program. */
int main(void) {
    uint64_t mem_end; // 메모리 최대 크기
    char **argv;      // 커널 명령줄 인수

    /* Clear BSS and get machine's RAM size. */
    bss_init(); // BSS 세그먼트 초기화

    /* Break command line into arguments and parse options. */
    argv = read_command_line(); // 커널 명령줄을 인수로 분해
    argv = parse_options(argv); // 옵션 파싱

    /* 스레드로 자신을 초기화하여 잠금을 사용할 수 있게 하고, 콘솔 잠금을 활성화합니다. */
    /* 스레드 시스템을 초기화합니다.
       현재 실행 중인 코드를 스레드로 변환하고
       ready_list, sleep_list, destruction_req 등의 전역 리스트와
       tid_lock을 초기화합니다.
       또한 initial_thread를 설정하고 초기화합니다. */
    thread_init();
    console_init(); // 콘솔 초기화

    /* Initialize memory system. */
    mem_end = palloc_init(); // 메모리 시스템 초기화
    malloc_init();           // 메모리 할당 초기화
    paging_init(mem_end);    // 페이징 초기화

#ifdef USERPROG
    tss_init(); // TSS 초기화
    gdt_init(); // GDT 초기화
#endif

    /* 인터럽트 초기화 */
    intr_init();  // 인터럽트 초기화
    timer_init(); // 타이머 초기화
    kbd_init();   // 키보드 초기화
    input_init(); // 입력 초기화
#ifdef USERPROG
    exception_init(); // 예외 초기화
    syscall_init();   // 시스템 호출 초기화
#endif
    /* 스레드 스케줄러를 시작하고 인터럽트를 활성화합니다.
       thread_start()는 다음과 같은 작업을 수행합니다:
       1. idle 스레드를 생성하여 CPU가 할 일이 없을 때 실행되도록 합니다.
       2. 선점형 스레드 스케줄링을 시작하기 위해 인터럽트를 활성화합니다.
       3. idle 스레드가 초기화될 때까지 대기합니다. */
    thread_start();      // 스레드 스케줄러 시작
    serial_init_queue(); // 시리얼 초기화
    timer_calibrate();   // 타이머 조정

#ifdef FILESYS
    /* Initialize file system. */
    disk_init();                  // 디스크 초기화
    filesys_init(format_filesys); // 파일 시스템 초기화
#endif

#ifdef VM
    vm_init(); // VM 초기화
#endif

    printf("Boot complete.\n"); // 부팅 완료 메시지 출력

    /* Run actions specified on kernel command line. */
    run_actions(argv); // 커널 명령줄에 지정된 작업 실행

    /* Finish up. */
    if (power_off_when_done)
        power_off(); // 전원 끄기
    /* 현재 실행 중인 스레드를 종료하고 스케줄러에 의해 다음 스레드가 실행되도록 합니다.
       이 함수는 init.c의 main() 함수가 종료될 때 호출되며,
       초기 스레드(initial thread)를 종료시키고 시스템의 정상적인 종료를 수행합니다.
       thread_exit() 호출 후에는 더 이상 이 코드로 돌아오지 않습니다. */
    thread_exit(); // 스레드 종료
}

/* Clear BSS */
static void bss_init(void) {
    /* The "BSS" is a segment that should be initialized to zeros.
       It isn't actually stored on disk or zeroed by the kernel
       loader, so we have to zero it ourselves.

       The start and end of the BSS segment is recorded by the
       linker as _start_bss and _end_bss.  See kernel.lds. */
    extern char _start_bss, _end_bss;
    memset(&_start_bss, 0, &_end_bss - &_start_bss);
}

/* Populates the page table with the kernel virtual mapping,
 * and then sets up the CPU to use the new page directory.
 * Points base_pml4 to the pml4 it creates. */
static void paging_init(uint64_t mem_end) {
    uint64_t *pml4, *pte;
    int perm;
    pml4 = base_pml4 = palloc_get_page(PAL_ASSERT | PAL_ZERO);

    extern char start, _end_kernel_text;
    // Maps physical address [0 ~ mem_end] to
    //   [LOADER_KERN_BASE ~ LOADER_KERN_BASE + mem_end].
    for (uint64_t pa = 0; pa < mem_end; pa += PGSIZE) {
        uint64_t va = (uint64_t)ptov(pa);

        perm = PTE_P | PTE_W;
        if ((uint64_t)&start <= va && va < (uint64_t)&_end_kernel_text)
            perm &= ~PTE_W;

        if ((pte = pml4e_walk(pml4, va, 1)) != NULL)
            *pte = pa | perm;
    }

    // reload cr3
    pml4_activate(0);
}

/* Breaks the kernel command line into words and returns them as
   an argv-like array. */
static char **read_command_line(void) {
    static char *argv[LOADER_ARGS_LEN / 2 + 1];
    char *p, *end;
    int argc;
    int i;

    argc = *(uint32_t *)ptov(LOADER_ARG_CNT);
    p = ptov(LOADER_ARGS);
    end = p + LOADER_ARGS_LEN;
    for (i = 0; i < argc; i++) {
        if (p >= end)
            PANIC("command line arguments overflow");

        argv[i] = p;
        p += strnlen(p, end - p) + 1;
    }
    argv[argc] = NULL;

    /* Print kernel command line. */
    printf("Kernel command line:");
    for (i = 0; i < argc; i++)
        if (strchr(argv[i], ' ') == NULL)
            printf(" %s", argv[i]);
        else
            printf(" '%s'", argv[i]);
    printf("\n");

    return argv;
}

/* Parses options in ARGV[]
   and returns the first non-option argument. */
static char **parse_options(char **argv) {
    for (; *argv != NULL && **argv == '-'; argv++) {
        char *save_ptr;
        char *name = strtok_r(*argv, "=", &save_ptr);
        char *value = strtok_r(NULL, "", &save_ptr);

        if (!strcmp(name, "-h"))
            usage();
        else if (!strcmp(name, "-q"))
            power_off_when_done = true;
#ifdef FILESYS
        else if (!strcmp(name, "-f"))
            format_filesys = true;
#endif
        else if (!strcmp(name, "-rs"))
            random_init(atoi(value));
        else if (!strcmp(name, "-mlfqs"))
            thread_mlfqs = true;
#ifdef USERPROG
        else if (!strcmp(name, "-ul"))
            user_page_limit = atoi(value);
        else if (!strcmp(name, "-threads-tests"))
            thread_tests = true;
#endif
        else
            PANIC("unknown option `%s' (use -h for help)", name);
    }

    return argv;
}

/* Runs the task specified in ARGV[1]. */
static void run_task(char **argv) {
    const char *task = argv[1];

    printf("Executing '%s':\n", task);
#ifdef USERPROG
    if (thread_tests) {
        run_test(task);
    } else {
        process_wait(process_create_initd(task));
    }
#else
    run_test(task);
#endif
    printf("Execution of '%s' complete.\n", task);
}

/* Executes all of the actions specified in ARGV[]
   up to the null pointer sentinel. */
static void run_actions(char **argv) {
    /* An action. */
    struct action {
        char *name;                    /* Action name. */
        int argc;                      /* # of args, including action name. */
        void (*function)(char **argv); /* Function to execute action. */
    };

    /* Table of supported actions. */
    static const struct action actions[] = {
        {"run", 2, run_task},
#ifdef FILESYS
        {"ls", 1, fsutil_ls},   {"cat", 2, fsutil_cat}, {"rm", 2, fsutil_rm},
        {"put", 2, fsutil_put}, {"get", 2, fsutil_get},
#endif
        {NULL, 0, NULL},
    };

    while (*argv != NULL) {
        const struct action *a;
        int i;

        /* Find action name. */
        for (a = actions;; a++)
            if (a->name == NULL)
                PANIC("unknown action `%s' (use -h for help)", *argv);
            else if (!strcmp(*argv, a->name))
                break;

        /* Check for required arguments. */
        for (i = 1; i < a->argc; i++)
            if (argv[i] == NULL)
                PANIC("action `%s' requires %d argument(s)", *argv, a->argc - 1);

        /* Invoke action and advance. */
        a->function(argv);
        argv += a->argc;
    }
}

/* Prints a kernel command line help message and powers off the
   machine. */
static void usage(void) {
    printf("\nCommand line syntax: [OPTION...] [ACTION...]\n"
           "Options must precede actions.\n"
           "Actions are executed in the order specified.\n"
           "\nAvailable actions:\n"
#ifdef USERPROG
           "  run 'PROG [ARG...]' Run PROG and wait for it to complete.\n"
#else
           "  run TEST           Run TEST.\n"
#endif
#ifdef FILESYS
           "  ls                 List files in the root directory.\n"
           "  cat FILE           Print FILE to the console.\n"
           "  rm FILE            Delete FILE.\n"
           "Use these actions indirectly via `pintos' -g and -p options:\n"
           "  put FILE           Put FILE into file system from scratch disk.\n"
           "  get FILE           Get FILE from file system into scratch disk.\n"
#endif
           "\nOptions:\n"
           "  -h                 Print this help message and power off.\n"
           "  -q                 Power off VM after actions or on panic.\n"
           "  -f                 Format file system disk during startup.\n"
           "  -rs=SEED           Set random number seed to SEED.\n"
           "  -mlfqs             Use multi-level feedback queue scheduler.\n"
#ifdef USERPROG
           "  -ul=COUNT          Limit user memory to COUNT pages.\n"
#endif
    );
    power_off();
}

/* Powers down the machine we're running on,
   as long as we're running on Bochs or QEMU. */
void power_off(void) {
#ifdef FILESYS
    filesys_done();
#endif

    print_stats();

    printf("Powering off...\n");
    outw(0x604, 0x2000); /* Poweroff command for qemu */
    for (;;)
        ;
}

/* Print statistics about Pintos execution. */
static void print_stats(void) {
    timer_print_stats();
    thread_print_stats();
#ifdef FILESYS
    disk_print_stats();
#endif
    console_print_stats();
    kbd_print_stats();
#ifdef USERPROG
    exception_print_stats();
#endif
}
