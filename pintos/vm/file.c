/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "include/threads/vaddr.h"
#include "threads/synch.h" // 락 구조체를 알기 위해 필요

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
bool file_lazy_load (struct page *page, void *aux);

extern struct lock filesys_lock; // syscall.c에 있는 락을 가져옴 

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;

	struct vm_entry *aux = page->uninit.aux;
	
	file_page->file = aux->file;
	file_page->offset = aux->offset;
	file_page->read_bytes = aux->read_bytes;
	file_page->zero_bytes = aux->zero_bytes;

	free(aux);

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;

	// 페이지가 물리 메모리에 적재되어 있고, 내용이 수정되었다면 파일에 기록
    if (page->frame != NULL && pml4_is_dirty(thread_current()->pml4, page->va)) {
        file_write_at(file_page->file, page->va, file_page->read_bytes, file_page->offset);
        pml4_set_dirty(thread_current()->pml4, page->va, false);
    }

    // pt에서 매핑 제거
    if (pml4_get_page(thread_current()->pml4, page->va) != NULL) {
        pml4_clear_page(thread_current()->pml4, page->va);
    }

	// 파일 닫기
    if (file_page->file != NULL) {
        file_close(file_page->file);
    }
	return;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {
	
	void *start_addr = addr;

	for(void *i = addr ; i < addr + length; i += PGSIZE) if (spt_find_page(&thread_current()->spt, i) != NULL) return NULL;
	
	// 파일 복제
	struct file *f = file_reopen(file);
    if (f == NULL) return NULL;

	// 파일의 크기를 가져옴
    size_t file_len = file_length(f);

	// 파일의 크기보다 오프셋 값이 큰 경우 예외처리
    if (offset >= file_len) {
        file_close(f);
        return NULL;
    }

    // 매핑할 전체 길이를 PGSIZE(4096) 배수로 맞춤
    size_t page_aligned_length = length % PGSIZE == 0 ? length : (length / PGSIZE + 1) * PGSIZE;

	// 읽을 byte 수
    size_t read_bytes = file_len > offset ? file_len - offset : 0;
    if (read_bytes > length) read_bytes = length;

	// 0으로 채울 byte 수 (PGSIZE - 읽을 byte 수)
    size_t zero_bytes = page_aligned_length - read_bytes;

    while (read_bytes > 0 || zero_bytes > 0) {
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        struct vm_entry *aux = malloc(sizeof(struct vm_entry));
        if (aux == NULL) {
            free(aux);
            return NULL;
        }

        aux->file = f;
        aux->offset = offset;
        aux->read_bytes = page_read_bytes;
        aux->zero_bytes = page_zero_bytes;

		// 가상 페이지 생성 및 spt에 등록
        if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, file_lazy_load, aux)) {
            free(aux);
            return NULL;
        }

        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        addr += PGSIZE;
        offset += page_read_bytes;
    }

    return start_addr;
}

/* Do the munmap */
// addr부터 시작하여 spt_find_page로 페이지를 찾고, 
// 페이지가 존재하면 destroy(page) (또는 spt_remove_page)를 호출해 제거한 뒤 
// addr += PGSIZE를 하며 반복합니다.
void
do_munmap (void *addr) {
	struct thread *curr = thread_current();
	// for(int i = addr; i; i+= PGSIZE){
	// 	struct page *page = spt_find_page(&curr->spt, i);
	// 	if (page != NULL) destroy(page);
	// }

	while(1){
		struct page *page = spt_find_page (&curr->spt, addr);
		
		if(page == NULL || page_get_type(page) != VM_FILE) break; 
		
		spt_remove_page(&curr->spt, page);
		
		addr += PGSIZE;
	}
}


bool file_lazy_load (struct page *page, void *aux) {
    ASSERT(page->frame != NULL);

    // printf("file_lazy_load called! page: %p, aux: %p\n", page, aux);
    struct file_page *file_page = &page->file;
    struct vm_entry *file_aux = (struct vm_entry *)aux;

    // aux에 담겨있던 정보를 page->file에 세팅
    file_page->file = file_aux->file;
    file_page->offset = file_aux->offset;
    file_page->read_bytes = file_aux->read_bytes;
    file_page->zero_bytes = file_aux->zero_bytes;

    // lock_acquire(&filesys_lock);
    // 실제 물리 메모리(kva)에 파일 내용 읽기
    int real_read_byte = file_read_at (file_page->file, page->frame->kva, file_page->read_bytes, file_page->offset);

    // lock_release(&filesys_lock);

    if(real_read_byte != (int)file_page->read_bytes){
        // printf("file_lazy_load read failed! actual: %d, expected: %d\n", real_read_byte, file_page->read_bytes);
        free(file_aux);
        return false;
    }

    // if (file_read_at (file_page->file, page->frame->kva, file_page->read_bytes, file_page->offset) != (int)file_page->read_bytes) {
    //     free(file_aux);
    //     return false;
    // }

    // 남은 공간은 0으로 채우기
    memset (page->frame->kva + file_page->read_bytes, 0, file_page->zero_bytes);

    free(file_aux); // 동적 할당한 aux 해제
    return true;
}