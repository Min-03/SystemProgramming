/* 
 * mm-implicit.c -  Simple allocator based on implicit free lists, 
 *                  first fit placement, and boundary tag coalescing. 
 *
 * Each block has header and footer of the form:
 * 
 *      31                     3  2  1  0 
 *      -----------------------------------
 *     | s  s  s  s  ... s  s  s  0  0  a/f
 *      ----------------------------------- 
 * 
 * where s are the meaningful size bits and a/f is set 
 * iff the block is allocated. The list has the following form:
 *
 * begin                                                          end
 * heap                                                           heap  
 *  -----------------------------------------------------------------   
 * |  pad   | hdr(8:a) | ftr(8:a) | zero or more usr blks | hdr(8:a) |
 *  -----------------------------------------------------------------
 *          |       prologue      |                       | epilogue |
 *          |         block       |                       | block    |
 *
 * The allocated prologue and epilogue blocks are overhead that
 * eliminate edge conditions during coalescing.
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "mm.h"
#include "memlib.h"

/*
 * If NEXT_FIT defined use next fit search, else use first fit search 
 */
#define NEXT_FITx

/* Team structure (this should be one-man team, meaning that you are the only member of the team) */
team_t team = {
#ifdef NEXT_FIT
    "explicit next fit", 
#else
    "explicit first fit", 
#endif
    "Minseo Kim", "2022019734", /* your name and student id in quote */
    "", ""
}; 

/* $begin mallocmacros */
/* Basic constants and macros */
#define WSIZE       4       /* word size (bytes) */  
#define DSIZE       8       /* doubleword size (bytes) */
#define PSIZE       8       /* pointer size (bytes) */
#define CHUNKSIZE  (1<<12)  /* initial heap size (bytes) */
#define OVERHEAD    8       /* overhead of header and footer (bytes) */

#define MAX(x, y) ((x) > (y)? (x) : (y))
#define MIN(x, y) ((x) > (y)? (y) : (x))

/* Pack a size and allocated bit into a word */
#define PACK(size, alloc)  ((size) | (alloc))

/* Read and write a word at address p */
#define GET(p)       (*(size_t *)(p))
#define PUT(p, val)  (*(size_t *)(p) = (val))  
#define PUT_ADDR(p, val) (*(void **)(p) = (void *) (val))

/* Read the size and allocated fields from address p */
#define GET_SIZE(p)  (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

/* Given block ptr bp, compute address of its header and footer */
#define HDRP(bp)       ((char *)(bp) - WSIZE)  
#define FTRP(bp)       ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* Given block ptr bp, compute address of its next and prev pointer */
#define NXTP(bp)       ((char *)(bp))  
#define PRVP(bp)       ((char *)(bp) + PSIZE)

/* Given block ptr bp, compute address of next and previous blocks */
#define NEXT_BLKP(bp)  ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))
#define PREV_BLKP(bp)  ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

/* Given block ptr bp, compute address of next and previous blocks in explicit list*/
#define NEXT_BLKP_EX(bp)  (*(void **)(NXTP(bp)))
#define PREV_BLKP_EX(bp)  (*(void **)(PRVP(bp)))
/* $end mallocmacros */

/* Global variables */
static char *heap_listp = NULL;  /* pointer to first explicit block */
#ifdef NEXT_FIT
static char *rover;       /* next fit rover */
#endif

/* function prototypes for internal helper routines */
static void *extend_heap(size_t words);
static void place(void *bp, size_t asize);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void printblock(void *bp); 
static void checkblock(void *bp);
static void insertblock(void *bp);
static void deleteblock(void *bp);

/* 
 * mm_init - Initialize the memory manager 
 */
/* $begin mminit */
int mm_init(void) 
{
    /* create the initial empty heap */
    if ((heap_listp = mem_sbrk(4*WSIZE + 2*PSIZE)) == (void *)-1)
	return -1;
    PUT(heap_listp, 0);                        /* alignment padding */
    PUT(heap_listp+WSIZE, PACK(OVERHEAD+2*PSIZE, 1));  /* prologue header */
    PUT_ADDR(heap_listp+DSIZE, NULL);
    PUT_ADDR(heap_listp+DSIZE+PSIZE, NULL);
    PUT(heap_listp+DSIZE+2*PSIZE, PACK(OVERHEAD+2*PSIZE, 1));  /* prologue footer */ 
    PUT(heap_listp+WSIZE+DSIZE+2*PSIZE, PACK(0, 1));   /* epilogue header */
    heap_listp += DSIZE;


#ifdef NEXT_FIT
    rover = heap_listp;
#endif

    /* Extend the empty heap with a free block of CHUNKSIZE bytes */
    // if (extend_heap(CHUNKSIZE/WSIZE) == NULL)
	// return -1;
    return 0;
}
/* $end mminit */

