#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include <sched.h>
#else
#include <time.h>
#endif

#include "libLisp.h"

static void runtime_init(Arena *arena, Arena *sym_arena, SymbolTable *symtab,
                         BindingStack *bindstack) {
  arena_init(arena, ARENA_SIZE);
  arena_init(sym_arena, 64 * 1024);
  symtab_init(symtab, sym_arena);
  bindstack_init(bindstack, 1024);
}

static void runtime_free(Arena *arena, Arena *sym_arena,
                         BindingStack *bindstack) {
  bindstack_free(bindstack);
  arena_free(arena);
  arena_free(sym_arena);
}

static void pin_to_cpu(int cpu) {
#if defined(__linux__)
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  sched_setaffinity(0, sizeof(mask), &mask);
#else
  (void)cpu;
#endif
}

static inline uint64_t rdtsc_serialized(void) {
#if defined(__linux__)
  unsigned int lo, hi;
  __asm__ __volatile__("cpuid\n\t"
                       "rdtsc\n\t"
                       : "=a"(lo), "=d"(hi)
                       : "a"(0)
                       : "rbx", "rcx");
  return ((uint64_t)hi << 32) | lo;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void benchmark_case(const char *input, int iterations) {
  Arena arena;
  Arena sym_arena;
  SymbolTable symtab;
  BindingStack bindstack;

  // ── PARSE benchmark ──────────────────────────────────────────
  runtime_init(&arena, &sym_arena, &symtab, &bindstack);

  // Warmup: parse only
  for (int i = 0; i < iterations; i++) {
    Lexer l = {input, input, &arena, &symtab};
    parse_expr(&l, &bindstack);
    arena_reset(&arena);
  }

  uint64_t parse_start = rdtsc_serialized();
  for (int i = 0; i < iterations; i++) {
    Lexer l = {input, input, &arena, &symtab};
    parse_expr(&l, &bindstack);
    arena_reset(&arena);
  }
  uint64_t parse_end = rdtsc_serialized();
  printf("%-30s parse cycles: %.1f\n", input,
         (double)(parse_end - parse_start) / iterations);

  runtime_free(&arena, &sym_arena, &bindstack);

  // ── EVAL benchmark ───────────────────────────────────────────
  runtime_init(&arena, &sym_arena, &symtab, &bindstack);

  // Parse once into permanent AST
  Lexer l = {input, input, &arena, &symtab};
  Node *ast = parse_expr(&l, &bindstack);

  // Warmup: eval only
  for (int i = 0; i < iterations; i++) {
    eval(ast, &bindstack);
    bindstack.sp = 0;
  }

  uint64_t eval_start = rdtsc_serialized();
  for (int i = 0; i < iterations; i++) {
    eval(ast, &bindstack);
    bindstack.sp = 0;
  }
  uint64_t eval_end = rdtsc_serialized();
  printf("%-30s eval cycles: %.1f\n", input,
         (double)(eval_end - eval_start) / iterations);

  runtime_free(&arena, &sym_arena, &bindstack);

  // ── PARSE+EVAL benchmark ─────────────────────────────────────
  runtime_init(&arena, &sym_arena, &symtab, &bindstack);

  // Warmup: parse+eval
  for (int i = 0; i < iterations; i++) {
    Lexer l2 = {input, input, &arena, &symtab};
    Node *ast2 = parse_expr(&l2, &bindstack);
    if (ast2)
      eval(ast2, &bindstack);
    arena_reset(&arena);
    bindstack.sp = 0;
  }

  uint64_t pe_start = rdtsc_serialized();
  for (int i = 0; i < iterations; i++) {
    Lexer l2 = {input, input, &arena, &symtab};
    Node *ast2 = parse_expr(&l2, &bindstack);
    if (ast2)
      eval(ast2, &bindstack);
    arena_reset(&arena);
    bindstack.sp = 0;
  }
  uint64_t pe_end = rdtsc_serialized();
  printf("%-30s parse+eval cycles: %.1f\n", input,
         (double)(pe_end - pe_start) / iterations);

  runtime_free(&arena, &sym_arena, &bindstack);
}

static void benchmark(const char *input) {
  pin_to_cpu(3);

  printf("=== Stable Benchmark (100k iterations) ===\n");

  benchmark_case(input, 100000);
}

static void usage(const char *prog) {
  printf("Usage: %s 's-expression'\n", prog);
}

int main(int argc, char **argv) {
  if (argc != 2 || argv[1][0] == '-') {
    usage(argv[0]);
    return argc == 2 ? 1 : 0;
  }

  benchmark(argv[1]);
  return 0;
}
