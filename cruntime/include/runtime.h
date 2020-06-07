﻿#pragma once
#ifndef __RUNTIME_H__
#define __RUNTIME_H__

/*---------------------------------------------------------------------------
  Copyright 2020 Daan Leijen, Microsoft Corporation.
 
  This is free software; you can redistribute it and/or modify it under the
  terms of the Apache License, Version 2.0. A copy of the License can be
  found in the file "license.txt" at the root of this distribution.
---------------------------------------------------------------------------*/

#define REFCOUNT_LIMIT_TO_32BIT 0
#define MULTI_THREADED          1
#define ARCH_LITTLE_ENDIAN      1
//#define ARCH_BIG_ENDIAN       1

#include <assert.h>
#include <errno.h>   // ENOSYS, etc
#include <limits.h>  // LONG_MAX, etc
#include <stddef.h>  // ptrdiff_t
#include <stdint.h>  // int_t, etc
#include <stdbool.h> // bool
#include <stdio.h>   // FILE*
#include <string.h>  // strlen
#include <stdlib.h>  // malloc, abort, etc.
#include <math.h>    

#include "runtime/platform.h"  // Platform abstractions and portability definitions
#include "runtime/atomic.h"    // Atomic operations


/*--------------------------------------------------------------------------------------
  Basic datatypes
--------------------------------------------------------------------------------------*/

// Tags for heap blocks
typedef enum tag_e {
  TAG_INVALID   = 0,
  TAG_MIN       = 1,
  TAG_MAX       = 65000,
  TAG_OPEN,        // open datatype, first field is a string tag
  TAG_BOX,         // boxed value type
  TAG_REF,         // mutable reference
  TAG_FUNCTION,    // function with free its free variables
  TAG_BIGINT,      // big integer (see `integer.c`)
  TAG_STRING_SMALL,// UTF8 encoded string of at most 7 bytes.
  TAG_STRING,      // UTF8 encoded string: valid (modified) UTF8 ending with a zero byte.
  TAG_BYTES,       // a vector of bytes
  TAG_VECTOR_SMALL,// a vector of (boxed) values of at most (SCAN_FSIZE_MAX-1) length
  TAG_VECTOR,      // a vector of (boxed) values
  TAG_INT64,       // boxed int64_t
#if INTPTR_SIZE < 8
  TAG_INT32,       // boxed int32_t
  TAG_DOUBLE,      // boxed IEEE double (64-bit)
  TAG_FLOAT,       // boxed IEEE float  (32-bit)
#endif
  // raw tags have a free function together with a `void*` to the data
  TAG_CPTR_RAW,    // full void* (must be first, see tag_is_raw())
  TAG_STRING_RAW,  // pointer to a valid UTF8 string
  TAG_BYTES_RAW,   // pointer to bytes
  TAG_LAST         
} tag_t;

static inline bool tag_is_raw(tag_t tag) {
  return (tag >= TAG_CPTR_RAW);
}

// Every heap block starts with a 64-bit header with a reference count, tag, and scan fields count.
// The reference count is 0 for a unique reference (for a faster free test in drop).
// Reference counts larger than 0x8000000 use atomic increment/decrement (for thread shared objects).
// (Reference counts are always 32-bit (even on 64-bit) platforms but get "sticky" if 
//  they get too large (>0xC0000000) and in such case we never free the object, see `refcount.c`)
typedef struct header_s {
  uint32_t  refcount;         // reference count
  uint16_t  tag;              // header tag
  uint8_t   scan_fsize;       // number of fields that should be scanned when releasing (`scan_fsize <= 0xFF`, if 0xFF, the full scan size is the first field)  
  uint8_t   thread_shared : 1;
} header_t;

#define SCAN_FSIZE_MAX (255)
#define HEADER(scan_fsize,tag)         { 0, tag, scan_fsize, 0 }            // start with refcount of 0
#define HEADER_STATIC(scan_fsize,tag)  { U32(0xFF00), tag, scan_fsize, 0 }  // start with recognisable refcount (anything > 1 is ok)


// Polymorphic operations work on boxed values. (We use a struct for extra checks on accidental conversion)
// See `box.h` for definitions.
typedef struct box_s {
  uintptr_t box;          // We use unsigned representation to avoid UB on shift operations and overflow.
} box_t;

// An integer is either a small int or a pointer to a bigint_t. Identity with boxed values.
// See `integer.h` for definitions.
typedef box_t integer_t;

