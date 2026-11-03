#include "filesys/file.h"
#include <debug.h>
#include "filesys/inode.h"
#include "threads/malloc.h"

/* An open file. */
struct file {
	struct inode *inode;        /* 디스크의 실제 파일 데이터(원본)를 가리키는 포인터 */
	off_t pos;                  /* 현재 파일의 어디까지 읽고 썼는지를 나타내는 커서 */
	bool deny_write;            /* 쓰기 금지 모드인지 확인(true면 쓰기 금지) */
};

/* Opens a file for the given INODE, of which it takes ownership,
 * and returns the new file.  Returns a null pointer if an
 * allocation fails or if INODE is null. */
// 원본만 있을 때 사용
// 디스크의 원본 파일(inode)을 받아서 새로운 struct file 객체를 메모리에 할당(calloc)하고 초기화합니다. 처음 열었으니 커서(pos)는 0부터 시작합니다.
// 메모리 할당에 실패하거나 inode가 없으면 NULL을 반환
struct file *
file_open (struct inode *inode) {
	struct file *file = calloc (1, sizeof *file);
	if (inode != NULL && file != NULL) {
		file->inode = inode;
		file->pos = 0;
		file->deny_write = false;
		return file;
	} else {
		inode_close (inode);
		free (file);
		return NULL;
	}
}

/* Opens and returns a new file for the same inode as FILE.
 * Returns a null pointer if unsuccessful. */
// 이미 만들어진 struct file(복사본)이 있을 때 사용
// 만들어진 복사본을 이용하여 struct file 복사본을 하나 더 만듦
struct file *
file_reopen (struct file *file) {
	return file_open (inode_reopen (file->inode));
}

/* Duplicate the file object including attributes and returns a new file for the
 * same inode as FILE. Returns a null pointer if unsuccessful. */
// file_reopen처럼 같은 원본을 가리키는 새 struct file을 만들지만, 현재 읽던 위치(pos)와 쓰기 금지 상태(deny_write)까지 똑같이 복사합니다.
struct file *
file_duplicate (struct file *file) {
	struct file *nfile = file_open (inode_reopen (file->inode));
	if (nfile) {
		nfile->pos = file->pos;
		if (file->deny_write)
			file_deny_write (nfile);
	}
	return nfile;
}

/* Closes FILE. */
// 열려 있는 file 객체를 닫고 메모리(free)를 해제합니다. 만약 이 파일이 쓰기 금지(deny_write)를 걸어두었다면 그것도 풀어줍니다.
void
file_close (struct file *file) {
	if (file != NULL) {
		file_allow_write (file);
		inode_close (file->inode);
		free (file);
	}
}

/* Returns the inode encapsulated by FILE. */
// 이 file 객체가 가리키고 있는 원본 inode 포인터를 그대로 반환합니다.
// inode 포인터 : 디스크의 실제 파일 데이터(원본)를 가리키는 포인터
struct inode *
file_get_inode (struct file *file) {
	return file->inode;
}

/* Reads SIZE bytes from FILE into BUFFER,
 * starting at the file's current position.
 * Returns the number of bytes actually read,
 * which may be less than SIZE if end of file is reached.
 * Advances FILE's position by the number of bytes read. */
// 파일의 현재 커서 위치(file->pos)에서부터 size 바이트만큼 읽음(read).
// 작업이 끝나면 실제로 읽거나 쓴 바이트 수만큼 file->pos(커서)가 앞으로 이동
off_t
file_read (struct file *file, void *buffer, off_t size) {
	off_t bytes_read = inode_read_at (file->inode, buffer, size, file->pos);
	file->pos += bytes_read;
	return bytes_read;
}

/* Reads SIZE bytes from FILE into BUFFER,
 * starting at offset FILE_OFS in the file.
 * Returns the number of bytes actually read,
 * which may be less than SIZE if end of file is reached.
 * The file's current position is unaffected. */
// 파일의 현재 커서(file->pos)를 무시하고, 인자로 직접 지정한 file_ofs(오프셋) 위치에서부터 size 바이트만큼 읽음. 
// 작업이 끝나도 file->pos는 전혀 변하지 않습니다.
off_t
file_read_at (struct file *file, void *buffer, off_t size, off_t file_ofs) {
	return inode_read_at (file->inode, buffer, size, file_ofs);
}

/* Writes SIZE bytes from BUFFER into FILE,
 * starting at the file's current position.
 * Returns the number of bytes actually written,
 * which may be less than SIZE if end of file is reached.
 * (Normally we'd grow the file in that case, but file growth is
 * not yet implemented.)
 * Advances FILE's position by the number of bytes read. */
// 파일의 현재 커서 위치(file->pos)에서부터 size 바이트만큼 씀(write)
// // 작업이 끝나면 실제로 읽거나 쓴 바이트 수만큼 file->pos(커서)가 앞으로 이동

off_t
file_write (struct file *file, const void *buffer, off_t size) {
	off_t bytes_written = inode_write_at (file->inode, buffer, size, file->pos);
	file->pos += bytes_written;
	return bytes_written;
}

/* Writes SIZE bytes from BUFFER into FILE,
 * starting at offset FILE_OFS in the file.
 * Returns the number of bytes actually written,
 * which may be less than SIZE if end of file is reached.
 * (Normally we'd grow the file in that case, but file growth is
 * not yet implemented.)
 * The file's current position is unaffected. */
// // 파일의 현재 커서(file->pos)를 무시하고, 인자로 직접 지정한 file_ofs(오프셋) 위치에서부터 size 바이트만큼 씀. 
// 작업이 끝나도 file->pos는 전혀 변하지 않습니다.
off_t
file_write_at (struct file *file, const void *buffer, off_t size,
		off_t file_ofs) {
	return inode_write_at (file->inode, buffer, size, file_ofs);
}

/* Prevents write operations on FILE's underlying inode
 * until file_allow_write() is called or FILE is closed. */
// 이 파일의 원본(inode)에 다른 누군가가 데이터를 쓰지 못하도록 막습니다
void
file_deny_write (struct file *file) {
	ASSERT (file != NULL);
	if (!file->deny_write) {
		file->deny_write = true;
		inode_deny_write (file->inode);
	}
}

/* Re-enables write operations on FILE's underlying inode.
 * (Writes might still be denied by some other file that has the
 * same inode open.) */
// file_deny_write로 걸어두었던 쓰기 금지를 해제합니다(쓰기 가능)
void
file_allow_write (struct file *file) {
	ASSERT (file != NULL);
	if (file->deny_write) {
		file->deny_write = false;
		inode_allow_write (file->inode);
	}
}

/* Returns the size of FILE in bytes. */
// file 길이 반환
off_t
file_length (struct file *file) {
	ASSERT (file != NULL);
	return inode_length (file->inode);
}

/* Sets the current position in FILE to NEW_POS bytes from the
 * start of the file. */
// 파일의 커서 위치(file->pos)를 new_pos 위치로 강제 이동시킵니다
void
file_seek (struct file *file, off_t new_pos) {
	ASSERT (file != NULL);
	ASSERT (new_pos >= 0);
	file->pos = new_pos;
}

/* Returns the current position in FILE as a byte offset from the
 * start of the file. */
// 현재 파일의 커서 위치(file->pos)가 파일 시작점으로부터 몇 바이트 떨어져 있는지 반환
off_t
file_tell (struct file *file) {
	ASSERT (file != NULL);
	return file->pos;
}
