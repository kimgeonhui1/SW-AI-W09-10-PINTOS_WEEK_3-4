/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

static uint64_t page_hash (const struct hash_elem *p_, void *aux UNUSED);
static bool page_less (const struct hash_elem *a_,const struct hash_elem *b_, void *aux UNUSED);

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* 가상 주소값의 해시값을 반환해주는 helper 함수 */
uint64_t
page_hash (const struct hash_elem *p_, void *aux UNUSED){
  const struct page *p = hash_entry (p_, struct page, hash_elem);
  return hash_bytes (&p->va, sizeof p->va);
}

/* page a와 page b를 비교하여 a가 b보다 작다면 true / b가 a보다 작다면 false를 반환해주는 helper 함수 */
bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED){
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);

  return a->va < b->va;
}

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		struct page *page = malloc(sizeof(struct page));
		if(page == NULL) goto err;
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		if (VM_TYPE (type) == VM_ANON) uninit_new (page, upage, init, type, aux, anon_initializer);
        else if (VM_TYPE (type) == VM_FILE) uninit_new (page, upage, init, type, aux, file_backed_initializer);
        else {
            free (page);
            goto err;
        }
		page->writable = writable;
		/* TODO: Insert the page into the spt. */
		if (spt_insert_page (spt, page) == false) {
            free (page);
            goto err;
        }
		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	/* 내용은 없는 빈 껍데기 구조체 page 생성 */
	struct page page_s = {0};
	// struct page *page = &page_s;
	/* TODO: Fill this function. */
	/* page 구조체가 비어 있는데 저렇게 하는게 맞나? */
	// page->va = va; /* va 할당 */
	
	// pg_round_down(페이지에 들어가있는 주소?) -> 페이지의 시작점을 알려줌
	page_s.va = pg_round_down(va);

	struct hash_elem* h_e;
	h_e = hash_find(&spt->spt_hash, &page_s.hash_elem); /* va를 담고 있는 hash_elem을 키값으로 find */
	
	/* spt에 존재한다면 page 변수에 spt에 있는 페이지 할당(hash_entry 사용) */
	if(h_e != NULL){
		return hash_entry(h_e, struct page, hash_elem);
	/* h_e가 NULL이라면 spt에 존재하지 않기 때문에 NULL 반환 */
	}
	return NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
		struct page *page) {
	int succ = false;
	/* TODO: Fill this function. */
	/* 
		hash_insert() 호출 성공 시 NULL 반환
		hash_insert() 호출 실패 시 이미 존재하는 hash_elem 포인터 반환, insert X
	*/
	struct hash_elem* h_e = hash_insert(&spt->spt_hash, &page->hash_elem);
	if(h_e == NULL){
		succ = true;
	}

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */

	void *kva = palloc_get_page (PAL_USER);

    /* 2. 만약 할당 실패(메모리 부족) 시 페이지 교체(Eviction) 수행 */
    if (kva == NULL) {
        // 아직 Swap/Eviction 구현 전이라면 PANIC("todo")을 두거나 vm_evict_frame() 호출
        frame = vm_evict_frame ();
        frame->page = NULL;
        return frame;
    }

    /* 3. frame 구조체 할당 및 초기화 */
    frame = (struct frame *) malloc (sizeof (struct frame));
    frame->kva = kva;
    frame->page = NULL;

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f , void *addr ,
		bool user , bool write , bool not_present ) {
		// not_present = M.M에 적재되지 않은 상태면 true, M.M에 적재(매핑)되어 있으나 Fault가 나면 false
		// user = user 모드로 접근했는지
	struct supplemental_page_table *spt  = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	
	if(addr == NULL || is_kernel_vaddr(addr)) return false;

	if(not_present == true){
		page = spt_find_page (spt, addr);
		if(page == NULL) return false;
	
		if(write == true && page->writable == false) return false;

		return vm_do_claim_page (page);
	}
	return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	struct thread *curr = thread_current();

	/* TODO: Fill this function */
	page = spt_find_page(&curr->spt, va);
	if(page == NULL) return false;

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	struct thread *curr = thread_current ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if(pml4_set_page(curr->pml4, page->va, frame->kva, page->writable) == false){
		frame = NULL;
		palloc_free_page(frame->kva);
		free(frame);
		return false;
	}


	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	/* hash_init()함수를 호출하여 hash 초기화 */
	hash_init(&spt->spt_hash, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}
