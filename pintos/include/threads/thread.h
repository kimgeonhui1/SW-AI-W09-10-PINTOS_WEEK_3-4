#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"

#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

#ifdef USERPROG
#define FD_MAX 512	                    /* 프로세스 FD 테이블의 항목 수. fd · fork. */
#endif

bool priority_sort(const struct list_elem *a,const struct list_elem *b, void *aux UNUSED);
void priority_thread_yield(void);

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */

#ifdef USERPROG
/* 자식 TCB가 해제된 뒤에도 부모가 복제 결과와 종료 상태를 확인한다. */
struct child_status {
	tid_t tid;                         /* 자식의 식별 번호(자식의 ID). fork · wait. */ 
	int exit_status;                   /* 자식의 종료 코드. exit · wait. */
	bool fork_success;                 /* 자원 복제 성공 여부. fork. */

	struct semaphore fork_done;        /* 자원 복제(fork) 완료 알림. fork. */
	struct semaphore exit_done;        /* 자식 종료 알림. exit · wait. */


	struct list_elem elem;             /* 부모의 자식 상태 목록에 연결. fork · wait. */

	// 자식과 부모 둘 다 살아있을 때는 ref_count = 2
	// 자식이나 부모가 죽으면 ref_count -- 되며, ref_count = 0이 되면 child_status을 free()
	int ref_count;                     /* 부모와 자식이 보유한 참조 수. exit · wait. */ 
	struct lock ref_lock;              /* 참조 수 변경을 보호하는 락. exit · wait. */
};
#endif

struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */
	struct list_elem sleep_elem;        /* 알람 대기 목록에 연결. */

	int my_priority;                   /* 우선순위 기부 전 원래 우선순위. */
	struct list donation_list;         /* 기부받은 스레드 목록. */
	struct lock *lock_to_stop;          /* 획득을 기다리는 락. */
	struct list_elem donation_elem;    /* 락 소유자의 기부 목록에 연결. */

	int nice;                          /* MLFQS nice 값. */
	int recent_cpu;                    /* MLFQS CPU 사용량. */
	struct list_elem allelem;          /* 전체 스레드 목록에 연결. */

	int status_exit;                   /* 종료 상태. 기본값 -1. */

	struct file **fdt;					// fdt 정의
	int next_fd;						// 다음 FD


#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
	struct list children;               /* 직계 자식들의 child_status 목록. fork · wait. */
	struct child_status *as_child;      /* 내 부모와 공유하는 내 상태. fork · exit · wait. */
	struct file *running_file;          /* 실행 중인 내 프로그램 파일. 쓰기 금지용. rox. */
	struct intr_frame fork_if;          /* fork 시점 사용자 레지스터 */

	bool fork_success; 					/* 이번 fork의 자원 복제 결과 */
	struct semaphore fork_done;			/* 이번 fork의 복제 완료 알림 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
	uintptr_t saved_user_stack;
#endif
	int64_t timer_tick;                 /* 알람에서 깨어날 tick. */

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};
/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
// 커널 옵션으로 -mlfqs를 전달하면 thread_mlfqs가 true로 설정됨. 기본값은 false
// thread_mlfqs가 false이면, 우선순위 스케줄러 + 기부(Donation) 방식으로 작동함
// thread_mlfqs가 true이면 MLFQS(다단계 피드백 큐 스케줄러)방식으로 작동함
extern bool thread_mlfqs;

extern int load_avg;					// load_avg 정의

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void mlfqs_calculate_priority (struct thread *t);
void mlfqs_calculate_recent_cpu (struct thread *t);
void mlfqs_calculate_load_avg (void);
void mlfqs_increment_recent_cpu (void);
void mlfqs_recalculate_all_priority (void);
void mlfqs_recalculate_all_recent_cpu (void);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */
