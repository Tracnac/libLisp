#ifndef LIBLISP_H
#define LIBLISP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Forward declarations
// ============================================================================
typedef struct Arena Arena;
typedef struct Symbol Symbol;
typedef struct SymbolTable SymbolTable;
typedef struct BindingStack BindingStack;
typedef struct Node Node;
typedef struct Lexer Lexer;
typedef struct Token Token;

// ============================================================================
// Constants
// ============================================================================
#define ARENA_SIZE (1024 * 1024) // 1MB
#define SYMBOL_TABLE_SIZE 1024
#define MAX_BINDINGS 64

// ============================================================================
// Arena Allocator
// ============================================================================
struct Arena {
  char *ptr;
  size_t offset;
  size_t size;
};

static void arena_init(Arena *arena, size_t size) {
  arena->ptr = (char *)malloc(size);
  arena->offset = 0;
  arena->size = size;
}

static void *arena_alloc(Arena *arena, size_t size) {
  size_t aligned = (size + 7) & ~7; // 8-byte align
  if (arena->offset + aligned > arena->size)
    return NULL;
  void *result = arena->ptr + arena->offset;
  arena->offset += aligned;
  return result;
}

static void arena_reset(Arena *arena) { arena->offset = 0; }

static void arena_free(Arena *arena) { free(arena->ptr); }

// ============================================================================
// NaN-Boxing
// ============================================================================

#define NANBOX_INT_TAG 0xFFF9000000000000ULL
#define NANBOX_NAN_MASK 0xFFF8000000000000ULL

typedef uint64_t Value;

static inline Value val_int(int64_t i) { return NANBOX_INT_TAG | (uint32_t)i; }
static inline int64_t val_to_int(Value v) {
  return (int64_t)(int32_t)(v & 0xFFFFFFFFULL);
}
static inline int val_is_int(Value v) {
  return (v & NANBOX_NAN_MASK) == (NANBOX_INT_TAG & NANBOX_NAN_MASK);
}

// ============================================================================
// Symbol Table (Direct Storage)
// ============================================================================
struct Symbol {
  const char *name;
  uint32_t hash;
  uint32_t len;
  Value value;
  uint8_t is_bound;
  struct Symbol *next; // For collision resolution
};

struct SymbolTable {
  Symbol *buckets[SYMBOL_TABLE_SIZE];
  Arena *arena; // Symbols allocated from arena
};

// FNV-1a hash (fast)
static inline uint32_t hash_string_n(const char *str, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= (uint8_t)str[i];
    hash *= 16777619u;
  }
  return hash;
}

static void symtab_init(SymbolTable *st, Arena *arena) {
  memset(st->buckets, 0, sizeof(st->buckets));
  st->arena = arena;
}

static Symbol *symtab_intern(SymbolTable *st, const char *name, uint32_t hash,
                             size_t len) {
  // uint32_t idx = hash % SYMBOL_TABLE_SIZE;
  uint32_t idx = hash & (SYMBOL_TABLE_SIZE - 1);
  for (Symbol *s = st->buckets[idx]; s; s = s->next) {
    if (s->hash == hash &&
        s->len == (uint32_t)len && // was: strlen(s->name) == len
        memcmp(s->name, name, len) == 0) {
      return s;
    }
  }

  Symbol *sym = (Symbol *)arena_alloc(st->arena, sizeof(Symbol));
  if (!sym)
    return NULL;

  char *name_copy = (char *)arena_alloc(st->arena, len + 1);
  if (!name_copy)
    return NULL;
  memcpy(name_copy, name, len);
  name_copy[len] = '\0';

  sym->name = name_copy;
  sym->hash = hash;
  sym->len = (uint32_t)len; // ← store it once at intern time
  sym->value = val_int(0);
  sym->is_bound = 0;
  sym->next = st->buckets[idx];
  st->buckets[idx] = sym;

  return sym;
}
static inline Value sym_lookup(Symbol *sym) {
  return sym->is_bound ? sym->value : val_int(0);
}

static inline void sym_bind(Symbol *sym, Value value) {
  sym->value = value;
  sym->is_bound = 1;
}

static inline void sym_unbind(Symbol *sym) { sym->is_bound = 0; }

// ============================================================================
// Closure
// ============================================================================
#define NANBOX_CLO_TAG 0xFFFA000000000000ULL
#define NANBOX_CLO_MASK 0xFFFF000000000000ULL
#define NANBOX_CLO_PAYLOAD_MASK 0x0000FFFFFFFFFFFFULL

typedef struct GCHeader {
  uint32_t tag;
  uint32_t size;
  uintptr_t forward;
} GCHeader;

typedef struct Closure {
  GCHeader header;
  int param_count;
  int capture_count;
  Symbol **params;
  Symbol **captured_syms;
  Value *captured_vals;
  Node *body;
} Closure;

static inline void gc_fatal(const char *msg);

static inline int gc_nanbox_ptr_fits(const void *p) {
  return (((uintptr_t)p) & ~NANBOX_CLO_PAYLOAD_MASK) == 0;
}

static inline Value val_closure(Closure *c) {
#ifndef NDEBUG
  if (!gc_nanbox_ptr_fits(c)) {
    gc_fatal("closure pointer does not fit in NaN-box payload");
  }
#endif
  return NANBOX_CLO_TAG | ((uint64_t)(uintptr_t)c & NANBOX_CLO_PAYLOAD_MASK);
}

static inline Closure *val_to_closure(Value v) {
  return (Closure *)(uintptr_t)(v & NANBOX_CLO_PAYLOAD_MASK);
}

static inline int val_is_closure(Value v) {
  return (v & NANBOX_CLO_MASK) == NANBOX_CLO_TAG;
}

// ============================================================================
// GC
// ============================================================================
#ifndef GC_HEAP_SIZE
#define GC_HEAP_SIZE (4u * 1024u * 1024u)
#endif

#define GC_TAG_CLOSURE 1
#define GC_TAG_BIGNUM 2
#define GC_TAG_RATIONAL 3
#define GC_TAG_PAIR 4
#define ALIGN8(x) (((x) + 7) & ~7)

typedef struct GCHeap {
  Arena from;
  Arena to;
  size_t threshold;
} GCHeap;

typedef struct ShadowFrame {
  Value **roots;
  int count;
  struct ShadowFrame *prev;
} ShadowFrame;

typedef struct ShadowStack {
  ShadowFrame *top;
} ShadowStack;

#define GC_CONCAT2(a, b) a##b
#define GC_CONCAT(a, b) GC_CONCAT2(a, b)
#ifdef __COUNTER__
#define GC_UNIQUE_ID __COUNTER__
#else
#define GC_UNIQUE_ID __LINE__
#endif
#define GC_PUSH_ROOTS_IMPL(ss, id, ...)                                        \
  Value *GC_CONCAT(_gc_roots_, id)[] = {__VA_ARGS__};                          \
  ShadowFrame GC_CONCAT(_gc_frame_, id) = {                                    \
      .roots = GC_CONCAT(_gc_roots_, id),                                      \
      .count = (int)(sizeof(GC_CONCAT(_gc_roots_, id)) /                       \
                     sizeof(GC_CONCAT(_gc_roots_, id)[0])),                    \
      .prev = (ss)->top};                                                      \
  (ss)->top = &GC_CONCAT(_gc_frame_, id)
#define GC_PUSH_ROOTS(ss, ...) GC_PUSH_ROOTS_IMPL(ss, GC_UNIQUE_ID, __VA_ARGS__)
#define GC_POP_ROOTS(ss) ((ss)->top = (ss)->top->prev)

static inline void gc_fatal(const char *msg) {
  fprintf(stderr, "GC fatal: %s\n", msg);
  abort();
}

static inline void gc_heap_init(GCHeap *heap) {
  arena_init(&heap->from, GC_HEAP_SIZE);
  arena_init(&heap->to, GC_HEAP_SIZE);
  heap->threshold = GC_HEAP_SIZE - (GC_HEAP_SIZE / 4);
}

