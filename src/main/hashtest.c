#include "global.h"
#include <time.h> // srand

#include "cmd.h"
#include "util.h"
#include "db_graph.h"
#include "binary_kmer.h"

static const char usage[] =
"usage: hashtest [options] <num_ops>\n"
"  Test hash table speed.  Assume kemr size of "QUOTE_VALUE(MAX_KMER_SIZE)" if none given\n";

int main(int argc, char **argv)
{
  CmdArgs args;
  cmd_alloc(&args, argc, argv);

  if(args.argc != 1) print_usage(usage, NULL);

  argv = args.argv;
  argc = args.argc;

  unsigned long i, num_ops;
  if(!parse_entire_ulong(argv[0], &num_ops))
    print_usage(usage, "Invalid <num_ops>");

  /* initialize random seed: */
  srand(time(NULL));

  size_t kmers_in_hash = cmd_get_kmers_in_hash(&args, 0);

  dBGraph db_graph;
  db_graph_alloc(&db_graph, args.kmer_size, 1, kmers_in_hash);
  hash_table_print_stats(&db_graph.ht);

  BinaryKmer bkmer;
  boolean found;

  for(i = 0; i < num_ops; i++)
  {
    bkmer = binary_kmer_random(args.kmer_size);
    hash_table_find_or_insert(&db_graph.ht, bkmer, &found);
  }

  hash_table_print_stats(&db_graph.ht);
  db_graph_dealloc(&db_graph);

  cmd_free(&args);
  return EXIT_SUCCESS;
}