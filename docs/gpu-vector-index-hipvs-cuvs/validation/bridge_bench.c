/* Standalone timing of the seekdb cuVS bridge (no seekdb): build, serialize,
 * deserialize, one-query-at-a-time search like the SQL path, and recall.
 * usage: bridge_bench <data_dir> <dim> [nq] */
#include "seekdb_cuvs_bridge.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void *load(const char *dir, const char *name, size_t *bytes) {
  char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
  FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
  fseek(f, 0, SEEK_END); *bytes = ftell(f); rewind(f);
  void *b = malloc(*bytes);
  if (fread(b, 1, *bytes, f) != *bytes) { perror("read"); exit(1); }
  fclose(f); return b;
}

static double recall(const unsigned *ids, const int64_t *gt, long nq, long k) {
  long hit = 0;
  for (long q = 0; q < nq; q++)
    for (long i = 0; i < k; i++)
      for (long j = 0; j < k; j++)
        if ((int64_t)ids[q * k + i] + 1 == gt[q * 100 + j]) { hit++; break; }
  return (double)hit / (nq * k);
}

int main(int argc, char **argv) {
  const char *dir = argv[1]; long dim = atol(argv[2]); long nq = argc > 3 ? atol(argv[3]) : 4096;
  size_t bb, qb, gb;
  float *base = load(dir, "base.f32", &bb), *query = load(dir, "query.f32", &qb);
  int64_t *gt = load(dir, "gt100.i64", &gb);
  long n = bb / 4 / dim;
  printf("n=%ld dim=%ld nq=%ld\n", n, dim, nq);

  double t = now();
  if (getenv("BB_WARM")) {  /* separate CUDA context init / kernel loading from the build */
    void *w = seekdb_cuvs_build(base, 5000, dim);
    seekdb_cuvs_free(w);
    printf("warmup     %.3f s\n", now() - t);
  }
  t = now();
  void *h = seekdb_cuvs_build(base, n, dim);
  printf("build      %.3f s\n", now() - t);
  if (!h) return 1;

  char path[] = "/tmp/bridge_bench_XXXXXX"; int fd = mkstemp(path); close(fd);
  t = now();
  if (seekdb_cuvs_serialize(h, path)) return 2;
  FILE *f = fopen(path, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
  printf("serialize  %.3f s (%ld MB)\n", now() - t, sz >> 20);
  t = now();
  void *h2 = seekdb_cuvs_deserialize(path, n, dim);
  printf("deserialize %.3f s\n", now() - t);
  unlink(path);
  if (!h2) return 3;

  long ks[2] = {10, 100};
  for (int ki = 0; ki < 2; ki++) {
    long k = ks[ki];
    unsigned *ids = malloc(sizeof(unsigned) * nq * k); float *d = malloc(sizeof(float) * nq * k);
    t = now();
    for (long q = 0; q < nq; q++)
      if (seekdb_cuvs_search(h2, query + q * dim, 1, k, ids + q * k, d + q * k)) return 4;
    double el = now() - t;
    printf("top%-3ld single: %.1f qps  recall=%.4f\n", k, nq / el, recall(ids, gt, nq, k));
    t = now();
    if (seekdb_cuvs_search(h2, query, nq, k, ids, d)) return 5;
    printf("top%-3ld batch : %.3f s  recall=%.4f\n", k, now() - t, recall(ids, gt, nq, k));
    free(ids); free(d);
  }
  seekdb_cuvs_free(h); seekdb_cuvs_free(h2);
  return 0;
}
