#include "global.h"
#include "util.h"
#include "binary_kmer.h"
#include "hash_table.h"
#include "db_graph.h"
#include "db_node.h"
#include "supernode.h"

static bool supernode_is_closed_cycle(const dBNode *nlist, size_t len,
                                         BinaryKmer bkmer0, BinaryKmer bkmer1,
                                         const dBGraph *db_graph)
{
  Edges edges0, edges1;
  BinaryKmer shiftkmer;
  Nucleotide nuc;
  const size_t kmer_size = db_graph->kmer_size;

  edges0 = db_node_get_edges_union(db_graph, nlist[0].key);
  if(edges_get_indegree(edges0, nlist[0].orient) != 1) return false;

  edges1 = db_node_get_edges_union(db_graph, nlist[len-1].key);
  if(edges_get_outdegree(edges1, nlist[len-1].orient) != 1) return false;

  nuc = bkmer_get_last_nuc(bkmer0, nlist[0].orient, kmer_size);
  shiftkmer = bkmer_shift_add_last_nuc(bkmer1, nlist[len-1].orient, kmer_size, nuc);

  if(binary_kmers_are_equal(bkmer0, shiftkmer)) return true;

  shiftkmer = binary_kmer_reverse_complement(shiftkmer, kmer_size);
  return binary_kmers_are_equal(bkmer0, shiftkmer);
}

// Orient supernode
// Once oriented, supernode has lowest possible kmerkey at the beginning,
// oriented FORWARDs if possible
void supernode_normalise(dBNode *nlist, size_t len, const dBGraph *db_graph)
{
  // Sort supernode into forward orientation
  ctx_assert(len > 0);

  if(len == 1) {
    nlist[0].orient = FORWARD;
    return;
  }

  BinaryKmer bkmer0 = db_node_get_bkmer(db_graph, nlist[0].key);
  BinaryKmer bkmer1 = db_node_get_bkmer(db_graph, nlist[len-1].key);

  // Check if closed cycle
  if(supernode_is_closed_cycle(nlist, len, bkmer0, bkmer1, db_graph))
  {
    // find lowest kmer to start from
    BinaryKmer lowest = bkmer0, tmp;
    size_t i, idx = 0;
    for(i = 1; i < len; i++) {
      tmp = db_node_get_bkmer(db_graph, nlist[i].key);
      if(binary_kmer_less_than(tmp, lowest)) {
        lowest = tmp;
        idx = i;
      }
    }

    // If already starting from the lowest kmer no change needed
    if(idx > 0 || nlist[0].orient != FORWARD)
    {
      // a->b->c->d->e->f->a
      // if c is lowest and FORWARD:  c->d->e->f->a->b (keep orientations)
      // if c is lowest and REVERSE:  c->b->a->f->e->d (reverse orientations)

      if(nlist[idx].orient == FORWARD) {
        // Shift left by idx, without affecting orientations
        db_nodes_left_shift(nlist, len, idx);
      } else {
        db_nodes_reverse_complement(nlist, idx+1);
        db_nodes_reverse_complement(nlist+idx+1, len-idx-1);
      }
    }
  }
  else if(binary_kmer_less_than(bkmer1,bkmer0)) {
    db_nodes_reverse_complement(nlist, len);
  }
}

// Extend a supernode, nlist[offset] must already be set
// Walk along nodes starting from node/or, storing the supernode in nlist
// Returns the number of nodes added, adds no more than `limit`
// return false if out of space and limit > 0
bool supernode_extend(dBNodeBuffer *nbuf, size_t limit,
                         const dBGraph *db_graph)
{
  ctx_assert(nbuf->len > 0);

  const size_t kmer_size = db_graph->kmer_size;
  dBNode node0 = nbuf->data[0], node1 = nbuf->data[nbuf->len-1], node = node1;

  BinaryKmer bkmer = db_node_oriented_bkmer(db_graph, node);
  Edges edges = db_node_get_edges_union(db_graph, node.key);
  Nucleotide nuc;

  while(edges_has_precisely_one_edge(edges, node.orient, &nuc))
  {
    bkmer = binary_kmer_left_shift_add(bkmer, kmer_size, nuc);
    node = db_graph_find(db_graph, bkmer);
    edges = db_node_get_edges_union(db_graph, node.key);

    ctx_assert(node.key != HASH_NOT_FOUND);

    if(edges_has_precisely_one_edge(edges, rev_orient(node.orient), &nuc))
    {
      if(node.key == node0.key || node.key == nbuf->data[nbuf->len-1].key) {
        // don't create a loop A->B->A or a->b->B->A
        break;
      }

      if(limit && nbuf->len >= limit) return false;

      db_node_buf_add(nbuf, node);
    }
    else break;
  }

  return true;
}

void supernode_find(hkey_t hkey, dBNodeBuffer *nbuf, const dBGraph *db_graph)
{
  dBNode first = {.key = hkey, .orient = REVERSE};
  size_t offset = nbuf->len;
  db_node_buf_add(nbuf, first);
  supernode_extend(nbuf, 0, db_graph);
  db_nodes_reverse_complement(nbuf->data+offset, nbuf->len-offset);
  supernode_extend(nbuf, 0, db_graph);
}

