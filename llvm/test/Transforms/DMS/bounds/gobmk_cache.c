// RUN: clang -fdms=bounds -g -O0 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O1 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O3 %s -o %t && %t
// (we just test that we can compile this with bounds checks and then run it and
// it exits successfully, no bounds-check violations or other crashes)
// (we do test with several different optimization levels)

// this file simplified from engine/cache.c in 445.gobmk

#include <stdlib.h>

typedef struct hashnode_t {
  void *results;
} Hashnode;

typedef struct hashtable {
  int            hashtablesize;	/* Number of hash buckets */
  Hashnode     **hashtable;	/* Pointer to array of hashnode lists */

  int            num_nodes;	/* Total number of hash nodes */
  Hashnode      *all_nodes;	/* Pointer to all allocated hash nodes. */
} Hashtable;

__attribute__((noinline))
void hashtable_clear(Hashtable *table)
{
  int bucket;
  int i;

  if (!table)
    return;

  /* Initialize all hash buckets to the empty list. */
  for (bucket = 0; bucket < table->hashtablesize; ++bucket)
    table->hashtable[bucket] = NULL;

  /* Mark all nodes as free. */
  for (i = 0; i < table->num_nodes; i++)
    table->all_nodes[i].results = NULL;
}

__attribute__((noinline))
int hashtable_init(Hashtable *table, int tablesize, int num_nodes)
{
  /* Allocate memory for the pointers in the hash table proper. */
  table->hashtablesize = tablesize;
  table->hashtable = (Hashnode **) malloc(tablesize * sizeof(Hashnode *));
  if (table->hashtable == NULL) {
    free(table);
    return 0;
  }

  /* Allocate memory for the nodes. */
  table->num_nodes = num_nodes;
  table->all_nodes = (Hashnode *) malloc(num_nodes * sizeof(Hashnode));
  if (table->all_nodes == NULL) {
    free(table->hashtable);
    free(table);
    return 0;
  }

  /* Initialize the table and all nodes to the empty state . */
  hashtable_clear(table);

  return 1;
}

__attribute__((noinline))
Hashtable * hashtable_new(int tablesize, int num_nodes)
{
  Hashtable *table;

  /* Allocate the hashtable struct. */
  table = (Hashtable *) malloc(sizeof(Hashtable));
  if (table == NULL)
    return NULL;

  /* Initialize the table. */
  if (!hashtable_init(table, tablesize, num_nodes)) {
    free(table);
    return NULL;
  }

  return table;
}

int main(int argc, char* argv[]) {
  const int nodes = 400000;
  Hashtable* movehash = hashtable_new((int) 1.5*nodes, (int) nodes);
  if (!movehash) return 1;
  //hashtable_clear(movehash);
  return 0;
}