static inline void gc_heap_free(GCHeap *heap) {
  arena_free(&heap->from);
  arena_free(&heap->to);
}

static inline void gc_copy(GCHeap *heap, Value *slot);

static inline int gc_valid_object_size(const GCHeader *header) {
  if (!header)
    return 0;
  if (header->size < sizeof(GCHeader))
    return 0;
  if ((header->size & 7u) != 0)
    return 0;
  if (header->size > GC_HEAP_SIZE)
    return 0;
  if (header->tag != GC_TAG_CLOSURE && header->tag != GC_TAG_BIGNUM &&
      header->tag != GC_TAG_RATIONAL && header->tag != GC_TAG_PAIR)
    return 0;
  return 1;
}

static inline size_t gc_closure_max_captures(const GCHeader *header) {
  if (!header || header->size < sizeof(Closure))
    return 0;
  return (header->size - sizeof(Closure)) / sizeof(Value);
}

static inline void gc_validate_closure(const GCHeader *header,
                                       const Closure *c) {
  if (header->size < sizeof(Closure)) {
    gc_fatal("closure size too small");
  }
  size_t max_caps = gc_closure_max_captures(header);
  if (c->capture_count < 0 || (size_t)c->capture_count > max_caps) {
    gc_fatal("closure capture_count out of bounds");
  }
}

static inline void gc_scan_closure(GCHeap *heap, Closure *c) {
  for (int i = 0; i < c->capture_count; i++) {
    gc_copy(heap, &c->captured_vals[i]);
  }
}

static inline void gc_scan_object(GCHeap *heap, void *obj) {
  GCHeader *header = (GCHeader *)obj;
  if (!gc_valid_object_size(header)) {
    gc_fatal("invalid GC object size");
  }
  switch (header->tag) {
  case GC_TAG_CLOSURE:
    gc_validate_closure(header, (Closure *)obj);
    gc_scan_closure(heap, (Closure *)obj);
    break;
  case GC_TAG_BIGNUM:
  case GC_TAG_RATIONAL:
  case GC_TAG_PAIR:
  default:
    break;
  }
}

static inline void gc_copy(GCHeap *heap, Value *slot) {
  if (!slot || !val_is_closure(*slot))
    return;

  Closure *obj = val_to_closure(*slot);
  GCHeader *header = &obj->header;

  if (!gc_valid_object_size(header)) {
    gc_fatal("invalid GC object size");
  }
  gc_validate_closure(header, obj);

  if (header->forward != 0) {
    *slot = val_closure((Closure *)(uintptr_t)header->forward);
    return;
  }

  void *copy_mem = arena_alloc(&heap->to, (size_t)header->size);
  if (!copy_mem) {
    gc_fatal("GC to-space exhausted during copy");
  }

  memcpy(copy_mem, obj, (size_t)header->size);
  header->forward = (uintptr_t)copy_mem;

  Closure *copy = (Closure *)copy_mem;
  copy->header.forward = 0;
  copy->captured_vals = (Value *)((char *)copy + sizeof(Closure));

  *slot = val_closure(copy);
}

static inline void gc_collect(GCHeap *heap, ShadowStack *ss,
                              SymbolTable *symtab) {
  for (ShadowFrame *frame = ss ? ss->top : NULL; frame; frame = frame->prev) {
    for (int i = 0; i < frame->count; i++) {
      gc_copy(heap, frame->roots[i]);
    }
  }

  for (int i = 0; i < SYMBOL_TABLE_SIZE; i++) {
    for (Symbol *sym = symtab->buckets[i]; sym; sym = sym->next) {
      if (sym->is_bound) {
        gc_copy(heap, &sym->value);
      }
    }
  }

  char *scan = heap->to.ptr;
  while (scan < heap->to.ptr + heap->to.offset) {
    GCHeader *header = (GCHeader *)scan;
    gc_scan_object(heap, scan);
    scan += ALIGN8(header->size);
  }

  Arena old_from = heap->from;
  heap->from = heap->to;
  heap->to = old_from;
  arena_reset(&heap->to);
}

