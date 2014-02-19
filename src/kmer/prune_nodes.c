#include "global.h"
#include "prune_nodes.h"
#include "util.h"
#include "hash_table.h"
#include "db_graph.h"
#include "db_node.h"

static inline void prune_node_without_edges(dBGraph *db_graph, hkey_t node)
{
  ctx_assert(node != HASH_NOT_FOUND);
  Colour col;

  if(db_graph->col_edges != NULL)
    db_node_zero_edges(db_graph,node);

  if(db_graph->col_covgs != NULL)
    db_node_zero_covgs(db_graph, node);

  if(db_graph->node_in_cols != NULL)
    for(col = 0; col < db_graph->num_of_cols; col++)
      db_node_del_col(db_graph, node, col);

  hash_table_delete(&db_graph->ht, node);
}

// For all flagged nodes, trim edges to non-flagged nodes
// After calling this function on all nodes, call:
// prune_nodes_lacking_flag_no_edges
static void prune_edges_to_nodes_lacking_flag(hkey_t hkey, dBGraph *db_graph,
                                              const uint64_t *flags)
{
  Edges keep_edges = 0x0;
  Orientation orient;
  Nucleotide nuc;
  dBNode next_node;
  BinaryKmer bkmer;
  size_t col;

  if(bitset_get(flags, hkey))
  {
    // Check edges
    bkmer = db_node_get_bkmer(db_graph, hkey);
    keep_edges = db_node_get_edges_union(db_graph, hkey);

    for(orient = 0; orient < 2; orient++)
    {
      for(nuc = 0; nuc < 4; nuc++)
      {
        if(edges_has_edge(keep_edges, nuc, orient))
        {
          next_node = db_graph_next_node(db_graph, bkmer, nuc, orient);

          if(!bitset_get(flags, next_node.key))
          {
            // Next node fails filter - remove edge
            keep_edges = edges_del_edge(keep_edges, nuc, orient);
          }
        }
      }
    }
  }

  for(col = 0; col < db_graph->num_edge_cols; col++)
    db_node_edges(db_graph, col, hkey) &= keep_edges;
}

static void prune_nodes_lacking_flag_no_edges(hkey_t hkey, dBGraph *db_graph,
                                              const uint64_t *flags)
{
  if(!bitset_get(flags, hkey)) {
    // char str[MAX_KMER_SIZE+1];
    // BinaryKmer bkmer = db_node_get_bkmer(db_graph, hkey);
    // binary_kmer_to_str(bkmer, db_graph->kmer_size, str);
    // status("Removing: %s\n", str);
    prune_node_without_edges(db_graph, hkey);
  }
}

// Remove all nodes that do not have a given flag
void prune_nodes_lacking_flag(dBGraph *db_graph, const uint64_t *flags)
{
  // Trim edges from valid nodes
  if(db_graph->col_edges != NULL) {
    HASH_ITERATE(&db_graph->ht, prune_edges_to_nodes_lacking_flag,
                 db_graph, flags);
  }

  // Removed dead nodes
  HASH_ITERATE_SAFE(&db_graph->ht, prune_nodes_lacking_flag_no_edges,
                    db_graph, flags);
}

// Remove all edges in the graph that connect to the given node
static void prune_connecting_edges(dBGraph *db_graph, hkey_t hkey)
{
  Edges uedges = db_node_get_edges_union(db_graph, hkey);
  BinaryKmer bkmer = db_node_get_bkmer(db_graph, hkey);
  dBNode next_node;
  Orientation or;
  Nucleotide nuc, lost_nuc;
  Edges remove_edge_mask;
  size_t col;

  for(or = 0; or < 2; or++)
  {
    // Remove edge from: next_node:!next_or -> hkey:!or
    // when working backwards, or = !or, next_or = !next_or
    // so this is actually if(or == REVERSE)

    if(or == FORWARD) {
      lost_nuc = binary_kmer_first_nuc(bkmer, db_graph->kmer_size);
      lost_nuc = dna_nuc_complement(lost_nuc);
    }
    else lost_nuc = binary_kmer_last_nuc(bkmer);

    for(nuc = 0; nuc < 4; nuc++)
    {
      if(edges_has_edge(uedges, nuc, or))
      {
        next_node = db_graph_next_node(db_graph, bkmer, nuc, or);
        remove_edge_mask = nuc_orient_to_edge(lost_nuc,
                                              rev_orient(next_node.orient));

        // Sanity test
        ctx_assert(next_node.key != HASH_NOT_FOUND);
        ctx_check(next_node.key == hkey ||
          (db_node_get_edges_union(db_graph, next_node.key) & remove_edge_mask)
            == remove_edge_mask);

        for(col = 0; col < db_graph->num_edge_cols; col++)
          db_node_edges(db_graph, col, next_node.key) &= ~remove_edge_mask;
      }
    }
  }
}

void prune_node(dBGraph *db_graph, hkey_t hkey)
{
  prune_connecting_edges(db_graph, hkey);
  prune_node_without_edges(db_graph, hkey);
}

void prune_supernode(dBNode *nodes, size_t len, dBGraph *db_graph)
{
  size_t i;
  if(len == 0) return;

  // Remove connecting nodes to first and last nodes
  prune_connecting_edges(db_graph, nodes[0].key);
  if(len > 1) prune_connecting_edges(db_graph, nodes[len-1].key);

  for(i = 0; i < len; i++) prune_node_without_edges(db_graph, nodes[i].key);
}

// Remove nodes that are in the hash table but not assigned any colours
static void db_graph_remove_node_if_uncoloured(hkey_t node, dBGraph *db_graph)
{
  Colour col = 0;
  while(col < db_graph->num_of_cols && !db_node_has_col(db_graph, node, col))
    col++;

  if(col == db_graph->num_of_cols)
    prune_node(db_graph, node);
}

void prune_uncoloured_nodes(dBGraph *db_graph)
{
  HASH_ITERATE_SAFE(&db_graph->ht, db_graph_remove_node_if_uncoloured, db_graph);
}
