#include "seekdb_cuvs_bridge.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
  if (argc != 3) return 2;
  const long count = 10000;
  const long dimension = 32;
  std::vector<float> vectors(count * dimension);
  for (long row = 0; row < count; ++row) {
    for (long column = 0; column < dimension; ++column) {
      vectors[row * dimension + column] = std::sin(float(row * 31 + column * 17));
    }
  }
  void *index = nullptr;
  if (std::strcmp(argv[1], "graph") == 0) {
    // OBCUVS03 path: copy the graph out of a built index, rebuild from host data + graph.
    if (seekdb_cuvs_warmup(dimension) != 0) return 8;
    void *built = seekdb_cuvs_build(vectors.data(), count, dimension);
    const long degree = built ? seekdb_cuvs_graph_degree(built) : -1;
    if (degree <= 0) return 9;
    std::vector<unsigned int> graph(count * degree);
    if (seekdb_cuvs_copy_graph(built, graph.data()) != 0) return 10;
    seekdb_cuvs_free(built);
    for (unsigned int edge : graph) {
      if (edge >= static_cast<unsigned int>(count)) return 11;
    }
    index = seekdb_cuvs_from_graph(vectors.data(), count, dimension, graph.data(), degree);
    if (!index) return 12;
  } else if (std::strcmp(argv[1], "write") == 0) {
    index = seekdb_cuvs_build(vectors.data(), count, dimension);
    if (!index || seekdb_cuvs_serialize(index, argv[2]) != 0) return 3;
  } else {
    void *invalid = seekdb_cuvs_deserialize(argv[2], count - 1, dimension);
    if (invalid) { seekdb_cuvs_free(invalid); return 6; }
    invalid = seekdb_cuvs_deserialize(argv[2], count, dimension + 1);
    if (invalid) { seekdb_cuvs_free(invalid); return 7; }
    index = seekdb_cuvs_deserialize(argv[2], count, dimension);
    if (!index) return 4;
  }
  unsigned int ids[10];
  float distances[10];
  int result = seekdb_cuvs_search(index, vectors.data(), 1, 10, ids, distances);
  seekdb_cuvs_free(index);
  if (result != 0 || ids[0] != 0 || std::fabs(distances[0]) > 0.0001f) return 5;
  std::printf("%s: n=%ld top1=%u distance=%g\n", argv[1], count, ids[0], distances[0]);
  return 0;
}