static inline Value gc_alloc_raw(GCHeap *heap, ShadowStack *ss,
                                 SymbolTable *symtab, uint32_t tag,
                                 size_t size) {
  if (size > UINT32_MAX) {
    gc_fatal("GC object too large");
  }
  size = ALIGN8(size);
  if (size < sizeof(GCHeader)) {
    gc_fatal("GC object size too small");
  }

  if (heap->from.offset + size > heap->threshold) {
    gc_collect(heap, ss, symtab);
  }

  void *mem = arena_alloc(&heap->from, size);
  if (!mem) {
    gc_collect(heap, ss, symtab);
    mem = arena_alloc(&heap->from, size);
    if (!mem) {
      gc_fatal("GC heap exhausted");
    }
  }

  GCHeader *header = (GCHeader *)mem;
  header->tag = tag;
  header->size = (uint32_t)size;
  header->forward = 0;

  if (tag == GC_TAG_CLOSURE) {
    return val_closure((Closure *)mem);
  }

  gc_fatal("GC tag not implemented");
  return val_int(0);
}

static inline Closure *gc_alloc_closure(GCHeap *heap, ShadowStack *ss,
                                        SymbolTable *symtab, int param_count,
                                        int capture_count) {
  size_t size = ALIGN8(sizeof(Closure) + (size_t)capture_count * sizeof(Value));
  Closure *c =
      val_to_closure(gc_alloc_raw(heap, ss, symtab, GC_TAG_CLOSURE, size));
  c->param_count = param_count;
  c->capture_count = capture_count;
  c->params = NULL;
  c->captured_syms = NULL;
  c->captured_vals = (Value *)((char *)c + sizeof(Closure));
  c->body = NULL;
  return c;
}

// ============================================================================
// Binding Stack (Fast Unwind)
// ============================================================================
typedef struct {
  uintptr_t sym_and_flag;
  Value old_value;
} BindEntry;

struct BindingStack {
  BindEntry *entries;
  int capacity;
  int sp;
};

// Encode/decode helpers
static inline uintptr_t bs_encode(Symbol *sym, uint8_t was_bound) {
  return (uintptr_t)sym | (uintptr_t)(was_bound & 1);
}
static inline Symbol *bs_sym(uintptr_t enc) {
  return (Symbol *)(enc & ~(uintptr_t)1);
}
static inline uint8_t bs_was_bound(uintptr_t enc) { return (uint8_t)(enc & 1); }

static void bindstack_init(BindingStack *bs, int capacity) {
  bs->entries = (BindEntry *)malloc(capacity * sizeof(BindEntry));
  bs->capacity = capacity;
  bs->sp = 0;
}

static void bindstack_push(BindingStack *bs, Symbol *sym, Value new_value) {
  if (bs->sp >= bs->capacity) { /* grow or fatal */
    return;
  }
  bs->entries[bs->sp].sym_and_flag = bs_encode(sym, sym->is_bound);
  bs->entries[bs->sp].old_value = sym->value;
  bs->sp++;
  sym->value = new_value;
  sym->is_bound = 1;
}

static void bindstack_pop_n(BindingStack *bs, int n) {
  int new_sp = bs->sp - n;
  for (int i = new_sp; i < bs->sp; i++) {
    Symbol *sym = bs_sym(bs->entries[i].sym_and_flag);
    sym->value = bs->entries[i].old_value;
    sym->is_bound = bs_was_bound(bs->entries[i].sym_and_flag);
  }
  bs->sp = new_sp;
}

static void bindstack_free(BindingStack *bs) { free(bs->entries); }

// ============================================================================
// Parser (Token-based)
// ============================================================================
#define CC_SPACE 0x01u
#define CC_DIGIT 0x02u
#define CC_SYMBOL 0x04u

