/* engine/heap.h on the C malloc of TinyGo, for wasm32.

   The heap of liustack/quickjs (engine/heap.c with TLSF in tlsf.c) maps 64 KiB
   chunks from the host and is written for 64-bit targets only: it stops the
   build on wasm32. Upstream has a second implementation of the same interface
   for its sanitizer builds, engine/test/heap-system.c, where every block comes
   from the system malloc and the engine is built with JS_HEAP_SYSTEM_MALLOC.
   This file is that one, rewritten for this host: TinyGo's malloc has no
   malloc_usable_size, so each block carries its size in a header of its own.
   The counting is the same as upstream's: a block costs its usable size and 8
   bytes, so JS_SetMemoryLimit limits what it limits on the native engine. */
#include "engine_config.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../third_party/quickjs-engine/heap.h"

/* TinyGo exports these to C (src/runtime/arch_tinygowasm_malloc.go). The
   engine's libc headers do not declare them: the engine itself never calls
   them. */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

#define HEAP_BLOCK_OVERHEAD 8

/* 16 bytes keep the block as aligned as malloc gave it (JS_MALLOC_ALIGN is 8) */
typedef struct WasmBlock {
    size_t size;
    size_t pad[16 / sizeof(size_t) - 1];
} WasmBlock;

static WasmBlock *block_of(const void *ptr)
{
    return (WasmBlock *)((char *)ptr - sizeof(WasmBlock));
}

size_t heap_usable_size(const void *ptr)
{
    return block_of(ptr)->size;
}

static int heap_over(const JSHeap *h, size_t base, size_t request)
{
    return base > h->limit || request > h->limit - base;
}

JSHeap *heap_create(uint32_t vm, size_t rt_size)
{
    JSHeap *h = calloc(1, rt_size);
    if (!h)
        return NULL;
    h->vm = vm;
    h->limit = (size_t)-1;
    h->rt_size = rt_size;
    h->count = rt_size;
    return h;
}

void heap_destroy(JSHeap *h)
{
    free(h);
}

/* there are no chunks to remap */
void heap_set_remap(JSHeap *h, int on)
{
    (void)h;
    (void)on;
}

void *heap_alloc(JSHeap *h, size_t size)
{
    WasmBlock *b;

    if (size == 0 || size > JS_HEAP_MAX_REQUEST || heap_over(h, h->count, size))
        return NULL;
    b = malloc(sizeof(WasmBlock) + size);
    if (!b)
        return NULL;
    b->size = size;
    h->count += size + HEAP_BLOCK_OVERHEAD;
    h->blocks++;
    return b + 1;
}

void heap_free(JSHeap *h, void *ptr)
{
    if (!ptr)
        return;
    h->count -= heap_usable_size(ptr) + HEAP_BLOCK_OVERHEAD;
    h->blocks--;
    free(block_of(ptr));
}

/* As upstream's heap-system.c: the count is checked as count + the request -
   the usable size of the old block. The copy of a large block cannot be
   stopped half way here (*h->stop is not read): the realloc of TinyGo copies
   it in one go. */
void *heap_realloc(JSHeap *h, void *ptr, size_t size)
{
    size_t old_size;
    WasmBlock *b;

    h->stopped = 0;
    if (!ptr)
        return heap_alloc(h, size);
    if (size == 0) {
        heap_free(h, ptr);
        return NULL;
    }
    if (size > JS_HEAP_MAX_REQUEST)
        return NULL;
    old_size = heap_usable_size(ptr);
    if (heap_over(h, h->count - old_size, size))
        return NULL;
    b = realloc(block_of(ptr), sizeof(WasmBlock) + size);
    if (!b)
        return NULL;
    b->size = size;
    h->count = h->count - old_size + size;
    return b + 1;
}

/* nothing to give back: the blocks go back to TinyGo's allocator one by one */
void heap_release_start(JSHeap *h, size_t keep)
{
    (void)h;
    (void)keep;
}

JSMemRange heap_release_next_chunk(JSHeap *h)
{
    JSMemRange none = { 0, 0 };
    (void)h;
    return none;
}

JSMemRange heap_release_next_range(JSHeap *h)
{
    JSMemRange none = { 0, 0 };
    (void)h;
    return none;
}

/* the engine asks for chunks only through heap.c, which is not built here */
uintptr_t host_memory_grow(uint32_t vm, uintptr_t size)
{
    (void)vm;
    (void)size;
    return 0;
}
