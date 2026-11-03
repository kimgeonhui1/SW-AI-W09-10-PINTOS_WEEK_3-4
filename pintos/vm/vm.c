/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"

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
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */		
		/* TODO: Insert the page into the spt. */
		struct page* page;
		page = malloc(sizeof(struct page));

		switch(VM_TYPE(type)){
			case VM_ANON:
				uninit_new(page, upage, init, type, aux, anon_initializer);
				break;
			case VM_UNINIT:
				uninit_new(page, upage, init, type, aux, file_backed_initializer);
				break;
			default:
				free(page);
				goto err;
		}

		page->writable = writable;
		/* spt insert */
		if(!spt_insert_page(spt, page)){
			free(page);
			goto err;
		}else{
			return true;
		}
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
	page_s.va = pg_round_down(va); /* va 할당 */
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
	frame = malloc(sizeof(struct frame));
	/* TODO: Fill this function. */
	// user pool에서 할당받기
	// 멤버 초기화하기 및 할당받은 page 0으로 초기화
	frame->kva = palloc_get_page(PAL_USER);
	/* frame->page 멤버 초기화(NULL) */
	frame->page = NULL;

	memset (frame->kva, 0, PGSIZE);

	if(frame->kva == NULL) PANIC("todo");

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
		/* TODO: vm_alloc_page(VM_ANON|VM_MARKER_0) 호출 = 익명 페이지, 스택용 페이지 둘 중 하나의 페이지를 할당 받는다 */
		/* 
			할당할 때 addr을 PGSIZE의 배수인 페이지 시작 주소로 내림 
			→ 폴트 주소를 페이지의 끝 주소로 이동시켜 등록 (pg_round_down함수 사용)
		*/
		if(!vm_alloc_page(VM_ANON|VM_MARKER_0, pg_round_down(addr), 1)){
			return;
		}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
		bool user, bool write, bool not_present) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	void *fault_addr = addr;
	/* 
		Case1) addr가 잘못된 접근의 fault인지? 
			→ 사용자 주소 공간이 아니다?
	*/
	/* ✔︎ 잘못된 접근이라면 프로세스 종료! (Gitbook 참고) */
	if(fault_addr == NULL || is_kernel_vaddr(fault_addr)) return false;
	/* 
		Case2) not_present = 물리 페이지에 할당되지 않은 정상적인 페이지 폴트인가? / 권한 위반(not_present==1)인가?
			- not_present true → 물리 페이지 할당(vm_do_claim_page)
			- not_present false → 프로세스 종료(exit(-1))
		내용을 준비하면 복구가 가능한 fault인지? = 정상적인 폴트인가?
	*/
	if(not_present){
		page = spt_find_page(spt, fault_addr);

		if(page == NULL){
			/* 
				stack확장이 필요한 유효한 상황인지 판별
					- 사용자 모드에서 접근한 rsp인지? 커널 모드에서 접근한 rsp인지?
						→ user == 1(사용자 모드) user == 0(커널 모드)
							1인 경우 f->rsp 사용자 스택의 끝 주소 
							0인 경우 f->rsp가 사용자 스택의 주소라는 보장이 없음. 스레드의 별도로 저장하는 로직 필요
					- case1) 접근 주소와 사용자 스택 포인터의 관계를 보고 스택 접근인지 판단한다. PUSH로 인해 사용자 rsp보다 8바이트 아래에서 폴트가 발생하는 경우도 고려한다.
					- case2) 사용자 모드에서 커널 모드로 넘어가기 직전에 사용자 스택(사용자 rsp값)을 저장하는 로직을 넣어야 한다.(Gitbook에서 struct thread에 저장하라고 명시)
				→ 유효한 상황인 경우 vm_stack_growth() 호출
			*/
			if(user == 1){
				if((uint64_t)f->rsp - 8 <= (uint64_t)fault_addr && (uint64_t)fault_addr < USER_STACK && (USER_STACK - (uint64_t)fault_addr) <= (1<<20)){
					vm_stack_growth(fault_addr);
					page = spt_find_page(spt, fault_addr);
				}	
			}else{
				if(thread_current()->tmp_user_stack - 8 <= (uint64_t)fault_addr && (uint64_t)fault_addr < USER_STACK && (USER_STACK - (uint64_t)fault_addr) <= (1<<20)){
					vm_stack_growth(fault_addr);
					page = spt_find_page(spt, fault_addr);
				}
			}
			if(page == NULL) return false;
		}
		/* 쓰기 권한이 허용된 페이지인지? */
		if(write && page->writable == 0){
			return false;
		}
	}else{
		return false;
	}
	
	return vm_do_claim_page (page);
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
vm_claim_page (void *va) {
	struct page *page = NULL;
	struct thread *curr = thread_current ();

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
	// 현재 스레드의 pml4에서 va-> pa로 매핑하는것 필요
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
	if(!hash_init(&spt->spt_hash, page_hash, page_less, NULL)) return;
}

