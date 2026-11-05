/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

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
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;

	/* TODO: 되쓰기 */
	bool dirty = pml4_is_dirty(thread_current()->pml4, page->va);
	if(dirty){
		file_write_at(page->file.file, page->frame->kva, page->file.read_bytes, page->file.ofs);
	}
	/* TODO: 페이지 자원 정리 */
	pml4_clear_page(thread_current()->pml4, page->va);
	palloc_free_page(page->frame->kva);
	free(page->frame);
}

/* mmap의 load를 담당할 콜백 함수(실제로 파일을 물리 메모리에 로드) */
static bool
lazy_load_file(struct page* page, void* aux) {
	struct vm_entry* vm_entry = aux;

	/* 파일 위치를 ofs위치로 이동 → ofs위치부터 읽을 바이트를 계산 */
	file_seek(vm_entry->file, vm_entry->ofs);

	if(file_read(vm_entry->file, page->frame->kva, vm_entry->read_bytes) != (int)vm_entry->read_bytes){
		return false;
	}

	memset((page->frame->kva+vm_entry->read_bytes), 0, vm_entry->zero_bytes);

	page->file.file = vm_entry->file;
	page->file.ofs = vm_entry->ofs;
	page->file.read_bytes = vm_entry->read_bytes;

	free(vm_entry);

	return true;
}


/* Do the mmap */
/*
	파일의 offset부터 length만큼에 해당하는 가상 페이지를 addr부터 SPT에 등록하는 함수
*/
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	/* 첫 설계 기준은 addr를 반환하는 순간 요청한 범위의 모든 페이지가 SPT에 지연 로딩 상태로 등록돼 있어야 한다 */
	
	/*
		잘못된 접근 시 NULL로 반환할 조건(Gitbook 명시)
		1. addr의 시작 주소가 페이지의 경계에 걸려 있지 않을 경우
		2. 사용자 주소 공간에 있지 않은 경우
		3. 시작 주소부터 length만큼 공간이 커널 영역을 넘을 경우 → KERN_BASE 한참 아래 공간은 채워지다가 KERN_BASE 근처에서 페이지를 할당하다가 커널을 넘을 경우 존재
	*/	
	if(pg_ofs(addr) != 0 || !is_user_vaddr(addr) || KERN_BASE - (uint64_t)addr < length){
		return NULL;
	}

	/*
		원래 FD가 닫혀도 매핑 해제 시까지 파일을 사용할 수 있도록 파일 객체가 해제되지 않아야 한다.
		그렇기 때문에 file_reopen()함수를 사용해 file객체를 복사해 copy_file에 담는다.
	*/
	struct file* copy_file = file_reopen(file);
	/* file_reopen()실패 시 file_close 및 NULL 반환 */
	if(copy_file == NULL){
		return NULL;
	}

	/* spt의 mapped_info_list에 시작 주소와 끝 주소를 저장해서 관리 */
	struct mapped_info* m_info = malloc(sizeof(struct mapped_info));
	if(m_info == NULL){
		file_close(copy_file);
		return NULL;
	}
	/* spt의 mapped_info에 매핑의 시작 주소와 끝 주소 저장 */
	m_info->start_addr = addr;
	void* start_addr = addr;
	struct supplemental_page_table* curr_spt = &thread_current()->spt;
	off_t file_size = file_length(file); /* file 전체 사이즈를 가져오기 */
	/* file_size가 0이라면 NULL 반환 */
	if(file_size == 0){
		file_close(copy_file);
		free(m_info);
		return NULL;
	}
	while(length > 0){
		size_t page_read_bytes = length < PGSIZE ? length : PGSIZE;
		if(offset > file_size){
			page_read_bytes = 0;
		}else if(page_read_bytes > file_size-offset){
			page_read_bytes = file_size-offset;
		}
		size_t page_zero_bytes = PGSIZE - page_read_bytes;
		/* SPT에서 등록했던 페이지 해제 */
		if(spt_find_page(&thread_current()->spt, addr) != NULL){
			void* temp_addr = start_addr;
			while(temp_addr != addr){
				struct page* page = spt_find_page(&thread_current()->spt, temp_addr);
				if(page != NULL){
					spt_remove_page(&thread_current()->spt, page);
				}
				temp_addr += PGSIZE;
			}
			return NULL;
		}

		/* aux에 vm_entry 구조체 내용 할당 */
		struct vm_entry* vm_entry;
		/* vm_entry 공간 확보 */
		vm_entry = malloc(sizeof(struct vm_entry));
		/* 공간 확보 실패 시 전에 등록했던 addr에 해당된 페이지들 삭제 및 해제 */
		if(vm_entry == NULL){
			void *temp_addr = start_addr;
    		while (temp_addr != addr) {
    		    struct page *page = spt_find_page(curr_spt, temp_addr);
    		    if (page != NULL) {
    		        spt_remove_page(curr_spt, page);
    		    }
    		    temp_addr += PGSIZE;
    		}
			return NULL;
		}

		void *aux = NULL;

		vm_entry->file = copy_file; 
		vm_entry->ofs = offset; 
		vm_entry->read_bytes = page_read_bytes;
		vm_entry->zero_bytes = page_zero_bytes;
		aux = vm_entry;

		if(!(vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_file, aux))){
			/* SPT에서 등록했던 페이지 해제 */
			void* temp_addr = start_addr;
			while(temp_addr != addr){
				struct page* page = spt_find_page(&thread_current()->spt, temp_addr);
				if(page != NULL){
					spt_remove_page(&thread_current()->spt, page);
				}
				temp_addr += PGSIZE;
			}
			free(vm_entry);
			return NULL;			
		}
		page_read_bytes = length < PGSIZE ? length : PGSIZE;
		length -= page_read_bytes;
		offset += page_read_bytes;
		addr += PGSIZE;
	}
	m_info->end_addr = addr;
	m_info->file = copy_file;
	list_push_back(&thread_current()->spt.mapped_info_list, &m_info->list_elem);

	return start_addr;	
}