typedef enum {
  TOKEN_LPAREN,
  TOKEN_RPAREN,
  TOKEN_SYMBOL,
  TOKEN_INTEGER,
  TOKEN_EOF,
  TOKEN_ERROR
} TokenType;

struct Token {
  TokenType type;
  const char *start;
  const char *end;
  int64_t int_value;
  uint32_t hash; // pre-computed FNV-1a hash; valid when type == TOKEN_SYMBOL
};

struct Lexer {
  const char *input;
  const char *pos;
  Arena *arena;
  SymbolTable *symtab;
};

static const uint8_t char_class[256] = {
    [' '] = CC_SPACE,
    ['\t'] = CC_SPACE,
    ['\n'] = CC_SPACE,
    ['\r'] = CC_SPACE,

    ['0'] = CC_DIGIT | CC_SYMBOL,
    ['1'] = CC_DIGIT | CC_SYMBOL,
    ['2'] = CC_DIGIT | CC_SYMBOL,
    ['3'] = CC_DIGIT | CC_SYMBOL,
    ['4'] = CC_DIGIT | CC_SYMBOL,
    ['5'] = CC_DIGIT | CC_SYMBOL,
    ['6'] = CC_DIGIT | CC_SYMBOL,
    ['7'] = CC_DIGIT | CC_SYMBOL,
    ['8'] = CC_DIGIT | CC_SYMBOL,
    ['9'] = CC_DIGIT | CC_SYMBOL,

    ['a'] = CC_SYMBOL,
    ['b'] = CC_SYMBOL,
    ['c'] = CC_SYMBOL,
    ['d'] = CC_SYMBOL,
    ['e'] = CC_SYMBOL,
    ['f'] = CC_SYMBOL,
    ['g'] = CC_SYMBOL,
    ['h'] = CC_SYMBOL,
    ['i'] = CC_SYMBOL,
    ['j'] = CC_SYMBOL,
    ['k'] = CC_SYMBOL,
    ['l'] = CC_SYMBOL,
    ['m'] = CC_SYMBOL,
    ['n'] = CC_SYMBOL,
    ['o'] = CC_SYMBOL,
    ['p'] = CC_SYMBOL,
    ['q'] = CC_SYMBOL,
    ['r'] = CC_SYMBOL,
    ['s'] = CC_SYMBOL,
    ['t'] = CC_SYMBOL,
    ['u'] = CC_SYMBOL,
    ['v'] = CC_SYMBOL,
    ['w'] = CC_SYMBOL,
    ['x'] = CC_SYMBOL,
    ['y'] = CC_SYMBOL,
    ['z'] = CC_SYMBOL,

    ['A'] = CC_SYMBOL,
    ['B'] = CC_SYMBOL,
    ['C'] = CC_SYMBOL,
    ['D'] = CC_SYMBOL,
    ['E'] = CC_SYMBOL,
    ['F'] = CC_SYMBOL,
    ['G'] = CC_SYMBOL,
    ['H'] = CC_SYMBOL,
    ['I'] = CC_SYMBOL,
    ['J'] = CC_SYMBOL,
    ['K'] = CC_SYMBOL,
    ['L'] = CC_SYMBOL,
    ['M'] = CC_SYMBOL,
    ['N'] = CC_SYMBOL,
    ['O'] = CC_SYMBOL,
    ['P'] = CC_SYMBOL,
    ['Q'] = CC_SYMBOL,
    ['R'] = CC_SYMBOL,
    ['S'] = CC_SYMBOL,
    ['T'] = CC_SYMBOL,
    ['U'] = CC_SYMBOL,
    ['V'] = CC_SYMBOL,
    ['W'] = CC_SYMBOL,
    ['X'] = CC_SYMBOL,
    ['Y'] = CC_SYMBOL,
    ['Z'] = CC_SYMBOL,

    ['-'] = CC_SYMBOL,
    ['_'] = CC_SYMBOL,
    ['*'] = CC_SYMBOL,
};

