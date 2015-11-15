#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define CHUNKSIZE 64 * 1024
#define BUSDIV 16
#define BUFSIZE 100

typedef struct BlockHeader {
	size_t size;
	int free;
	struct BlockHeader *next;
} BlockHeader;

#define HEADERSIZE sizeof(BlockHeader)

/* the base of the link list structure of blocks */
static void *BlockBase = NULL;
/* holds the address of the current top of heap brk */
static size_t CurrentTopBreak = 0;
/* holds the current amount of free space avail */
static size_t CurrentFreeSpace = 0;

/* Returns a new block of memory from the runtime heap */
BlockHeader *GetBlock(BlockHeader *last, size_t size) {
	BlockHeader *block;
	size_t blockSize, blockHeaderSize, nextBreak;
	void *sbrkResult;
	char buf[BUFSIZE];
	
	/* align block header */
	blockHeaderSize = HEADERSIZE;
	while (blockHeaderSize % BUSDIV != 0) {
		blockHeaderSize++;
	}
	
	/* align block content */
	blockSize = size + blockHeaderSize;
	while (blockSize % BUSDIV != 0) {
		blockSize++;
	}

	/* get the top of the break and error check */
	sbrkResult = sbrk(0);
	if (sbrkResult == (void *)-1) {
		snprintf(buf, BUFSIZE,
		 "sbrk error\n");
		fputs(buf, stderr);
		return NULL;
	}

	/* nextBreak will be the address of the next block
	 * after creating the requested block
	 */
	nextBreak = (size_t)sbrkResult + blockSize;
	
	/* break off more space from the heap if there's
	 * not enough space for the block
	 */
	if (CurrentTopBreak == 0 || nextBreak > CurrentTopBreak) {
		/* break off blockSize if the block size is larger
		 * than the regular chunk break
		 */
		if (blockSize > CHUNKSIZE) {
			sbrkResult = sbrk(blockSize);
			if (sbrkResult == (void *) -1) {
				snprintf(buf, BUFSIZE,
				 "sbrk error\n");
				fputs(buf, stderr);
				errno = ENOMEM;
				return NULL;
			}

			sbrkResult = sbrk(0);
			if (sbrkResult == (void *) -1) {
				snprintf(buf, BUFSIZE,
				 "sbrk error\n");
				fputs(buf, stderr);
				return NULL;
			}
			CurrentTopBreak = (size_t)sbrkResult;
			CurrentFreeSpace = CurrentTopBreak - blockSize;
		}
		/* otherwise break off the regular chunk break */
		else {
			sbrkResult = sbrk(CHUNKSIZE);
			if (sbrkResult == (void *) -1) {
				snprintf(buf, BUFSIZE,
				 "sbrk error\n");
				fputs(buf, stderr);
				errno = ENOMEM;
				return NULL;
			}
	
			sbrkResult = sbrk(0);
			if (sbrkResult == (void *) -1) {
				snprintf(buf, BUFSIZE,
				 "sbrk error\n");
				fputs(buf, stderr);
				return NULL;
			}
			CurrentTopBreak = (size_t)sbrkResult;
			CurrentFreeSpace = CurrentTopBreak - CHUNKSIZE;
		}
	}

	block = (BlockHeader *)CurrentFreeSpace;

	/* set the block as the next element of the link
	 * list of blocks
	 */
	if (last) {
		last->next = block;
	}

	block->size = size;
	block->next = NULL;
	block->free = 0;
	return block;
}

/* returns a freed block of memory from the runtime heap
 * if one exists
 */
BlockHeader *FindFreeBlock(BlockHeader **last, size_t size) {
	BlockHeader *linkBlock = BlockBase;

	/* traverse through the link list of blocks and look for one that
	 * is free and can accomodate the requested size
	 */
	while (linkBlock && !(linkBlock->free && linkBlock->size >= size)) {
		*last = linkBlock;
		linkBlock = linkBlock->next;
	}
	return linkBlock;
}

/* returns a pointer to a dynamically allocated block of memory
 * of the requested size
 */
void *malloc(size_t size) {
	BlockHeader *block, *last;
	size_t dataStart;
	char buf[BUFSIZE];

	if (size <= 0) {
		return NULL;
	}

	/* if the link list of blocks is empty
	 * then request a block and make it the list base
	 */
	if (!BlockBase) {
		block = GetBlock(NULL, size);
		if (!block) {
			return NULL;
		}
		BlockBase = block;
	}
	else {
		/* look for any free blocks that can be reused */
		last = BlockBase;
		block = FindFreeBlock(&last, size);

		/* request a new block if no free could be re-used */
		if (!block) {
			block = GetBlock(last, size);
			if (!block) {
				return NULL;
			}
		}
		/* un-free the block if one could be re-used */
		else {
			block->free = 0;
		}
	}

	/* align the start addr of the data content of the block */
	dataStart = (size_t)(block + 1);
	while (dataStart % BUSDIV != 0) {
		dataStart++;
	}

	if (getenv("DEBUG_MALLOC")) {
		snprintf(buf, BUFSIZE, 
		 "MALLOC: malloc(%d)		=> (ptr=%p, size=%d)\n", 
		 (int)size, (void *)dataStart, (int)block->size);
		fputs(buf, stdout);
	}

	/* return a pointer to the data content of the block */
	return (void *)dataStart;
}