// boxed forward declarations
static inline uintx_t   unbox_enum(box_t v);
static inline box_t     box_enum(uintx_t u);


/*--------------------------------------------------------------------------------------
  Blocks 
--------------------------------------------------------------------------------------*/

// A heap block is a header followed by `scan_fsize` boxed fields and further raw bytes
// A `block_t*` is never NULL (to avoid testing for NULL for reference counts).
typedef struct block_s {
  header_t header;
} block_t;

// A large block has a (boxed) large scan size for vectors.
typedef struct block_large_s {
  block_t  _block;
  box_t    large_scan_fsize; // if `scan_fsize == 0xFF` there is a first field with the full scan size
} block_large_t;

// A pointer to a block is never NULL.
typedef block_t* ptr_t;


static inline decl_const tag_t block_tag(const block_t* b) {
  return (tag_t)(b->header.tag);
}

static inline decl_pure size_t block_scan_fsize(const block_t* b) {
  const size_t sfsize = b->header.scan_fsize;
  if (likely(sfsize != SCAN_FSIZE_MAX)) return sfsize;
  const block_large_t* bl = (const block_large_t*)b; 
  return unbox_enum(bl->large_scan_fsize);
}

static inline decl_pure uintptr_t block_refcount(const block_t* b) {
  return b->header.refcount;
}

static inline decl_pure bool block_is_unique(const block_t* b) {
  return (likely(b->header.refcount == 0));
}


/*--------------------------------------------------------------------------------------
  The thread local context as `context_t`
  This is passed by the code generator as an argument to every function so it can
  be (usually) accessed efficiently through a register.
--------------------------------------------------------------------------------------*/
typedef void*  heap_t;

// A function has as its first field a pointer to a C function that takes the 
// `function_t` itself as a first argument. The following fields are the free variables.
typedef struct function_s {
  block_t  _block;
  box_t    fun;     
  // followed by free variables
} *function_t;

// A vector is an array of boxed values.
typedef struct vector_s {
  block_t _block;
} *vector_t;


#define YIELD_CONT_MAX (8)

typedef enum yield_kind_e {
  YIELD_NONE,
  YIELD_NORMAL,
  YIELD_FINAL
} yield_kind_t;

typedef struct yield_s {
  int32_t    marker;          // marker of the handler to yield to
  function_t clause;          // the operation clause to execute when the handler is found
  size_t     conts_count;     // number of continuations in `conts`
  function_t conts[YIELD_CONT_MAX];  // fixed array of continuations. The final continuation `k` is
                              // composed as `fN ○ ... ○ f2 ○ f1` if `conts = { f1, f2, ..., fN }` 
                              // if the array becomes full, a fresh array is allocated and the first
                              // entry points to its composition.
} yield_t;

typedef struct context_s {
  uint8_t     yielding;         // are we yielding to a handler? 0:no, 1:yielding, 2:yielding_final (e.g. exception) // put first for efficiency
  heap_t      heap;             // the (thread-local) heap to allocate in; todo: put in a register?
  vector_t    evv;              // the current evidence vector for effect handling: vector for size 0 and N>1, direct evidence for one element vector
  yield_t     yield;            // inlined yield structure (for efficiency)
  int32_t     marker_unique;    // unique marker generation
  block_t*    delayed_free;     // list of blocks that still need to be freed
  integer_t   unique;           // thread local unique number generation
  uintptr_t   thread_id;        // unique thread id
  function_t  log;              // logging function
  function_t  out;              // std output
} context_t;

// Get the current (thread local) runtime context (should always equal the `_ctx` parameter)
decl_export context_t* runtime_context(void); 

// The current context is passed as a _ctx parameter in the generated code
#define current_context()   _ctx

// Is the execution yielding?
static inline decl_pure bool yielding(const context_t* ctx) {
  return (ctx->yielding != YIELD_NONE);
}

static inline decl_pure bool yielding_non_final(const context_t* ctx) {
  return (ctx->yielding == YIELD_NORMAL);
}

// Get a thread local marker unique number.
static inline int32_t marker_unique(context_t* ctx) {
  return ctx->marker_unique++;
}



/*--------------------------------------------------------------------------------------
  Allocation
--------------------------------------------------------------------------------------*/

static inline void* runtime_malloc(size_t sz, context_t* ctx) {
  UNUSED(ctx);
  return malloc(sz);
}

static inline void* runtime_zalloc(size_t sz, context_t* ctx) {
  UNUSED(ctx);
  return calloc(1, sz);
}

