#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
	struct file *file;		// 파일
	off_t offset;			// 파일의 어디부터 읽을지 오프셋 값
	size_t read_bytes;		// 읽을 공간
	size_t zero_bytes;		// 0으로 채울 공간(읽고 남은 공간)
	bool writable;			// 쓰기가능 여부
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
