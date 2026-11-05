#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
	struct file* file; /* 매핑이 이미 열어 둔 같은 파일 참조 */
	off_t ofs; /* 이 페이지에 대응하는 파일 오프셋 */
	uint32_t read_bytes; /* 이 페이지에서 파일에 대응하는 바이트 수 */
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