static inline int is_space(char c) { return char_class[(uint8_t)c] & CC_SPACE; }
static inline int is_digit(char c) { return char_class[(uint8_t)c] & CC_DIGIT; }
static inline int is_symbol_char(char c) {
  return char_class[(uint8_t)c] & CC_SYMBOL;
}

// Fast integer parsing
static inline int64_t parse_int(const char *start, const char *end) {
  int64_t result = 0;
  int neg = 0;

  if (*start == '-') {
    neg = 1;
    start++;
  }

  for (const char *p = start; p < end; p++) {
    result = result * 10 + (*p - '0');
  }

  return neg ? -result : result;
}

static Token lexer_next(Lexer *l) {
  Token t;
  t.type = TOKEN_ERROR;

  if (!l || !l->pos)
    return t;

  // Skip whitespace
  while (l->pos && is_space(*l->pos))
    l->pos++;

  if (!l->pos || *l->pos == '\0') {
    t.type = TOKEN_EOF;
    return t;
  }

  if (*l->pos == '(') {
    t.type = TOKEN_LPAREN;
    l->pos++;
    return t;
  }

  if (*l->pos == ')') {
    t.type = TOKEN_RPAREN;
    l->pos++;
    return t;
  }

  if (is_digit(*l->pos) || (*l->pos == '-' && is_digit(l->pos[1]))) {
    t.type = TOKEN_INTEGER;
    t.start = l->pos;

    // Skip the digits
    if (*l->pos == '-')
      l->pos++;
    while (is_digit(*l->pos))
      l->pos++;
    t.end = l->pos;
    t.int_value = parse_int(t.start, t.end);
    return t;
  }

  if (is_symbol_char(*l->pos)) {
    t.type = TOKEN_SYMBOL;
    t.start = l->pos;
    uint32_t h = 2166136261u;
    while (is_symbol_char(*l->pos)) {
      h ^= (uint8_t)*l->pos;
      h *= 16777619u;
      l->pos++;
    }
    t.end = l->pos;
    t.hash = h;
    return t;
  }

  t.type = TOKEN_ERROR;
  return t;
}

// ============================================================================
// AST Node (Compact representation)
// ============================================================================
typedef enum { NODE_LET, NODE_SYMBOL, NODE_INTEGER } NodeType;

struct Node {
  NodeType type;
  union {
    struct {
      Symbol **symbols; // Pre-resolved during parse
      Node **value_exprs;
      int binding_count;
      struct Node *body;
    } let;
    struct {
      Symbol *sym; // Pre-resolved symbol
    } symbol;
    struct {
      int64_t value;
    } integer;
  };
};

// Forward declaration for parse_expr
static Node *parse_expr(Lexer *l, BindingStack *bs);

// Parse let expression: (let (symbol value) body) or
// (let ((symbol value)(symbol value) ...) body)
static Node *parse_let(Lexer *l, BindingStack *bs) {
  Symbol *symbols[MAX_BINDINGS];
  Node *value_exprs[MAX_BINDINGS];
  int binding_count = 0;

  Token t = lexer_next(l); // '(' opening the bindings list
  if (t.type != TOKEN_LPAREN)
    return NULL;

  // Peek at the first token to distinguish single vs multi binding form.
  Token peek = lexer_next(l);

  if (peek.type == TOKEN_LPAREN) {
    // Multi-binding form: ((sym val) (sym val) ...) body
    while (1) {
      // peek holds the current '(' or ')' of the binding group
      if (peek.type == TOKEN_RPAREN)
        break;
      if (peek.type != TOKEN_LPAREN)
        return NULL;

      t = lexer_next(l);
      if (t.type != TOKEN_SYMBOL)
        return NULL;
      size_t len = t.end - t.start;
      Symbol *sym = symtab_intern(l->symtab, t.start, t.hash, len);
      if (!sym)
        return NULL;

      Node *val_expr = parse_expr(l, bs);
      if (!val_expr)
        return NULL;

      t = lexer_next(l);
      if (t.type != TOKEN_RPAREN)
        return NULL;

      symbols[binding_count] = sym;
      value_exprs[binding_count] = val_expr;
      binding_count++;

      peek = lexer_next(l); // next binding '(' or closing ')'
    }
  } else {
    // Single-binding form: (sym val) body — peek is the symbol token
    if (peek.type != TOKEN_SYMBOL)
      return NULL;
    size_t len = peek.end - peek.start;
    Symbol *sym = symtab_intern(l->symtab, peek.start, peek.hash, len);
    if (!sym)
      return NULL;

    Node *val_expr = parse_expr(l, bs);
    if (!val_expr)
      return NULL;

    t = lexer_next(l); // ')' closing the binding pair
    if (t.type != TOKEN_RPAREN)
      return NULL;

    symbols[0] = sym;
    value_exprs[0] = val_expr;
    binding_count = 1;
  }

  Node *body = parse_expr(l, bs);
  if (!body)
    return NULL;

  Node *node = (Node *)arena_alloc(
      l->arena,
      sizeof(Node) + binding_count * (sizeof(Symbol *) + sizeof(Node *)));
  if (!node)
    return NULL;

  node->type = NODE_LET;
  node->let.binding_count = binding_count;
  node->let.body = body;
  node->let.symbols = (Symbol **)(node + 1);
  node->let.value_exprs = (Node **)(node->let.symbols + binding_count);

  for (int i = 0; i < binding_count; i++) {
    node->let.symbols[i] = symbols[i];
    node->let.value_exprs[i] = value_exprs[i];
  }

  return node;
}

