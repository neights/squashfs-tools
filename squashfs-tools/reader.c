/*
 * Create a squashfs filesystem.  This is a highly compressed read only
 * filesystem.
 *
 * Copyright (c) 2021
 * Phillip Lougher <phillip@squashfs.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * reader.c
 */


#define TRUE 1
#define FALSE 0

#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "squashfs_fs.h"
#include "mksquashfs.h"
#include "caches-queues-lists.h"
#include "progressbar.h"
#include "mksquashfs_error.h"
#include "pseudo.h"
#include "sort.h"

static char *pathname_reader(struct dir_ent *dir_ent)
{
	static char *pathname = NULL;
	static int size = ALLOC_SIZE;

	if (dir_ent->nonstandard_pathname)
		return dir_ent->nonstandard_pathname;

	return pathname = _pathname(dir_ent, pathname, &size);
}


static inline int is_fragment(struct inode_info *inode)
{
	off_t file_size = inode->buf.st_size;

	/*
	 * If this block is to be compressed differently to the
	 * fragment compression then it cannot be a fragment
	 */
	if(inode->noF != noF)
		return FALSE;

	return !inode->no_fragments && file_size && (file_size < block_size ||
		(inode->always_use_fragments && file_size & (block_size - 1)));
}


static void put_file_buffer(struct file_buffer *file_buffer)
{
	/*
	 * Decide where to send the file buffer:
	 * - compressible non-fragment blocks go to the deflate threads,
	 * - fragments go to the process fragment threads,
	 * - all others go directly to the main thread
	 */
	if(file_buffer->error) {
		file_buffer->fragment = 0;
		seq_queue_put(to_main, file_buffer);
	} else if (file_buffer->file_size == 0)
		seq_queue_put(to_main, file_buffer);
	else if(file_buffer->fragment)
		queue_put(to_process_frag, file_buffer);
	else
		queue_put(to_deflate, file_buffer);
}


