#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

struct lock;

/* 파일 시스템 호출을 보호하는 공용 락. fd · fork · rox. */
extern struct lock filesys_lock;

void syscall_init (void);

#endif /* userprog/syscall.h */