// ============================================================================
// Parser
// ============================================================================
static Node *parse_expr(Lexer *l, BindingStack *bs) {
  Token t = lexer_next(l);

  if (t.type == TOKEN_INTEGER) {
    Node *node = (Node *)arena_alloc(l->arena, sizeof(Node));
    if (!node)
      return NULL;
    node->type = NODE_INTEGER;
    node->integer.value = t.int_value;
    return node;
  }

  if (t.type == TOKEN_SYMBOL) {
    // Plain symbol reference — 'let' only valid inside '(' context
    size_t len = t.end - t.start;
    Symbol *sym = symtab_intern(l->symtab, t.start, t.hash, len);
    if (!sym)
      return NULL;
    Node *node = (Node *)arena_alloc(l->arena, sizeof(Node));
    if (!node)
      return NULL;
    node->type = NODE_SYMBOL;
    node->symbol.sym = sym;
    return node;
  }

  if (t.type == TOKEN_LPAREN) {
    // Peek: is this a 'let' form?
    const char *save_pos = l->pos;
    Token peek = lexer_next(l);

    if (peek.type == TOKEN_SYMBOL && (peek.end - peek.start) == 3 &&
        memcmp(peek.start, "let", 3) == 0) {
      // '(' and 'let' consumed; parse_let handles the rest
      Node *node = parse_let(l, bs);
      if (!node)
        return NULL;
      t = lexer_next(l); // consume the outer ')'
      if (t.type != TOKEN_RPAREN)
        return NULL;
      return node;
    }

    // Generic parenthesized expression — restore and recurse
    l->pos = save_pos;
    Node *node = parse_expr(l, bs);
    if (!node)
      return NULL;
    t = lexer_next(l);
    if (t.type != TOKEN_RPAREN)
      return NULL;
    return node;
  }

  return NULL;
}

// ============================================================================
// Evaluator
// ============================================================================
static Value eval(Node *node, BindingStack *bs) {
  switch (node->type) {
  case NODE_INTEGER:
    return val_int(node->integer.value);

  case NODE_SYMBOL: {
    Value val = sym_lookup(node->symbol.sym);
    return val;
  }

  case NODE_LET: {
    int n = node->let.binding_count;
    for (int i = 0; i < n; i++) {
      Value val = eval(node->let.value_exprs[i], bs);
      bindstack_push(bs, node->let.symbols[i], val);
    }
    Value result = eval(node->let.body, bs);
    bindstack_pop_n(bs, n);
    return result;
  }
  }
  return val_int(0);
}

#ifdef __cplusplus
}
#endif

#endif
