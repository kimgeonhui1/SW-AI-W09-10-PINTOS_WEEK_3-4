#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/vaddr.h"
#include "threads/init.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "threads/palloc.h"


void syscall_entry (void);
void syscall_handler (struct intr_frame *);

void check_addr(void * addr);
void check_addr_page(void * buffer, size_t size, bool);
void check_addr_byte(char *string);

void halt(void);
void exit(int status);
int write(int fd, const void *buffer, unsigned size);

static tid_t sys_fork (const char *thread_name, struct intr_frame *f);
int exec (const char *cmd_line);
int wait (tid_t pid);

bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

struct lock filesys_lock;

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	lock_init(&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
    thread_current()->saved_user_stack = f->rsp;
	// TODO: Your implementation goes here.
	switch(f->R.rax){
        // 인자 0개
        case SYS_HALT:
            halt();
            break;

        // 인자 1개 (int status)
        case SYS_EXIT:
            exit((int)f->R.rdi);
            break;

        // 인자 3개 (int fd, void *buffer, unsigned size)
        case SYS_WRITE:
            f->R.rax = write((int)f->R.rdi, (void *)f->R.rsi, (unsigned)f->R.rdx);
            break;

        // 인자 2개 (const char *thread_name, struct intr_frame *f)
        case SYS_FORK:
            f->R.rax = sys_fork ((const char *) f->R.rdi, f);
            break;

        // 인자 1개 (const char *cmd_line)
        case SYS_EXEC:
            f->R.rax = exec((const char *)f->R.rdi);
            break;

        // 인자 1개 (tid_t pid)
        case SYS_WAIT:
            f->R.rax = wait((tid_t)f->R.rdi);
            break;

        // 인자 2개 (const char *file, unsigned initial_size)
        case SYS_CREATE:
            f->R.rax = create((const char *)f->R.rdi, (unsigned)f->R.rsi);
            break;

        // 인자 1개 (const char *file)
        case SYS_REMOVE:
            f->R.rax = remove((const char *)f->R.rdi);
            break;

        // 인자 1개 (const char *file)
        case SYS_OPEN:
            f->R.rax = open((const char *)f->R.rdi);
            break;

        // 인자 1개 (int fd)
        case SYS_FILESIZE:
            f->R.rax = filesize((int)f->R.rdi);
            break;

        // 인자 3개 (int fd, void *buffer, unsigned size)
        case SYS_READ:
            f->R.rax = read((int)f->R.rdi, (void *)f->R.rsi, (unsigned)f->R.rdx);
            break;

        // 인자 2개 (int fd, unsigned position)
        case SYS_SEEK:
            seek((int)f->R.rdi, (unsigned)f->R.rsi);
            break;

        // 인자 1개 (int fd)
        case SYS_TELL:
            f->R.rax = tell((int)f->R.rdi);
            break;

        // 인자 1개 (int fd)
        case SYS_CLOSE:
            close((int)f->R.rdi);
            break;

        default:
            exit(-1);
            break;
    }

}

void
check_addr(void * addr){
    struct thread *curr = thread_current();

	// NULL 포인터인지
	// 커널 공간을 가리키고 있는지
	// 페이지 테이블(pml4)에 매핑되어 있는지
    // if (addr == NULL || !is_user_vaddr (addr) || pml4_get_page (curr->pml4, addr) == NULL)  exit (-1);    
    if (addr == NULL || !is_user_vaddr (addr))  exit (-1);    

    #ifdef VM
        // pml4_get_page 대신 spt_find_page 
        // struct page *page = spt_find_page(&curr->spt, addr);
        // if (page != NULL && page->writable == false) {
        //     exit(-1);
        // }

    #else
        if (pml4_get_page(curr->pml4, addr) == NULL) {
            exit(-1);
        }
        return NULL;
    #endif

    return;
}

// 페이지 단위 검사하는(check_addr()함수 사용)함수 구현해야함
// -> read, write에서 사용
// read는 디스크(파일)에서 데이터를 가져와서 User Memory(buffer)에 Write 해야함 -> Writable == true여야함
// write는 User Memory(buffer)에 있는 내용을 읽어서 파일이나 화면에 출력
void
check_addr_page(void * buffer, size_t size, bool write){
	// 예외사항
	if(buffer == NULL) exit(-1);
    if(!is_user_vaddr(buffer)) exit(-1);
	if(size ==0) return;

    struct thread *curr = thread_current();
    // struct page *writable_page = spt_find_page(&curr->spt, buffer);
    // if (writable_page != NULL && writable_page->writable == false && write == true) exit(-1);
    
	const char *str_start = (const char *)buffer;
    const char *str_end = str_start + size - 1;	

    char *start_page = pg_round_down(str_start);
    char *end_page = pg_round_down(str_end);

	// 시작주소 ~ 끝주소까지 페이지 단위로 검사
	// pg_round_down(페이지에 들어가있는 주소?) -> 페이지의 시작점을 알려줌
	// pg_round_up () -> 페이지의 끝을 알려줌
	for(char *page = start_page; page <= end_page; page += PGSIZE) {
        check_addr(page);
        
        struct page *writable_page = spt_find_page(&curr->spt, page);
        if (writable_page != NULL && writable_page->writable == false && write == false) exit(-1);
    }

    // check_addr(str_end);

	return;
}

// 1byte 단위로 검사하는 함수 구현해야함?
// -> fork, exec, create, remove, open에서 사용
void
check_addr_byte(char *string){
	// while(*string != '\0'){
	// 	check_addr((void *)string);
	// 	string++;
	// }

	// 
	check_addr((void *)string);
	while(*string != '\0'){
		string++;
		check_addr((void *)string);
	}
	
	return;
}

void halt(void){
	power_off();
}

