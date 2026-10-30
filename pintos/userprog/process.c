#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "userprog/syscall.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}


struct thread_create_aux{
	struct child_status *cs;
	char *fn_copy;
};

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */

	// 자식의 상태를 기록할 공간 할당 및 부모의 children 리스트에 추가
	struct child_status *cs = malloc (sizeof (struct child_status));
	if (cs == NULL) return TID_ERROR; 

    cs->exit_status = -1;            
    sema_init(&cs->exit_done, 0);     


	list_push_back (&thread_current()->children, &cs->elem);

	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL){
		list_remove(&cs->elem);
		free(cs);
		return TID_ERROR;
	}
	strlcpy (fn_copy, file_name, PGSIZE);

	// file_name에서 파일명만 잘라서 줘야 함
	char cut_file_name[16];
	char *save_ptr;
	strlcpy(cut_file_name, file_name, sizeof(cut_file_name));
	char * only_file_name = strtok_r(cut_file_name, " ", &save_ptr);

	struct thread_create_aux *aux = malloc(sizeof(struct thread_create_aux));
	if(aux == NULL){
		palloc_free_page(fn_copy);
		list_remove(&cs->elem);
		free(cs);
		return TID_ERROR;
	}

	aux -> fn_copy = fn_copy;
	aux -> cs = cs;

	/* Create a new thread to execute FILE_NAME. */
	// thread_create(스레드(파일)명, 우선순위, 바로 실행할 함수, 바로 실행할 함수에 전달할 인자)
	tid = thread_create (only_file_name, PRI_DEFAULT, initd, aux);
	if (tid == TID_ERROR){
		palloc_free_page (fn_copy);
		list_remove(&cs->elem);
		free(cs);
		free(aux);
		return tid;
	}

	cs -> tid = tid;

	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	struct thread_create_aux *aux = f_name;

	char *fn_copy = aux->fn_copy;

	thread_current()->as_child = aux->cs;

	free(aux);

	// 기본 환경 구축(초기값 세팅)
	process_init ();
	// 인자로 받은 파일명을 디스크에 찾아 메모리에 적재/실행
	if (process_exec (fn_copy) < 0)
		PANIC("Fail to launch initd\n");
	// 에러 발생
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	struct thread *parent = thread_current ();

	/* 1. 자식의 상태를 보관할 공간 할당 */
	struct child_status *cs = malloc (sizeof (struct child_status));
	if (cs == NULL)
		return TID_ERROR;

	/* 2. 할당한 공간에 child_status 구조체의 초기값 지정 */
	cs->tid = TID_ERROR;
	cs->exit_status = -1;
	cs->fork_success = false;
	sema_init (&cs->fork_done, 0);
	sema_init (&cs->exit_done, 0);
	cs->ref_count = 2; // 부모(1) + 자식(1)
	lock_init (&cs->ref_lock);

	/* 3. 부모의 children 필드에 저장 */
	list_push_back (&parent->children, &cs->elem);

	/* 스케줄링용 tf와 구분하여 자식이 이어갈 사용자 실행 상태를 보관한다. */
	parent->fork_if = *if_; /* 부모 */
	parent->fork_success=false;

	// 자식 스레드 생성(__do_fork 실행) 
	tid_t tid= thread_create (name,	PRI_DEFAULT, __do_fork, parent);

	// 스레드 생성이 실패한 경우, 등록했던 child_status 제거 및 free
	if (tid==TID_ERROR) {
		list_remove(&cs->elem);
		free(cs);
		return TID_ERROR;
	}
	cs->tid = tid;

	sema_down(&parent->fork_done);

	// 자식의 복제 작업이 실패한 경우, 등록했던 child_status 제거 및 메모리 해제
	if(!parent->fork_success) {
		list_remove(&cs->elem);
		free(cs);
		return TID_ERROR;
	}
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va)) return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);

	if (parent_page == NULL) return false;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page (PAL_USER);

	if (newpage == NULL) return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);

	writable = (*pte & PTE_W) != 0;

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
	/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	/* thread_create의 마지막 인자로 전달한 부모 TCB를 받는다. */
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	bool succ = true;

	ASSERT (parent != NULL);
	ASSERT (current->fdt != NULL);  /* 테이블이 준비되어 있어야 함 */
	ASSERT (current->running_file == NULL);

	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	/*  부모의 process_fork(): */


	struct list_elem *e;
	// 부모의 children 리스트를 돌면서 내 tid와 일치하는 child_status를 찾아 내 as_child에 저장 (process_fork에서 만든 초기값을 가져옴)
	for (e = list_begin (&parent->children); e != list_end (&parent->children); e = list_next (e)) {
        struct child_status *cs = list_entry (e, struct child_status, elem);
        if (cs->tid == current->tid) {
            current->as_child = cs;
            break;
        }
    }

	/* 1. Read the cpu context to local stack. */
	/* 자식의 커널 스택에 복사해 사용자 모드 복귀 시 레지스터를 복원한다. */
	/* 부모의 *if_ → parent->fork_if → 자식의 if_ */
	/* fork의 자식 반환값은 사용자 모드에서 rax를 통해 전달된다. */
	memcpy (&if_, &parent->fork_if, sizeof (struct intr_frame)); /* 자식 */
	if_.R.rax = 0;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create ();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	/* thread_create()가 할당한 fdt에 부모의 열린 파일을 복제한다. */
	current->next_fd = parent->next_fd;

	/* 파일 시스템 락 획득; */
	lock_acquire(&filesys_lock);

	if (parent->fdt != NULL) {
		for (int fd = 2; fd < FD_MAX; fd++) {
			if (parent->fdt[fd] == NULL)
				continue;

			current->fdt[fd] =
				file_duplicate(parent->fdt[fd]);

			if (current->fdt[fd] == NULL) {
				// 파일 시스템 락 해제;
				lock_release(&filesys_lock);
				goto error;
			}
		}
	}

	if (parent->running_file != NULL) {
		current->running_file =
			file_duplicate(parent->running_file);

		if (current->running_file == NULL) {
			// 파일 시스템 락 해제;
			lock_release(&filesys_lock);
			goto error;
		}
	}

	// 파일 시스템 락 해제;
	lock_release(&filesys_lock);
	process_init ();

	parent->fork_success=true;
	sema_up(&parent->fork_done);

	/* Finally, switch to the newly created process. */
	/* do iret 은 사용자 프로그램으로 실행을 전환 하는 함수*/
	if (succ)
		do_iret (&if_);