// Count number of read starts using coverage data
uint32_t supernode_read_starts(const uint32_t *covgs, uint32_t len)
{
  if(len == 0) return 0;
  if(len == 1) return covgs[0];

  uint32_t i, read_starts = covgs[0];

  for(i = 1; i+1 < len; i++)
  {
    if(covgs[i] > covgs[i-1] && covgs[i-1] != covgs[i+1])
      read_starts += covgs[i] - covgs[i-1];
  }

  if(covgs[len-1] > covgs[len-2])
    read_starts += covgs[len-1] - covgs[len-2];

  return read_starts;
}

//
// Multithreaded fetching supernode length histogram
//

static inline void increment_len_hist(const dBNodeBuffer *nbuf, size_t threadid,
                                      void *arg)
{
  (void)threadid;
  uint64_t *arr = (uint64_t*)arg, arrlen, snode_len;

  // First element of arr is its length
  arrlen = arr[0];
  arr++;
  snode_len = MIN2(nbuf->len, arrlen-1);

  // Add to histogram
  __sync_fetch_and_add((volatile uint64_t *)&arr[snode_len], 1);
}

void supernode_write_len_distrib(FILE *fout, const char *path, size_t histlen,
                                 size_t nthreads, uint8_t *visited,
                                 const dBGraph *db_graph)
{
  const size_t kmer_size = db_graph->kmer_size;
  size_t i, end;

  ctx_assert(histlen >= 2);
  status("[supernode] Saving supernode length distribution to: %s", path);
  status("[supernode]   using %zu thread%s", nthreads, util_plural_str(nthreads));

  uint64_t *hist = ctx_calloc(histlen+1, sizeof(uint64_t));
  hist[0] = histlen;
  supernodes_iterate(nthreads, visited, db_graph, increment_len_hist, hist);

  // Write to file
  fprintf(fout, "SupernodeKmerLength,bp,Count\n");
  fprintf(fout, "1,%zu,%"PRIu64"\n", kmer_size, hist[1]);
  for(end = histlen-1; end > 1 && hist[end] == 0; end--);
  for(i = 2; i <= end; i++) {
    if(hist[i] > 0) fprintf(fout, "%zu,%zu,%"PRIu64"\n", i, kmer_size+i-1, hist[i]);
  }

  ctx_free(hist);
}

//
// Iterate over supernodes in the graph with multiple threads
//

static inline void supernode_iterate_node(hkey_t hkey, size_t threadid,
                                          dBNodeBuffer *nbuf,
                                          uint8_t *visited,
                                          const dBGraph *db_graph,
                                          void (*func)(const dBNodeBuffer *_nbuf,
                                                       size_t threadid,
                                                       void *_arg),
                                          void *arg)
{
  bool got_lock = false;
  size_t i;

  if(!bitset_get_mt(visited, hkey))
  {
    db_node_buf_reset(nbuf);
    supernode_find(hkey, nbuf, db_graph);

    // Mark first node as visited
    bitlock_try_acquire(visited, nbuf->data[0].key, &got_lock);

    // Check if someone else marked it first
    if(got_lock)
    {
      // Mark remaining nodes as visited
      for(i = 1; i < nbuf->len; i++)
        bitset_set_mt(visited, nbuf->data[i].key);

      func(nbuf, threadid, arg);
    }
  }
}

typedef struct {
  const size_t threadid, nthreads;
  uint8_t *const visited;
  const dBGraph *db_graph;
  void (*func)(const dBNodeBuffer *_nbuf, size_t threadid, void *_arg);
  void *arg;
} SupernodeIterator;

static void supernodes_iterate_thread(void *arg)
{
  SupernodeIterator cl = *(SupernodeIterator*)arg;

  dBNodeBuffer nbuf;
  db_node_buf_alloc(&nbuf, 2048);

  HASH_ITERATE_PART(&cl.db_graph->ht, cl.threadid, cl.nthreads,
                    supernode_iterate_node,
                    cl.threadid, &nbuf, cl.visited, cl.db_graph,
                    cl.func, cl.arg);

  db_node_buf_dealloc(&nbuf);
}

void supernodes_iterate(size_t nthreads, uint8_t *visited,
                        const dBGraph *db_graph,
                        void (*func)(const dBNodeBuffer *_nbuf,
                                     size_t threadid,
                                     void *_arg),
                        void *arg)
{
  size_t i;
  SupernodeIterator *workers = ctx_calloc(nthreads, sizeof(SupernodeIterator));

  for(i = 0; i < nthreads; i++) {
    SupernodeIterator tmp = {.threadid = i, .nthreads = nthreads,
                             .visited = visited, .db_graph = db_graph,
                             .func = func, .arg = arg};
    memcpy(&workers[i], &tmp, sizeof(SupernodeIterator));
  }

  util_run_threads(workers, nthreads, sizeof(SupernodeIterator),
                   nthreads, supernodes_iterate_thread);

  ctx_free(workers);
}