/* 
 * mm_malloc - Allocate a block with at least size bytes of payload 
 */
/* $begin mmmalloc */
void *mm_malloc(size_t size) 
{
	if (size == 0) {
        return NULL;
    }
    size_t realSize = DSIZE * ((size + OVERHEAD + 2 * PSIZE + (DSIZE - 1)) / DSIZE);
    void *bp = find_fit(realSize);
    if (bp != NULL) {
        place(bp, realSize);
        return bp;
    }
    bp = extend_heap(realSize / WSIZE);
    if (bp == NULL) return NULL;
    place(bp, realSize);

    return bp;
} 
/* $end mmmalloc */

/* 
 * mm_free - Free a block 
 */
/* $begin mmfree */
void mm_free(void *bp)
{
    if (!GET_ALLOC(HDRP(bp))) return;
    PUT(HDRP(bp), GET_SIZE(HDRP(bp)));
    PUT(FTRP(bp), GET_SIZE(HDRP(bp)));
    coalesce(bp);
}

/* $end mmfree */

/*
 * mm_realloc - naive implementation of mm_realloc
 */
void *mm_realloc(void *ptr, size_t size)
{
    size_t realSize = DSIZE * ((size + OVERHEAD + 2 * PSIZE + (DSIZE - 1)) / DSIZE);
    if (ptr == NULL) {
        return mm_malloc(realSize);
    } else if (size == 0) {
        free(ptr);
        return NULL;
    }
    size_t remainSize = GET_SIZE(HDRP(ptr)) - realSize;
    //allocate within existing block
    if (GET_SIZE(HDRP(ptr)) >= realSize) {
        if (remainSize >= (DSIZE + 2 * PSIZE + OVERHEAD)) {
            PUT(HDRP(ptr), PACK(realSize, 1));
            PUT(FTRP(ptr), PACK(realSize, 1));
            void *nxt = NEXT_BLKP(ptr);
            PUT(HDRP(nxt), PACK(remainSize, 0));
            PUT(FTRP(nxt), PACK(remainSize, 0));
            insertblock(nxt);
        } 
        return ptr;
    }
    //extend block
    void *nxt = NEXT_BLKP(ptr);
    if (!GET_ALLOC(HDRP(nxt)) && (GET_SIZE(HDRP(nxt)) >= remainSize)) {
        deleteblock(nxt);
        size_t totalSize = GET_SIZE(HDRP(ptr)) + GET_SIZE(HDRP(nxt));
        if ((totalSize - realSize) >= (DSIZE + 2 * PSIZE + OVERHEAD)) {
            PUT(HDRP(ptr), PACK(realSize, 1));
            PUT(FTRP(ptr), PACK(realSize, 1));
            nxt = NEXT_BLKP(ptr);
            PUT(HDRP(nxt), PACK(totalSize - realSize, 0));
            PUT(FTRP(nxt), PACK(totalSize - realSize, 0));
            insertblock(nxt);
        } else {
            PUT(HDRP(ptr), PACK(totalSize, 1));
            PUT(FTRP(ptr), PACK(totalSize, 1));
        }
        return ptr;
    } 
    
    //move block
    void *bp = mm_malloc(realSize);
    if (bp == NULL) return NULL;
    size_t copySize = MIN(size, GET_SIZE(HDRP(ptr)));
    memcpy(bp, ptr, copySize);
    mm_free(ptr);
    return bp;
}


/* 
 * mm_checkheap - Check the heap for consistency 
 */
void mm_checkheap(int verbose) 
{
    char *bp = heap_listp;

    if (verbose)
	printf("Heap (%p):\n", heap_listp);

    if ((GET_SIZE(HDRP(heap_listp)) != DSIZE) || !GET_ALLOC(HDRP(heap_listp)))
	printf("Bad prologue header\n");
    checkblock(heap_listp);

    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
	if (verbose) 
	    printblock(bp);
	checkblock(bp);
    }
     
    if (verbose)
	printblock(bp);
    if ((GET_SIZE(HDRP(bp)) != 0) || !(GET_ALLOC(HDRP(bp))))
	printf("Bad epilogue header\n");
}

/* The remaining routines are internal helper routines */

/* 
 * extend_heap - Extend heap with free block and return its block pointer
 */
/* $begin mmextendheap */
static void *extend_heap(size_t words) 
{
    char *bp;
    size_t size;
	
    /* Allocate an even number of words to maintain alignment */
    size = (words % 2) ? (words+1) * WSIZE : words * WSIZE;
    if ((bp = mem_sbrk(size)) == (void *)-1) 
	return NULL;

    /* Initialize free block header/footer and the epilogue header */
    PUT(HDRP(bp), PACK(size, 0));         /* free block header */
    PUT(FTRP(bp), PACK(size, 0));         /* free block footer */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* new epilogue header */
    /* Coalesce if the previous block was free */
    return coalesce(bp);
}
/* $end mmextendheap */

