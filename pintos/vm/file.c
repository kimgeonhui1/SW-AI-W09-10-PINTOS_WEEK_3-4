/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include <string.h>

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
bool lazy_load_file(struct page *page, void *aux);


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

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	struct thread *curr = thread_current();

	if (page->frame == NULL) return;

	// dirty bit 확인(수정되었는지 확인) 후 수정되었다면 파일에 덮어쓰기
	if (pml4_is_dirty (curr->pml4, page->va)) {
		file_write_at (file_page->file, page->frame->kva, file_page->read_bytes, file_page->offset);
		pml4_set_dirty (curr->pml4, page->va, false);
	}

	// Page Table(pml4)에서 가상 주소 매핑 제거
	pml4_clear_page (curr->pml4, page->va);

	// frame 해제
	palloc_free_page (page->frame->kva);
	free (page->frame);
	page->frame = NULL;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {

    // 파일 복제본 생성 (타입만 struct file *로 수정)
    struct file * f = file_reopen(file);
    if(f == NULL) return NULL;

    // 시작 주소 백업(return 용)
    void *start_addr = addr;

    // 매핑해야 할 페이지 개수(올림 처리)
    int page_count = ((int)length + (int)PGSIZE - 1) / (int)PGSIZE;

    // 몇번째 페이지인지(매핑된 페이지 개수를 넣기 위함)
    int page_index = 0;

    // offset부터 시작하므로 실제 남은 파일 길이는 file_length(f) - offset
    // page_index가 page_count가 될 때까지 반복하며 f_len을 PGSIZE씩 차감
    for(int64_t f_len = file_length(f) - offset; page_index < page_count; f_len -= PGSIZE){
        // 읽어야 할 byte
        int64_t read_byte = 0;
        // 0으로 채워야 할 byte
        int64_t zero_byte = 0;

        if(f_len > (int64_t)PGSIZE){
            if((int64_t)length > (int64_t)PGSIZE){
                read_byte = PGSIZE;
                zero_byte = 0;
            }else{
                read_byte = (int64_t)length;
                zero_byte = (int64_t)PGSIZE - (int64_t)length;
            }
        }else if(f_len > 0){ 
            // 파일의 크기가 PGSIZE 이하일 때 (만약 읽어야 할 크기가 파일의 크기보다 더 작다면 읽어야 할 크기만큼만 읽음)
            read_byte = (int64_t)length < f_len ? (int64_t)length : f_len;
            zero_byte = (int64_t)PGSIZE - read_byte;
        }else{
            // f_len이 0 이하가 된 경우 (파일 끝을 넘어선 페이지)
            read_byte = 0;
            zero_byte = PGSIZE;
        }

        struct file_page *aux = malloc(sizeof(struct file_page));
        if (aux == NULL) return NULL;

        aux->file = f;
        aux->offset = offset;
        aux->read_bytes = read_byte;
        aux->zero_bytes = zero_byte;
        if(page_index == 0){
            aux->page_cnt = page_count;
        }else{
            aux->page_cnt = 0;
        }

		// printf ("[do_mmap] addr=%p flen=%d ofs=%d read=%d zero=%d cnt=%d\n", addr, (int) file_length (f), (int) offset, (int) read_byte, (int) zero_byte, aux->page_cnt);

        if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_file, aux)) {
            free(aux);
            return NULL;
        }

        offset += read_byte;
        addr += PGSIZE;      // 다음 가상 페이지 주소로 이동
        length -= read_byte; // 남은 매핑 길이 차감
        page_index += 1;
    }
    return start_addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct thread *curr = thread_current();
	// addr로 첫 번째 페이지를 찾은 뒤, page_cnt만큼 반복문을 돌며 spt_remove_page를 호출
	struct page * first_page = spt_find_page(&curr->spt, addr);

	if (first_page == NULL || page_get_type(first_page) != VM_FILE) return;

	int page_count = 0;
	struct file *reopen_file = NULL;

	if (first_page->operations->type == VM_UNINIT) {
	    struct file_page *aux = (struct file_page *)first_page->uninit.aux;
    	page_count = aux->page_cnt;
		reopen_file = aux->file;
	} else{
    	page_count = first_page->file.page_cnt;
		reopen_file = first_page->file.file;
	}

	if(page_count < 1) return;

	for(int i = 0 ; i < page_count ; i++){
		struct page *p = spt_find_page(&curr->spt, addr + (i * PGSIZE));

    	if (p != NULL) spt_remove_page(&curr->spt, p);
	}

	// reopen 했던 파일 닫기
	if (reopen_file != NULL) file_close(reopen_file);

}

bool lazy_load_file(struct page *page, void *aux){
	struct file_page *vme = (struct file_page *)aux;

	if(file_read_at(vme->file, page->frame->kva, vme->read_bytes, vme->offset)!= (int)vme->read_bytes){
		free(vme);
		return false;
	}

	memset(page->frame->kva + vme->read_bytes, 0, vme->zero_bytes);

	// printf ("[lazy_load_file] va=%p kva=%p ofs=%d read=%d zero=%d\n",
    //     page->va, page->frame->kva, (int) vme->offset,
    //     (int) vme->read_bytes, (int) vme->zero_bytes);
	// printf ("[lazy_load_file] data: %.40s\n", (char *) page->frame->kva);

	page->file.file = vme->file;
    page->file.offset = vme->offset;
    page->file.read_bytes = vme->read_bytes;
    page->file.zero_bytes = vme->zero_bytes;
    page->file.page_cnt = vme->page_cnt;

	free(vme);
	
	return true;
}