/* Copy supplemental page table from src to dst */
/* 자식이 부모의 실행 문맥을 상속받을 때의 fork()에서 사용되는 함수 */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst, struct supplemental_page_table *src) {
	/* 복사 내용을 저장할 spt 초기화 */
	supplemental_page_table_init(dst);
	struct hash* src_hash = &src->spt_hash;
	/* 
		TODO: 원본 보조 테이블(src)의 모든 페이지 순회
		→ 순회를 어떻게 하지?
		→ 1) struct hash_iterator src_ite(원본 iterator) 선언
	*/
	// for(int i=0; i<src_hash->bucket_cnt; i++){
	// 	struct list *bucket = &src_hash->buckets[i];
	// 	struct list_elem *e, *next_e;
		
	// 	for(e = list_begin(bucket); e != list_end(bucket); e = next_e){
	// 		/* TODO: 대상 테이블(dst)에 각 항목을 정확히 복사 */
	// 		next_e = list_next(e);
	// 	}
	// }
	/* hash_iterator를 사용하여 반복문 한 번으로 직관적인 코드를 구현 */
	struct hash_iterator i;
	hash_first(&i, src_hash); /* 부모 해쉬 테이블의 첫 번째 해쉬 */
	/* 부모 해쉬 테이블 전체 순회 */
	while(hash_next(&i)){
		struct page* src_page = hash_entry(hash_cur(&i), struct page, hash_elem); /* 순회 중인 해쉬안에 있는 page 구조체 포인터 추출(hash_entry() 사용) */
			
		/* 원본 spt의 테이블 유형 = 미초기화 페이지일 경우 미초기화 페이지로 할당 */
		if(src_page->operations->type == 0){
			struct vm_entry* dst_aux = malloc(sizeof(struct vm_entry)); /* 복사해서 담을(자식한테) 새로운 vm_entry 구조체 공간 할당 */
			/* 할당 실패 시 false 반환 */
			if(dst_aux == NULL) {
				free(dst_aux);
				return false;
			}
			/* 원본 페이지의 미초기화 보조 정보(aux)가 비어 있지 않을 시 원본의 보조 정보를 자식 보조 정보에 복사(memcpy 사용) */
			if(src_page->uninit.aux != NULL){
				memcpy(dst_aux, src_page->uninit.aux, sizeof(struct vm_entry));
			}else{
				free(dst_aux);
				dst_aux = NULL;
			}
			/* 자식의 spt에 미초기화 페이지 복사 */
			if(!vm_alloc_page_with_initializer(src_page->uninit.type, src_page->va, src_page->writable, src_page->uninit.init, dst_aux)){
				free(dst_aux);
				return false;
			}else{
				/* 미초기화 페이지 할당 및 복사 성공 시 물리 프레임 할당 후. 연결 */
				// if(!vm_claim_page(src_page->va)){
				// 	free(dst_aux);
				// 	return false;
				// }
			}
		/* 원본 spt의 테이블 유형 = 익명 페이지일 경우 익명 페이지로 할당 */
		}else if(src_page->operations->type == 1){
			if(!vm_alloc_page(VM_ANON, src_page->va, src_page->writable)){
				return false;
			}else{
				if(!vm_claim_page(src_page->va)){
					return false;
				}else{ /* 부모의 프레임의 내용을 복사 후 자식의 새로운 프레임에 그대로 저장(붙여넣기) */
					struct page* dst_page = spt_find_page(dst, src_page->va);
					memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
				}
			}
		}
	}

	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	/* 
		hash_clear(struct hash* h, struct hash_action_func* destructor)의 인자로 삭제하고 싶은 spt의 hash를 넘겨주면 모든 해쉬 테이블의 해쉬들을 삭제해준다.
		다만, hash_action_func* 자리에 NULL을 넘겨주면 버킷의 공간은 유지한 채 삭제해준다.
	*/
	hash_clear(&spt->spt_hash, NULL);
}
