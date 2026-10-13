#define _GNU_SOURCE
#define main olly_program_main
#include "../olly.c"
#undef main

#include <stdio.h>
#include <time.h>

static double seconds_since(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) +
      (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static void clear_buffer(void) {
  int i;
  for (i = 0; i < E.numrows; i++) editor_free_row(&E.row[i]);
  free(E.row);
  E.row = NULL;
  E.numrows = 0;
  E.rowcap = 0;
  E.cx = E.cy = E.rx = 0;
  E.eol_crlf = 0;
  E.final_newline = 1;
  E.dirty = 0;
}

static void load_rows(int count) {
  static char line[] = "alpha beta gamma delta";
  int i;
  for (i = 0; i < count; i++) editor_insert_row(E.numrows, line, strlen(line));
}

int main(void) {
  struct timespec a, b;
  const int rows = 10000;

  clear_buffer();
  clock_gettime(CLOCK_MONOTONIC, &a);
  load_rows(rows);
  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("load_rows rows=%d seconds=%.6f\n", rows, seconds_since(a, b));

  E.cy = 0;
  E.cx = 0;
  clock_gettime(CLOCK_MONOTONIC, &a);
  for (int i = 0; i < 1000; i++) {
    E.cy = 0;
    E.cx = 0;
    if (!editor_search_string("not-present", 11, 1, 0, SEARCH_WRAP)) {
      /* Expected: scan the complete buffer without a match. */
    } else return 2;
  }
  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("search rows=%d iterations=1000 seconds=%.6f\n",
      rows, seconds_since(a, b));

  clear_buffer();
  load_rows(rows);
  E.cy = 0;
  E.cx = 0;
  clock_gettime(CLOCK_MONOTONIC, &a);
  int replaced = 0;
  while (editor_search_string("gamma", 5, 1, 0, SEARCH_NO_WRAP)) {
    erow *row = &E.row[E.cy];
    editor_row_delete_n(row, E.cx, 5);
    editor_row_insert_n(row, E.cx, "GAMMA", 5);
    E.cx += 5;
    replaced++;
  }
  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("replace rows=%d matches=%d seconds=%.6f\n",
      rows, replaced, seconds_since(a, b));

  clear_buffer();
  return 0;
}