static int seq = 0;
static void reader_read_process(struct dir_ent *dir_ent)
{
	long long bytes = 0;
	struct inode_info *inode = dir_ent->inode;
	struct file_buffer *prev_buffer = NULL, *file_buffer;
	int status, byte, res, child;
	int file = pseudo_exec_file(get_pseudo_file(inode->pseudo_id), &child);

	if(!file) {
		file_buffer = cache_get_nohash(reader_buffer);
		file_buffer->sequence = seq ++;
		goto read_err;
	}

	while(1) {
		file_buffer = cache_get_nohash(reader_buffer);
		file_buffer->sequence = seq ++;
		file_buffer->noD = inode->noD;

		byte = read_bytes(file, file_buffer->data, block_size);
		if(byte == -1)
			goto read_err2;

		file_buffer->size = byte;
		file_buffer->file_size = -1;
		file_buffer->error = FALSE;
		file_buffer->fragment = FALSE;
		bytes += byte;

		if(byte == 0)
			break;

		/*
		 * Update progress bar size.  This is done
		 * on every block rather than waiting for all blocks to be
		 * read incase write_file_process() is running in parallel
		 * with this.  Otherwise the current progress bar position
		 * may get ahead of the progress bar size.
		 */
		progress_bar_size(1);

		if(prev_buffer)
			put_file_buffer(prev_buffer);
		prev_buffer = file_buffer;
	}

	/*
	 * Update inode file size now that the size of the dynamic pseudo file
	 * is known.  This is needed for the -info option.
	 */
	inode->buf.st_size = bytes;

	res = waitpid(child, &status, 0);
	close(file);

	if(res == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		goto read_err;

	if(prev_buffer == NULL)
		prev_buffer = file_buffer;
	else {
		cache_block_put(file_buffer);
		seq --;
	}
	prev_buffer->file_size = bytes;
	prev_buffer->fragment = is_fragment(inode);
	put_file_buffer(prev_buffer);

	return;

read_err2:
	close(file);
read_err:
	if(prev_buffer) {
		cache_block_put(file_buffer);
		seq --;
		file_buffer = prev_buffer;
	}
	file_buffer->error = TRUE;
	put_file_buffer(file_buffer);
}


static void reader_read_file(struct dir_ent *dir_ent)
{
	struct stat *buf = &dir_ent->inode->buf, buf2;
	struct file_buffer *file_buffer;
	int blocks, file, res;
	long long bytes, read_size;
	struct inode_info *inode = dir_ent->inode;

	if(inode->read)
		return;

	inode->read = TRUE;
again:
	bytes = 0;
	read_size = buf->st_size;
	blocks = (read_size + block_size - 1) >> block_log;

	file = open(pathname_reader(dir_ent), O_RDONLY);
	if(file == -1) {
		file_buffer = cache_get_nohash(reader_buffer);
		file_buffer->sequence = seq ++;
		goto read_err2;
	}

	do {
		file_buffer = cache_get_nohash(reader_buffer);
		file_buffer->file_size = read_size;
		file_buffer->sequence = seq ++;
		file_buffer->noD = inode->noD;
		file_buffer->error = FALSE;

		/*
		 * Always try to read block_size bytes from the file rather
		 * than expected bytes (which will be less than the block_size
		 * at the file tail) to check that the file hasn't grown
		 * since being stated.  If it is longer (or shorter) than
		 * expected, then restat, and try again.  Note the special
		 * case where the file is an exact multiple of the block_size
		 * is dealt with later.
		 */
		file_buffer->size = read_bytes(file, file_buffer->data,
			block_size);
		if(file_buffer->size == -1)
			goto read_err;

		bytes += file_buffer->size;

		if(blocks > 1) {
			/* non-tail block should be exactly block_size */
			if(file_buffer->size < block_size)
				goto restat;

			file_buffer->fragment = FALSE;
			put_file_buffer(file_buffer);
		}
	} while(-- blocks > 0);

	/* Overall size including tail should match */
	if(read_size != bytes)
		goto restat;

	if(read_size && read_size % block_size == 0) {
		/*
		 * Special case where we've not tried to read past the end of
		 * the file.  We expect to get EOF, i.e. the file isn't larger
		 * than we expect.
		 */
		char buffer;
		int res;

		res = read_bytes(file, &buffer, 1);
		if(res == -1)
			goto read_err;

		if(res != 0)
			goto restat;
	}

	file_buffer->fragment = is_fragment(inode);
	put_file_buffer(file_buffer);

	close(file);

	return;

restat:
	res = fstat(file, &buf2);
	if(res == -1) {
		ERROR("Cannot stat dir/file %s because %s\n",
			pathname_reader(dir_ent), strerror(errno));
		goto read_err;
	}

	if(read_size != buf2.st_size) {
		close(file);
		memcpy(buf, &buf2, sizeof(struct stat));
		file_buffer->error = 2;
		put_file_buffer(file_buffer);
		goto again;
	}
read_err:
	close(file);
read_err2:
	file_buffer->error = TRUE;
	put_file_buffer(file_buffer);
}


void reader_scan(struct dir_info *dir)
{
	struct dir_ent *dir_ent = dir->list;

	for(; dir_ent; dir_ent = dir_ent->next) {
		struct stat *buf = &dir_ent->inode->buf;
		if(dir_ent->inode->root_entry)
			continue;

		if(IS_PSEUDO_PROCESS(dir_ent->inode)) {
			reader_read_process(dir_ent);
			continue;
		}

		switch(buf->st_mode & S_IFMT) {
			case S_IFREG:
				reader_read_file(dir_ent);
				break;
			case S_IFDIR:
				reader_scan(dir_ent->dir);
				break;
		}
	}
}


void *reader(void *arg)
{
	if(!sorted)
		reader_scan(queue_get(to_reader));
	else {
		int i;
		struct priority_entry *entry;

		queue_get(to_reader);
		for(i = 65535; i >= 0; i--)
			for(entry = priority_list[i]; entry;
							entry = entry->next)
				reader_read_file(entry->dir);
	}

	pthread_exit(NULL);
}