/* 
 * place - Place block of asize bytes at start of free block bp 
 *         and split if remainder would be at least minimum block size
 */
/* $begin mmplace */
/* $begin mmplace-proto */
static void place(void *bp, size_t asize)
/* $end mmplace-proto */
{
    size_t csize = GET_SIZE(HDRP(bp));   

    deleteblock(bp);
    if ((csize - asize) >= (DSIZE + 2 * PSIZE + OVERHEAD)) { 
	PUT(HDRP(bp), PACK(asize, 1));
	PUT(FTRP(bp), PACK(asize, 1));
	bp = NEXT_BLKP(bp);
	PUT(HDRP(bp), PACK(csize-asize, 0));
	PUT(FTRP(bp), PACK(csize-asize, 0));
    insertblock(bp);
    }
    else { 
	PUT(HDRP(bp), PACK(csize, 1));
	PUT(FTRP(bp), PACK(csize, 1));
    }
}
/* $end mmplace */

/* 
 * find_fit - Find a fit for a block with asize bytes 
 */
static void *find_fit(size_t asize)
{
#ifdef NEXT_FIT 
    /* next fit search */
    char *oldrover = rover;

    /* search from the rover to the end of list */
    for (; rover != NULL; rover = NEXT_BLKP_EX(rover))
	if (!GET_ALLOC(rover) && asize <= GET_SIZE(HDRP(rover))){
	    return rover;
    }

    // /* search from start of list to old rover */
    for (rover = NEXT_BLKP_EX(heap_listp); rover != NULL; rover = NEXT_BLKP_EX(rover))
	if (asize <= GET_SIZE(HDRP(rover))) {
	    return rover;
    }

    return NULL;  /* no fit found */
#else 
    /* first fit search */
    void *bp;

    for (bp = NEXT_BLKP_EX(heap_listp); bp != NULL; bp = NEXT_BLKP_EX(bp)) {
	if (asize <= GET_SIZE(HDRP(bp))) {
	    return bp;
	}
    }
    return NULL; /* no fit */
#endif
}

/*
 * coalesce - boundary tag coalescing. Return ptr to coalesced block
 */
static void *coalesce(void *bp) 
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    if (!prev_alloc) {
        void *prv = PREV_BLKP(bp);
        size += GET_SIZE(HDRP(prv));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(prv), PACK(size, 0));
        deleteblock(prv);
        bp = prv;
    }

    if (!next_alloc) {
        void *nxt = NEXT_BLKP(bp);
        size += GET_SIZE(HDRP(nxt));
        deleteblock(nxt);
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    }

    insertblock(bp);

#ifdef NEXT_FIT
    /* Make sure the rover isn't pointing into the free block */
    /* that we just coalesced */
    if ((rover > (char *)bp) && (rover < NEXT_BLKP(bp))) {
        rover = bp;
    }
#endif

    return bp;
}


static void printblock(void *bp) 
{
    size_t hsize, halloc, fsize, falloc;

    hsize = GET_SIZE(HDRP(bp));
    halloc = GET_ALLOC(HDRP(bp));  
    fsize = GET_SIZE(FTRP(bp));
    falloc = GET_ALLOC(FTRP(bp));  
    
    if (hsize == 0) {
	printf("%p: EOL\n", bp);
	return;
    }

    fprintf(stderr, "%p: header: [%d:%c] footer: [%d:%c]\n", bp, 
	   hsize, (halloc ? 'a' : 'f'), 
	   fsize, (falloc ? 'a' : 'f')); 
}

static void checkblock(void *bp) 
{
    if ((size_t)bp % 8)
	printf("Error: %p is not doubleword aligned\n", bp);
    if (GET(HDRP(bp)) != GET(FTRP(bp)))
	printf("Error: header does not match footer\n");
}

static void insertblock(void *bp) {
    void *tmp = NEXT_BLKP_EX(heap_listp);
    PUT_ADDR(NXTP(bp), tmp);
    if (tmp != NULL) PUT_ADDR(PRVP(tmp), bp);
    PUT_ADDR(PRVP(bp), heap_listp);
    PUT_ADDR(NXTP(heap_listp), bp);
}

static void deleteblock(void *bp) {
    #ifdef NEXT_FIT
        if (rover == bp) rover = NEXT_BLKP_EX(bp);
    #endif
    void *nxt = NEXT_BLKP_EX(bp);
    void *prv = PREV_BLKP_EX(bp);
    if (nxt != NULL) PUT_ADDR(PRVP(nxt), prv);
    PUT_ADDR(NXTP(prv), nxt);
}