static inline void runtime_free(void* p) {
  free(p);
}

static inline void* runtime_realloc(void* p, size_t sz, context_t* ctx) {
  UNUSED(ctx);
  return realloc(p, sz);
}

decl_export void block_free(block_t* b, context_t* ctx);

static inline void block_init(block_t* b, size_t size, size_t scan_fsize, tag_t tag) {
  UNUSED(size);
  assert_internal(scan_fsize < SCAN_FSIZE_MAX);
  header_t header = { 0 };
  header.tag = (uint16_t)tag;
  header.scan_fsize = (uint8_t)scan_fsize;
  b->header = header;
}

static inline void block_large_init(block_large_t* b, size_t size, size_t scan_fsize, tag_t tag) {
  UNUSED(size);
  header_t header = { 0 };
  header.tag = (uint16_t)tag;
  header.scan_fsize = SCAN_FSIZE_MAX;
  b->_block.header = header;
  b->large_scan_fsize = box_enum(scan_fsize);
}

static inline block_t* block_alloc(size_t size, size_t scan_fsize, tag_t tag, context_t* ctx) {
  assert_internal(scan_fsize < SCAN_FSIZE_MAX);
  block_t* b = (block_t*)runtime_malloc(size, ctx);
  block_init(b, size, scan_fsize, tag);
  return b;
}

static inline block_large_t* block_large_alloc(size_t size, size_t scan_fsize, tag_t tag, context_t* ctx) {
  block_large_t* b = (block_large_t*)runtime_malloc(size + 1 /* the scan_large_fsize field */, ctx);
  block_large_init(b, size, scan_fsize, tag);
  return b;
}

static inline block_t* block_realloc(block_t* b, size_t size, context_t* ctx) {
  assert_internal(block_is_unique(b));
  return (block_t*)runtime_realloc(b, size, ctx);
}

static inline void* _block_as(block_t* b) {
  return (void*)b;
}
static inline void* _block_as_assert(block_t* b, tag_t tag) {
  assert_internal(block_tag(b) == tag);
  return (void*)b;
}

#define block_alloc_as(struct_tp,scan_fsize,tag,ctx)  ((struct_tp*)block_alloc(sizeof(struct_tp),scan_fsize,tag,ctx))
#define block_as(tp,b,tag)                            ((tp)_block_as_assert(b,tag))


/*--------------------------------------------------------------------------------------
  Reference counting
--------------------------------------------------------------------------------------*/

decl_export void     block_check_free(block_t* b, context_t* ctx);
decl_export block_t* dup_block_check(block_t* b);

static inline block_t* dup_block(block_t* b) {
  uint32_t rc = b->header.refcount;
  if (unlikely((int32_t)rc < 0)) {  // note: assume two's complement  (we can skip this check if we never overflow a reference count or use thread-shared objects.)
    return dup_block_check(b);      // thread-shared or sticky (overflow) ?
  }
  b->header.refcount = rc+1;
  return b;
}

static inline void drop_block(block_t* b, context_t* ctx) {
  uint32_t rc = b->header.refcount;
  if ((int32_t)rc <= 0) {         // note: assume two's complement
    block_check_free(b, ctx);     // thread-shared, sticky (overflowed), or can be freed? 
  }
  else {
    b->header.refcount = rc-1;
  }
}

#define datatype_as(tp,v,tag)             (block_as(tp,&(v)->_block,tag))
#define datatype_tag(v)                   (block_tag(&(v)->_block))
#define drop_datatype(v,ctx)              (drop_block(&(v)->_block,ctx))
#define dup_datatype_as(tp,v)             ((tp)dup_block(&(v)->_block))

#define constructor_tag(v)                (datatype_tag(&(v)->_inherit))
#define drop_constructor(v,ctx)           (drop_datatype(&(v)->_inherit,ctx))
#define dup_constructor_as(tp,v)          (dup_datatype_as(tp, &(v)->_inherit))


/*----------------------------------------------------------------------
  Further includes
----------------------------------------------------------------------*/

// The unit type
typedef enum unit_e {
  Unit = 0
} unit_t;

// A function to free a raw C pointer, raw bytes, or raw string.
typedef void (free_fun_t)(void*);
decl_export void free_fun_null(void* p);

// "raw" types: first field is pointer to a free function, the next field a pointer to raw C data
typedef struct cptr_raw_s {
  block_t     _block;
  free_fun_t* free;
  void*       cptr;
} *cptr_raw_t;