/* Do the munmap */
/* 
	mmap()에서 반환한 addr주소로 매핑한 전체 페이지 해제
	1. 파일 되쓰기
		- 매핑을 통해 발생된 dirty 페이지는 파일의 원래 위치에 되쓴다
		- 읽기만 한 clean페이지는 되쓰지 않는다.
		- 마지막 페이지에서 파일 끝을 넘어가는 영역은 되쓰기 범위에서 제외. 쉽게 말해 0으로 채워진 영역은 해제할 때 버리면 된다.
	2. 주소 공간 정리
		- 이미 로드된 페이지는 페이지 테이블에서 매핑을 제거, 해제 후 기존 주소로 접근할 수 없게 한다
		- 매핑에 속한 모든 페이지를 SPT에서 제거.
		- 접근한 적 없는 페이지는 해제하기 위해 새로 load X
	3. 파일 참조 수명
		- 각 매핑은 file_reopen()으로 얻은 독립적인 파일 참조 사용
		- 원래 FD를 close(), remove()해도 기존 매핑 유지
		- 매핑에 필요한 마지막 되쓰기까지 파일 참조 유효하게 유지, 해제 과정에서 해당 참조 정리
*/
void
do_munmap (void* addr) {
	/* TODO: addr가 속해 있는 page가 SPT에 있는지 확인 */
	struct supplemental_page_table* curr_spt = &thread_current()->spt;
	void* end_addr = NULL;
	struct list* mapped_info_list = &curr_spt->mapped_info_list;
	struct mapped_info* found_m_info;
	struct list_elem* s = list_begin(mapped_info_list); 
	struct list_elem* e = list_end(mapped_info_list);
	while(s != e){
		struct mapped_info* m_info = list_entry(s, struct mapped_info, list_elem);
		if(addr == m_info->start_addr){
			end_addr = m_info->end_addr;
			found_m_info = m_info;
		}
		s = list_next(s);
	}
	if(end_addr == NULL){
		return;
	}
	struct page* curr_pg;
	void* start_addr = addr;
	while(start_addr != end_addr){
		curr_pg = spt_find_page(curr_spt, start_addr);
		if(curr_pg != NULL){
			/* TODO: curr_pg 해제 */
			spt_remove_page(curr_spt, curr_pg);
		}
		/* TODO: PGSIZE만큼 이동 */
		start_addr += PGSIZE;
	}

	/* 해제한 매핑 기록을 제거 */
	list_remove(&found_m_info->list_elem);
	file_close(found_m_info->file);
	free(found_m_info);
	/* TODO: 파일 되쓰기 */
	/* TODO: 주소 공간 정리 */
}