error:
	current->status_exit=-1;
	parent->fork_success = false;
    sema_up(&parent->fork_done);
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	// 기존 메모리 해제
	process_cleanup ();

	// rsp로 스택 공간 확장 (argv를 돌면서 문자열 길이(strlen() + 1(/0)))
	// rdi -> args
	// rsi -> argv 배열의 시작점 (argv[0])


	/* And then load the binary */
	// 새 binary load 및 인자 스택 setting
	success = load (file_name, &_if);

	/* If load failed, quit. */
	// 커널 복사본 메모리 해제
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	/* do_iret 은 사용자 프로그램으로 실행을 전환하는 함수 */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	// while(1);
	// timer_sleep(100);

	struct thread * curr = thread_current();
	struct list_elem *e;


	for (e = list_begin (&curr->children); e != list_end (&curr->children); e = list_next (e)) {
        struct child_status *cs = list_entry (e, struct child_status, elem);
        if (cs->tid == child_tid) {
			// 자식 프로세스가 종료될 때 까지(죽을때 까지) 대기
			sema_down (&cs->exit_done);

			// 자식의 종료 상태 코드 가져오기
			int status = cs->exit_status;
			// 부모의 child 리스트에서 제거 및 free()
			list_remove (&cs->elem);
			free(cs);

			// 자식의 종료 상태 반환
			return status;
        }
    }

	return -1;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	
	if (curr->pml4 != NULL) {
    	printf ("%s: exit(%d)\n", curr->name, curr->status_exit);
	}

	lock_acquire (&filesys_lock);

	// fdt 정리(닫기)
	if (curr->fdt != NULL) {
		// fd = 0은 stdin(표준 입력),  fd = 1은 stdout(표준 출력)이기에 fd=2부터
		// 1페이지는 4KB 이기에 포인터는 8바이트이기에 4096 / 8 = 512이니, 2~511까지 순회
        for (int fd = 2; fd < FD_MAX; fd++) {
			struct file *file = curr->fdt[fd];   
			if (file != NULL) {            
				file_close (file);
				curr->fdt[fd] = NULL;
            }
        }
        // FDT 페이지 메모리 해제
        palloc_free_page (curr->fdt);
        curr->fdt = NULL;
    }

	// 쓰기 금지 해제
	if (curr->running_file != NULL) {
        file_close (curr->running_file);
        curr->running_file = NULL;
    }

    lock_release (&filesys_lock);

	if(curr->as_child != NULL){
		// 자식 프로세스가 종료되면 의 thread는 사라짐
		// 때문에 자식 프로세스의 종료 상태를 남겨놓기 위해(부모 프로세스가 자식의 작업의 성공 여부에 따라 다음 동작을 결정하기에)
		// 사라지지 않은 구조체인 child_status의 exit_status에 자식 프로세스의 종료 상태인 status_exit를 백업해 둠
		curr->as_child->exit_status = curr->status_exit;
		// 부모 프로세스 깨우기
		sema_up(&curr->as_child->exit_done);
	}

	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}

	/* 종료하거나 exec로 교체할 때 실행 파일의 쓰기 금지를 해제한다. */
	if (curr->running_file != NULL) {
		lock_acquire (&filesys_lock);
		file_close (curr->running_file);
		curr->running_file = NULL;
		lock_release (&filesys_lock);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
 

 
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	bool filesys_locked = false;
	int i;

	char *argv[100];
	int argc = 0;
	char *token, *save_ptr;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	for (token = strtok_r ((char*)file_name, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr)){
		argv[argc] = token;
		argc += 1;
	}	

	/* Open executable file. */
	lock_acquire (&filesys_lock);
	filesys_locked = true;
	file = filesys_open (argv[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}
	/* 로딩 중부터 실행이 끝날 때까지 파일 내용이 바뀌지 않게 한다. */
	file_deny_write (file);

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv[0]);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	/* ELF ehdr에는 프로그램의 시작 주소, 실행 파일의 종류
		cpu 아키텍처, 프로그램 헤더의 위치와 개수 등이 기록되어 있음
		rip 는 다음에 실행할 명령어 저장하는 곳
	*/
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	
	int64_t arg_len = 0;

	// 1. argv[1]~ argv[n]까지 문자열 길이 구하기 및 문자열 합치기
	for(int i = 0; i < argc; i++){
		arg_len += strlen(argv[i]) + 1;
	}

	// 3 all_of_argument_length를 8의 배수로 조정하기
	int64_t stack_length = (arg_len + 7) & ~7;

	// 4. stack 공간 할당 
	if_->rsp = if_->rsp - stack_length;
	// memset 사용하여 stack에 할당한 공간 0으로 초기화
	memset((void*)if_->rsp, 0, stack_length);
	
	// stack에 인자 문자열 순서대로 복사
	char *curr_addr = (char *)if_->rsp;
	for (int i = 0; i < argc; i++) {
		int one_arg_len = strlen(argv[i]) + 1;		// 인자 문자열의 길이 구하기
		memcpy(curr_addr, argv[i], one_arg_len);	// 스택의 빈 공간에 인자 문자열 복사
    	argv[i] = curr_addr;                   		// argv[i]가 가리키는 곳을 스택 주소로 갱신
    	curr_addr += one_arg_len;   				// 다음 문자열 시작 위치로 이동
	}

	// argv[argc] = NULL 처리
	if_->rsp -= 8;
	*(char **)if_->rsp = NULL;

	// argv[i]에 주소 갱신(최신화) -> 지금 넣은건 인자 문자열들만 넣음
	// argv[0] ~ argv[n] 까지 인자 문자열을 가리키는 주소를 갱신 ? -> 역순으로 넣음 
	for(int i= argc-1; i >= 0; i--) {
		if_->rsp -= 8;
		*(char **)if_->rsp = argv[i]; 
	}

	// Pintos x86-64 환경에서 NULL 포인터는 8바이트가 모두 0인 값입니다, 위 아래 코드 같은 기능임
	
	// rdi 정의(argc)(파싱한 인자의 개수)
	if_->R.rdi = argc;
	// rsi 정의(argv 배열의 시작 주소) 0번째(첫번째) 인자 시작 주소를 rsi에 넣어주니까
	if_->R.rsi = if_->rsp; 

	if(if_->rsp % 16 == 8) {
		if_->rsp -= 8;
		memset ((void *) if_->rsp, 0, 8);
	}
	
	// stack 정렬 및 fake return address (rsp 최하단에 위치하는 8byte짜리 공간? 보통 0으로 채움)	 
	if_->rsp -= 8;
	memset((void*)if_->rsp,0,8); 

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	if (filesys_locked) {
		if (success)
			t->running_file = file;
		else
			file_close (file);
		lock_release (&filesys_lock);
	}
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
/* 사용자 프로그램이 사용할 스택 메모리를 준비하고, 스택 포인(rsp)를 초기화하는 함수 */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	struct vm_entry *vme = (struct vm_entry *)aux;


    // 파일 오프셋부터 read_bytes만큼 물리 메모리에 읽어옴
	// 정확히 읽어오지 않으면(읽어온 byte수와 read_bytes가 일치하지 않으면) return false
    if (file_read_at (vme->file, page->frame->kva, vme->read_bytes, vme->offset) != (int)vme->read_bytes) {
        free (vme);
        return false;
    }

    // 페이지의 남은 공간(zero_bytes)을 0으로 초기화 
    memset (page->frame->kva + vme->read_bytes, 0, vme->zero_bytes);

    free(vme); 

    return true;
}



/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
// File의 오프셋(OFS)부터 시작하여 read_bytes 바이트만큼 가져와서 0으로 초기화 시키고
// writeable이 true일 경우, 쓰기 가능이여야 하고, false인 경우 읽기전용이여야 함
// 메모리 할당 오류나 디스크 읽기 오류 발생 시 false 반환, 아니면 true 반환
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		// file에서 ofs에서 시작해서 읽어올 byte 수
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		// 0으로 채울 byte 수
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct vm_entry *aux = malloc(sizeof(struct vm_entry));
		aux->file = file;
		aux->offset = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer (VM_ANON, upage, writable, lazy_load_segment, aux)){
			free(aux);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		ofs += page_read_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */
	if (vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true)) {
        success = vm_claim_page (stack_bottom);
        if (success) {
            if_->rsp = USER_STACK;
        }
    }
	return success;
}
#endif /* VM */