#include "runtime/box.h"
#include "runtime/integer.h"
#include "runtime/bitcount.h"
#include "runtime/string.h"

/*----------------------------------------------------------------------
  TLD operations
----------------------------------------------------------------------*/

// Get a thread local unique number.
static inline integer_t gen_unique(context_t* ctx) {
  integer_t u = ctx->unique;
  ctx->unique = integer_inc(dup_integer(u),ctx);
  return u;
}


/*--------------------------------------------------------------------------------------
  Reference counting scrutinee of a match where we try to avoid
  reference increments on fields of the match, and reuse memory in-place for
  deallocated scrutinees.
--------------------------------------------------------------------------------------*/
typedef struct orphan_s {
  block_t _block;
} *orphan_t;

extern struct orphan_s _orphan_null;

static inline decl_const orphan_t orphan_null(void) {
  return &_orphan_null;
}

static inline orphan_t orphan_block(block_t* b) {
  // set tag to zero, unique, with zero scan size (so decref is valid on it)
  memset(&b->header, 0, sizeof(header_t));
  return (orphan_t)b;
}

static inline orphan_t block_release0(block_t* b, context_t* ctx) {
  if (block_is_unique(b)) {
    return orphan_block(b);
  }
  else {
    drop_block(b, ctx);
    return orphan_null();
  }
}

static inline orphan_t block_release1(block_t* b, box_t unused_field1, context_t* ctx) {
  if (block_is_unique(b)) {
    drop_boxed(unused_field1, ctx);
    return orphan_block(b);
  }
  else {
    drop_block(b, ctx);
    return orphan_null();
  }
}

static inline void block_no_reuse(orphan_t o) {
  if (o != orphan_null()) {
    runtime_free(o);
  };
}

static inline block_t* block_alloc_reuse(orphan_t o, size_t size, size_t scan_fsize, tag_t tag, context_t* ctx) {
  // TODO: check usable size p >= size
  block_t* b;
  if (o != orphan_null()) {
    assert_internal(block_is_unique(&o->_block));
    b = &o->_block;
  }
  else {
    b = (block_t*)runtime_malloc(size, ctx);
  }
  block_init(b, size, scan_fsize, tag);
  return b;
}

/*--------------------------------------------------------------------------------------
  Value tags
--------------------------------------------------------------------------------------*/


// Tag for value types is always a boxed enum
typedef box_t value_tag_t;

// Use inlined #define to enable constant initializer expression
/*
static inline value_tag_t value_tag(uint_t tag) {
  return box_enum(tag);
}
*/
#define value_tag(tag) (box_from_uintptr(((uintx_t)tag << 2) | 0x03))


/*--------------------------------------------------------------------------------------
  Functions
--------------------------------------------------------------------------------------*/

#define function_alloc_as(tp,scan_fsize,ctx)    block_alloc_as(tp,scan_fsize,TAG_FUNCTION,ctx)
#define function_call(restp,argtps,f,args)      ((restp(*)argtps)(unbox_cptr(f->fun)))args
#define define_static_function(name,cfun) \
  static struct function_s _static_##name = { { HEADER_STATIC(0,TAG_FUNCTION) }, { (uintptr_t)&cfun } }; /* note: should be box_cptr(&cfun) but we need a constant expression */ \
  function_t name = &_static_##name;


extern function_t function_id;

static inline function_t unbox_function_t(box_t v) {
  return unbox_datatype_as(function_t, v, TAG_FUNCTION);
}

static inline box_t box_function_t(function_t d) {
  return box_datatype_as(function_t, d, TAG_FUNCTION);
}

static inline bool function_is_unique(function_t f) {
  return block_is_unique(&f->_block);
}

static inline void drop_function(function_t f, context_t* ctx) {
  drop_block(&f->_block, ctx);
}

static inline function_t dup_function(function_t f) {
  return block_as(function_t,dup_block(&f->_block),TAG_FUNCTION);
}


/*--------------------------------------------------------------------------------------
  References
--------------------------------------------------------------------------------------*/
typedef struct ref_s {
  block_t _block;
  box_t   value;
} *ref_t;

static inline ref_t ref_alloc(box_t value, context_t* ctx) {
  ref_t r = block_alloc_as(struct ref_s, 1, TAG_REF, ctx);
  r->value = value;
  return r;
}

static inline box_t ref_get(ref_t b) {
  return dup_boxed(b->value);
}

