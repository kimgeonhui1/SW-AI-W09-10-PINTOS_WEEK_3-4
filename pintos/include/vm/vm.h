#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include <hash.h>

enum vm_type {
	/* page not initialized(페이지를 초기화하지 않았습니다) */
	VM_UNINIT = 0,
	/* page not related to the file, aka anonymous page */
	// 파일과 관련 없는 페이지, 일명 익명 페이지
	VM_ANON = 1,
	/* page that realated to the file (파일과 연결된 페이지)*/
	VM_FILE = 2,
	/* page that hold the page cache, for project 4 */
	// project4에서 사용할 Page Cache 저장 Page
	VM_PAGE_CACHE = 3,

	/* Bit flags to store state(상태 저장용 비트 플래그) */

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. */
	// 저장 정보를 위한 보조 비트 플래그 마커입니다.
	// 값이 int에 맞을 때까지 마커를 더 추가할 수 있습니다.
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* DO NOT EXCEED THIS VALUE.(이 값을 초과하지 마시오) */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* The representation of "page".
 * This is kind of "parent class", which has four "child class"es, which are
 * uninit_page, file_page, anon_page, and page cache (project4).
 * DO NOT REMOVE/MODIFY PREDEFINED MEMBER OF THIS STRUCTURE. */
 // 일종의 부모 클래스로, 4개의 자식 클래스를 가지고 있음 
 // 미리 정의된 멤버들을 제거/수정하지 마시오
struct page {
	const struct page_operations *operations;
	void *va;              /* Address in terms of user space(가상 주소) */
	struct frame *frame;   /* Back reference for frame (이 페이지가 실제로 M.M에 적재되었다면 연결되는 frame 포인터. 없다면 Swap되거나 M.M에 적재되지 않음) */

	/* Your implementation(내가 구현할 곳) */ 
	// hash용 elem
	struct hash_elem hash_elem;
	// 쓰기 여부
	bool write_ok;

	/* Per-type data are binded into the union.
	 * Each function automatically detects the current union */
	// 각 함수는 UNION을 자동 감지
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* The representation of "frame" */
struct frame {
	void *kva;
	struct page *page;
};

/* The function table for page operations.
 * This is one way of implementing "interface" in C.
 * Put the table of "method" into the struct's member, and
 * call it whenever you needed. */
// 페이지 작업을 위함 함수 테이블
// "method"테이블을 구조체 멤버에 넣고 필요할때마다 호출하시오??
struct page_operations {
	bool (*swap_in) (struct page *, void *);	// swap_in (메모리로 적재)
	bool (*swap_out) (struct page *);			// swap_out(디스크로 쫓겨남)
	void (*destroy) (struct page *);			// 해제
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. */
// 현재 프로세스의 메모리 공간 표현. 자율적으로 짤 것
// Page Fault, 자원 관리를 위해 각 페이지에 대한 추가 정보를 들고 있는 Page Table
// 1. Page Fault 발생 시, 해당 주소가 어떤 페이지에 해당하는지 찾아냄
// 2. 프로세스 복사(fork)시 자식 프로세스에게 가상 메모리 정보를 복사함
struct supplemental_page_table {
	struct hash spt_hash;
};

#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst, struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);

#endif  /* VM_VM_H */