/* returns the header to the given pointer to a block */
BlockHeader *GetBlockHeader(void *ptr) {
	BlockHeader *headerPtr;
	size_t startAddr;

	headerPtr = ptr;
	headerPtr -= 1;
	startAddr = (size_t)headerPtr;
	/* header was aligned when created
	 * so need to align it back
	 */
	while (startAddr % BUSDIV != 0) {
		startAddr--;
	}
	headerPtr = (BlockHeader *)startAddr;

	return headerPtr;
}

/* marks the block of memory as freed for re-use */
void free(void *ptr) {
	BlockHeader *blockStart;
	char buf[BUFSIZE];

	if (!ptr) {
		return;
	}

	blockStart = GetBlockHeader(ptr);
	blockStart->free = 1;

	if (getenv("DEBUG_MALLOC")) {
		snprintf(buf, BUFSIZE, "MALLOC: free(%p)\n", ptr);
		fputs(buf, stdout);
	}
}

/* reallocates the block to the requested size,
 * expanding or shrinking as needed
 */
void *realloc(void *ptr, size_t size) {
	BlockHeader *blockHeader, *shrinkHeader;
	void *newHeader;
	int blockSize, spaceNeeded;
	size_t blockEnd, blockFreeSpace, shrinkSize;
	char buf[BUFSIZE];

	if (!ptr) {
		return malloc(size);
	}

	if(size == 0) {
		free(ptr);
		return NULL;
	}
	else {
		blockHeader = GetBlockHeader(ptr);
		blockSize = HEADERSIZE + blockHeader->size;
		blockEnd = (size_t)blockHeader + blockSize;

		/* if there's another block in the list
		 * then set the amount of free pad space available
		 * no need to set if there is none since we'll do
		 * a nell check later before checking blockFreeSpace
		 */
		if (blockHeader->next != NULL) {
			blockFreeSpace = (size_t)(blockHeader->next) 
			 - blockEnd;
		}
		spaceNeeded = size - blockHeader->size;

		/* using realloc to shrink will cause spaceNeeded to be negative
		 * if there's space for a header and at least a byte, then 
		 * create a new block
		 */
		if (spaceNeeded < 0 && (spaceNeeded * -1) > HEADERSIZE) {
			shrinkSize = (spaceNeeded * -1) - HEADERSIZE;
			shrinkHeader = GetBlock(blockHeader, shrinkSize);
			shrinkHeader->next = blockHeader->next;
			blockHeader->next = shrinkHeader;
		}

		/* expand the block if there is no next block
		 * or there's enough pad space in the current
		 * block for the new size
		 */
		if ((blockHeader->next == NULL 
		 && CurrentFreeSpace < spaceNeeded) || 
		 spaceNeeded <= blockFreeSpace) {
			blockHeader->size = size;
			newHeader = ptr;
		}
		/* otherwise we need to allocate a new block
		 * and copy the old block's content over
		 */
		else {
			newHeader = malloc(size);
			memcpy(newHeader, ptr, blockHeader->size);
			free(ptr);
		}

		if (getenv("DEBUG_MALLOC")) {
			snprintf(buf, BUFSIZE, 
			 "MALLOC: realloc(%p,%d)	=> (ptr=%p, size=%d)\n",
			 ptr, (int)size, newHeader, 
			 (int)(((BlockHeader *)newHeader)->size));
			fputs(buf, stdout);
		}

		return newHeader;
	} 
}

/* allocates a block of dynamic memory but also initializes
 * all the bytes to 0
 */
void *calloc(size_t nmemb, size_t size) {
	size_t blockSize;
	void *ptr;
	char buf[BUFSIZE];

	blockSize = nmemb * size;
	ptr = malloc(blockSize);
	memset(ptr, 0, blockSize);

	if (getenv("DEBUG_MALLOC")) {
		snprintf(buf, BUFSIZE, 
		 "MALLOC: calloc(%d,%d)		=> (ptr=%p, size=%d)\n", 
		 (int)nmemb, (int)size, ptr, (int)(GetBlockHeader(ptr)->size));
		fputs(buf, stdout);
	}

	return ptr;
}

