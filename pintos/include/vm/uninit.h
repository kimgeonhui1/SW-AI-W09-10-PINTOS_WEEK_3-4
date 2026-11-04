#ifndef VM_UNINIT_H
#define VM_UNINIT_H
#include "vm/vm.h"

struct page;
enum vm_type;

// lazy load segment
typedef bool vm_initializer (struct page *, void *aux);

/* Uninitlialized page. The type for implementing the
 * "Lazy loading". */
// lazy loading을 위한 초기화되지 않은 페이지입니다
struct uninit_page {
	/* Initiate the contets of the page */
	// 데이터를 읽어서 채우는 함수 포인터
	vm_initializer *init;
	// 익명 페이지가 될지 파일 기반 페이지가 될지 선택
	enum vm_type type;
	// init 함수가 실행될때 넘겨주는 추가 정보
	void *aux;
	/* Initiate the struct page and maps the pa to the va */
	// page fault 발생 시 vm_uninit상태에서 type에 맞게 바꿔줌 (anon_initializer 또는 file_backed_initializer가 여기 들어감)
	bool (*page_initializer) (struct page *, enum vm_type, void *kva);
};

void uninit_new (struct page *page, void *va, vm_initializer *init,
		enum vm_type type, void *aux,
		bool (*initializer)(struct page *, enum vm_type, void *kva));
#endif