void exit(int status){
	thread_current() ->status_exit = status;

    if (lock_held_by_current_thread(&filesys_lock)) lock_release(&filesys_lock);
	// 현재 실행중인 스레드를 강제 종료시킴
	thread_exit();
}

int write(int fd, const void *buffer, unsigned size){
	check_addr_page((void *)buffer, size, true);

    struct thread *curr = thread_current();

    if (size == 0) return size;

	if (fd == 1){
		putbuf((char *)buffer, size);
		return size;
	}
	else if (fd <= 0 || fd >= FD_MAX){
		return -1;
	}
    else{
        struct file *file = curr->fdt[fd];

        if (file == NULL) return -1;

        lock_acquire(&filesys_lock);

        int write_size = file_write(file, buffer, size);
        
        lock_release(&filesys_lock);
    	
        return write_size;
    }
}

/* 이름을 읽기 전에 검증하고 시스템 콜 진입 시점의 사용자 프레임을 넘긴다. */
static tid_t
sys_fork (const char *thread_name, struct intr_frame *f) {
	check_addr_byte ((char *) thread_name);
	return process_fork (thread_name, f);
}
int exec (const char *cmd_line) { 
    check_addr_byte((char *)cmd_line);

    // 4KB의 페이지 할당 및 인자로 받은 cmd_line(파일명 + 인자로 이뤄진 문자열) 살리기 위한 공간 할당 
    // process_exec()함수에서 process_cleanup()이 실행하면 인자로 받은 cmd_line(파일명 + 인자로 이루어진 문자열)이 없어짐
    char *cmd_line_copy = palloc_get_page(PAL_ZERO);
    if(cmd_line_copy == NULL) return -1;

    // 할당받은 공간에 cmd_line(파일명) 넣기
    strlcpy (cmd_line_copy, cmd_line, PGSIZE);

    // process_exec(cmd_line_copy) 실행 및 반환값이 -1이면(오류 발생 시) 강제 종료
    if (process_exec(cmd_line_copy) == -1) exit(-1);

    NOT_REACHED(); 
}
int wait (tid_t pid) { 

	return process_wait(pid);
}

bool create (const char *file, unsigned initial_size) { 
	check_addr_byte((char *)file);

    lock_acquire(&filesys_lock);

    bool success = filesys_create(file, initial_size);

    lock_release(&filesys_lock);

	return success;
}

bool remove (const char *file) { 
	check_addr_byte((char *)file);	

    lock_acquire(&filesys_lock);

    bool success = filesys_remove(file);

    lock_release(&filesys_lock);
	return success; 
}

int open (const char *file) { 
	check_addr_byte((char *)file);
    struct thread *curr = thread_current();

    lock_acquire(&filesys_lock);
    struct file *f = filesys_open (file);

    if(f == NULL){
        lock_release(&filesys_lock);
        return -1;
    } 

    int allocate_fd = -1;

    // 비어있는 슬롯 찾기
    for (int fd = 2; fd < FD_MAX; fd++) {
        struct file *file = curr->fdt[fd];   
			if (file == NULL) {            
				curr->fdt[fd] = f;
                curr->next_fd = fd + 1;
                allocate_fd = fd;
                break;
            }
    }    

    // 비어 있는 슬롯이 없다면 file 닫기
    if(allocate_fd == -1) file_close(f);

    lock_release(&filesys_lock);
    return allocate_fd;

}

int filesize (int fd) { 
    struct thread * curr = thread_current();

    if ( fd < 2 || fd >= FD_MAX || curr->fdt[fd] == NULL) return -1;

    lock_acquire(&filesys_lock);

    int file_len = file_length(curr->fdt[fd]);

    lock_release(&filesys_lock);
    
    return file_len; 
}

int read (int fd, void *buffer, unsigned size) { 
	check_addr_page((void*)buffer, size, false);
	
    struct thread *curr = thread_current();

    

    if (size == 0) return 0;

    if(fd==0){
        // buffer는 void* 이므로 byte 단위로 접근을 위해 형변환
        char *char_buffer = (char *)buffer;
        for(int i = 0; i < size; i++){
            // loop를 돌며 size만큼 키보드에서 입력을 받아오며(input_getc) buffer배열에 넣기
            char_buffer[i] = input_getc();
        }
        // size만큼 작성했으니 size return
        return size;
    }

    // 잘못된 접근
    else if(fd < 2 || fd >= FD_MAX) return -1;
    
    else{
        struct file *file = curr->fdt[fd];

        if (file == NULL) return -1;

        lock_acquire(&filesys_lock);
        
        int read_size = file_read(file, buffer, size);

        lock_release(&filesys_lock);
        return read_size;
    }
    return -1; 
}

void seek (int fd, unsigned position) {
    if (fd < 2 || fd >= FD_MAX) return;

    struct thread *curr = thread_current();
    struct file *file = curr->fdt[fd];

    if(file == NULL) return;

    lock_acquire(&filesys_lock);
    file_seek(file, position);
    lock_release(&filesys_lock);

    return;
}

unsigned tell (int fd) { 
    if (fd < 2 || fd >= FD_MAX) return -1;

    struct thread *curr = thread_current();
    struct file *file = curr->fdt[fd];

    if(file == NULL) return;

    lock_acquire(&filesys_lock);
    off_t offset = file_tell(file);
    lock_release(&filesys_lock);


    return (unsigned)offset; 
}

void close (int fd) {
    if (fd < 0 || fd == 0 || fd == 1 || fd >= FD_MAX) return;

    struct thread *curr = thread_current();

    if(curr->fdt[fd] == NULL) return;

    lock_acquire(&filesys_lock);
    file_close(curr->fdt[fd]);
    curr->fdt[fd] = NULL;

    lock_release(&filesys_lock);

    return;
}