static inline unit_t ref_set(ref_t r, box_t value, context_t* ctx) {
  box_t b = r->value; 
  drop_boxed(b, ctx);
  r->value = value;
  return Unit;
}

static inline box_t ref_swap(ref_t r, box_t value) {
  box_t b = r->value;
  r->value = value;
  return b;
}

decl_export void fatal_error(int err, const char* msg, ...);

static inline void unsupported_external(const char* msg) {
  fatal_error(ENOSYS,msg);
}


/*--------------------------------------------------------------------------------------
  Unit
--------------------------------------------------------------------------------------*/

static inline box_t box_unit_t(unit_t u) {
  return box_enum(u);
}

static inline unit_t unbox_unit_t(box_t u) {
  assert_internal( unbox_enum(u) == (uintx_t)Unit);
  return Unit;
}

/*--------------------------------------------------------------------------------------
  Vector
--------------------------------------------------------------------------------------*/
extern vector_t vector_empty;

static inline void drop_vector(vector_t v, context_t* ctx) {
  drop_datatype(v, ctx);
}

static inline vector_t dup_vector(vector_t v) {
  return dup_datatype_as(vector_t, v);
}

typedef struct vector_small_s {
  struct vector_s _inherit;
  box_t    length;
  box_t    vec[1];            // vec[length]
} *vector_small_t;

typedef struct vector_large_s {
  struct block_large_s _block;
  box_t    length;
  box_t    vec[1];            // vec[length]
} *vector_large_t;

static inline vector_t vector_alloc(size_t length, box_t def, context_t* ctx) {
  if (length==0) {
    return dup_vector(vector_empty);
  }
  else if (length < SCAN_FSIZE_MAX) {
    vector_small_t v = block_as(vector_small_t, block_alloc(sizeof(struct vector_small_s) + (length-1)*sizeof(box_t), 1 + length, TAG_VECTOR_SMALL, ctx), TAG_VECTOR_SMALL);
    v->length = box_enum(length);
    if (def.box != box_null.box) {
      for (size_t i = 0; i < length; i++) {
        v->vec[i] = def;
      }
    }
    return &v->_inherit;
  }
  else {
    vector_large_t v = (vector_large_t)block_large_alloc(sizeof(struct vector_large_s) + (length-1)*sizeof(box_t), 1 + length, TAG_VECTOR, ctx);
    v->length = box_enum(length);
    if (def.box != box_null.box) {
      for (size_t i = 0; i < length; i++) {
        v->vec[i] = def;
      }
    }
    return (vector_t)(&v->_block._block);
  }
}

static inline size_t vector_len(const vector_t v) {
  if (datatype_tag(v)==TAG_VECTOR_SMALL) {
    return unbox_enum(datatype_as(vector_small_t,v,TAG_VECTOR_SMALL)->length);
  }
  else {
    return unbox_enum(datatype_as(vector_large_t, v, TAG_VECTOR)->length);
  }
}

static inline box_t* vector_buf(vector_t v, size_t* len) {
  if (len != NULL) *len = vector_len(v);
  if (datatype_tag(v)==TAG_VECTOR_SMALL) {
    return &(datatype_as(vector_small_t, v, TAG_VECTOR_SMALL)->vec[0]);
  }
  else {
    return &(datatype_as(vector_large_t, v, TAG_VECTOR)->vec[0]);
  }
}

static inline box_t vector_at(const vector_t v, size_t i) {
  assert(i < vector_len(v));
  return dup_boxed(vector_buf(v,NULL)[i]);
}

static inline box_t box_vector_t(vector_t v, context_t* ctx) {
  UNUSED(ctx);
  box_ptr(&v->_block);
}

static inline vector_t unbox_vector_t(box_t v, context_t* ctx) {
  UNUSED(ctx);
  block_t* b = unbox_ptr(v);
  assert_internal(block_tag(b) == TAG_VECTOR_SMALL || block_tag(b) == TAG_VECTOR);
  return (vector_t)b;
}

/*--------------------------------------------------------------------------------------
  Bytes
--------------------------------------------------------------------------------------*/

typedef struct bytes_s {
  block_t  _block;               // TAG_BYTES or TAG_BYTES_RAW
}* bytes_t;

struct bytes_vector_s {          // in-place bytes
  struct bytes_s  _inherit;
  size_t          length;
  char            buf[1];
};

struct bytes_raw_s {             // pointer to bytes with free function
  struct bytes_s _inherit;
  free_fun_t*    free;
  uint8_t*       data;
  size_t         length;
};

#endif // include guard
