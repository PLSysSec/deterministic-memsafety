#!/bin/bash

if [[ $# -lt 1 ]]; then
  echo "Expected an argument: path to the directory containing counts to summarize" >&2
  exit 1
elif ! [[ -d "$1" ]]; then
  echo "error: $1 is not a directory" >&2
  exit 1
else
  counts_dir=$1
fi

get_count_of() {
  rg --no-ignore --no-heading --color=never --threads=12 "$1" $counts_dir | {
    sum=0
    while read line; do
      count=${line##* }
      sum=$(( $sum + $count ))
    done
    echo $sum
  }
}

SCRATCH_DIR=./scratch
mkdir -p $SCRATCH_DIR
get_count_of "Loads with clean addr" >$SCRATCH_DIR/loads_clean_addr.txt &
get_count_of "Loads with blemished16 addr" >$SCRATCH_DIR/loads_blemished16_addr.txt &
get_count_of "Loads with blemished32 addr" >$SCRATCH_DIR/loads_blemished32_addr.txt &
get_count_of "Loads with blemished64 addr" >$SCRATCH_DIR/loads_blemished64_addr.txt &
get_count_of "Loads with blemishedconst addr" >$SCRATCH_DIR/loads_blemishedconst_addr.txt &
get_count_of "Loads with dirty addr" >$SCRATCH_DIR/loads_dirty_addr.txt &
get_count_of "Loads with unknown addr" >$SCRATCH_DIR/loads_unknown_addr.txt &
get_count_of "Stores with clean addr" >$SCRATCH_DIR/stores_clean_addr.txt &
get_count_of "Stores with blemished16 addr" >$SCRATCH_DIR/stores_blemished16_addr.txt &
get_count_of "Stores with blemished32 addr" >$SCRATCH_DIR/stores_blemished32_addr.txt &
get_count_of "Stores with blemished64 addr" >$SCRATCH_DIR/stores_blemished64_addr.txt &
get_count_of "Stores with blemishedconst addr" >$SCRATCH_DIR/stores_blemishedconst_addr.txt &
get_count_of "Stores with dirty addr" >$SCRATCH_DIR/stores_dirty_addr.txt &
get_count_of "Stores with unknown addr" >$SCRATCH_DIR/stores_unknown_addr.txt &
get_count_of "Storing a clean ptr to mem" >$SCRATCH_DIR/stores_clean_val.txt &
get_count_of "Storing a blemished16 ptr to mem" >$SCRATCH_DIR/stores_blemished16_val.txt &
get_count_of "Storing a blemished32 ptr to mem" >$SCRATCH_DIR/stores_blemished32_val.txt &
get_count_of "Storing a blemished64 ptr to mem" >$SCRATCH_DIR/stores_blemished64_val.txt &
get_count_of "Storing a blemishedconst ptr to mem" >$SCRATCH_DIR/stores_blemishedconst_val.txt &
get_count_of "Storing a dirty ptr to mem" >$SCRATCH_DIR/stores_dirty_val.txt &
get_count_of "Storing an unknown ptr to mem" >$SCRATCH_DIR/stores_unknown_val.txt &
get_count_of "Passing a clean ptr to a func" >$SCRATCH_DIR/passing_clean_ptr.txt &
get_count_of "Passing a blemished16 ptr to a func" >$SCRATCH_DIR/passing_blemished16_ptr.txt &
get_count_of "Passing a blemished32 ptr to a func" >$SCRATCH_DIR/passing_blemished32_ptr.txt &
get_count_of "Passing a blemished64 ptr to a func" >$SCRATCH_DIR/passing_blemished64_ptr.txt &
get_count_of "Passing a blemishedconst ptr to a func" >$SCRATCH_DIR/passing_blemishedconst_ptr.txt &
get_count_of "Passing a dirty ptr to a func" >$SCRATCH_DIR/passing_dirty_ptr.txt &
get_count_of "Passing an unknown ptr to a func" >$SCRATCH_DIR/passing_unknown_ptr.txt &
get_count_of "Returning a clean ptr from a func" >$SCRATCH_DIR/returning_clean_ptr.txt &
get_count_of "Returning a blemished16 ptr from a func" >$SCRATCH_DIR/returning_blemished16_ptr.txt &
get_count_of "Returning a blemished32 ptr from a func" >$SCRATCH_DIR/returning_blemished32_ptr.txt &
get_count_of "Returning a blemished64 ptr from a func" >$SCRATCH_DIR/returning_blemished64_ptr.txt &
get_count_of "Returning a blemishedconst ptr from a func" >$SCRATCH_DIR/returning_blemishedconst_ptr.txt &
get_count_of "Returning a dirty ptr from a func" >$SCRATCH_DIR/returning_dirty_ptr.txt &
get_count_of "Returning an unknown ptr from a func" >$SCRATCH_DIR/returning_unknown_ptr.txt &
get_count_of "Nonzero constant pointer arithmetic on a clean ptr" > $SCRATCH_DIR/arith_clean_ptr.txt &
get_count_of "Nonzero constant pointer arithmetic on a blemished16 ptr" > $SCRATCH_DIR/arith_blemished16_ptr.txt &
get_count_of "Nonzero constant pointer arithmetic on a blemished32 ptr" > $SCRATCH_DIR/arith_blemished32_ptr.txt &
get_count_of "Nonzero constant pointer arithmetic on a blemished64 ptr" > $SCRATCH_DIR/arith_blemished64_ptr.txt &
get_count_of "Nonzero constant pointer arithmetic on a blemishedconst ptr" > $SCRATCH_DIR/arith_blemishedconst_ptr.txt &
get_count_of "Nonzero constant pointer arithmetic on a dirty ptr" > $SCRATCH_DIR/arith_dirty_ptr.txt &
get_count_of "Nonzero constant pointer arithmetic on an unknown ptr" > $SCRATCH_DIR/arith_unknown_ptr.txt &
get_count_of "Producing a ptr from inttoptr" >$SCRATCH_DIR/inttoptrs.txt &
wait

loads_clean_addr=$(cat $SCRATCH_DIR/loads_clean_addr.txt)
loads_blemished16_addr=$(cat $SCRATCH_DIR/loads_blemished16_addr.txt)
loads_blemished32_addr=$(cat $SCRATCH_DIR/loads_blemished32_addr.txt)
loads_blemished64_addr=$(cat $SCRATCH_DIR/loads_blemished64_addr.txt)
loads_blemishedconst_addr=$(cat $SCRATCH_DIR/loads_blemishedconst_addr.txt)
loads_dirty_addr=$(cat $SCRATCH_DIR/loads_dirty_addr.txt)
loads_unknown_addr=$(cat $SCRATCH_DIR/loads_unknown_addr.txt)
stores_clean_addr=$(cat $SCRATCH_DIR/stores_clean_addr.txt)
stores_blemished16_addr=$(cat $SCRATCH_DIR/stores_blemished16_addr.txt)
stores_blemished32_addr=$(cat $SCRATCH_DIR/stores_blemished32_addr.txt)
stores_blemished64_addr=$(cat $SCRATCH_DIR/stores_blemished64_addr.txt)
stores_blemishedconst_addr=$(cat $SCRATCH_DIR/stores_blemishedconst_addr.txt)
stores_dirty_addr=$(cat $SCRATCH_DIR/stores_dirty_addr.txt)
stores_unknown_addr=$(cat $SCRATCH_DIR/stores_unknown_addr.txt)
stores_clean_val=$(cat $SCRATCH_DIR/stores_clean_val.txt)
stores_blemished16_val=$(cat $SCRATCH_DIR/stores_blemished16_val.txt)
stores_blemished32_val=$(cat $SCRATCH_DIR/stores_blemished32_val.txt)
stores_blemished64_val=$(cat $SCRATCH_DIR/stores_blemished64_val.txt)
stores_blemishedconst_val=$(cat $SCRATCH_DIR/stores_blemishedconst_val.txt)
stores_dirty_val=$(cat $SCRATCH_DIR/stores_dirty_val.txt)
stores_unknown_val=$(cat $SCRATCH_DIR/stores_unknown_val.txt)
passing_clean_ptr=$(cat $SCRATCH_DIR/passing_clean_ptr.txt)
passing_blemished16_ptr=$(cat $SCRATCH_DIR/passing_blemished16_ptr.txt)
passing_blemished32_ptr=$(cat $SCRATCH_DIR/passing_blemished32_ptr.txt)
passing_blemished64_ptr=$(cat $SCRATCH_DIR/passing_blemished64_ptr.txt)
passing_blemishedconst_ptr=$(cat $SCRATCH_DIR/passing_blemishedconst_ptr.txt)
passing_dirty_ptr=$(cat $SCRATCH_DIR/passing_dirty_ptr.txt)
passing_unknown_ptr=$(cat $SCRATCH_DIR/passing_unknown_ptr.txt)
returning_clean_ptr=$(cat $SCRATCH_DIR/returning_clean_ptr.txt)
returning_blemished16_ptr=$(cat $SCRATCH_DIR/returning_blemished16_ptr.txt)
returning_blemished32_ptr=$(cat $SCRATCH_DIR/returning_blemished32_ptr.txt)
returning_blemished64_ptr=$(cat $SCRATCH_DIR/returning_blemished64_ptr.txt)
returning_blemishedconst_ptr=$(cat $SCRATCH_DIR/returning_blemishedconst_ptr.txt)
returning_dirty_ptr=$(cat $SCRATCH_DIR/returning_dirty_ptr.txt)
returning_unknown_ptr=$(cat $SCRATCH_DIR/returning_unknown_ptr.txt)
arith_clean_ptr=$(cat $SCRATCH_DIR/arith_clean_ptr.txt)
arith_blemished16_ptr=$(cat $SCRATCH_DIR/arith_blemished16_ptr.txt)
arith_blemished32_ptr=$(cat $SCRATCH_DIR/arith_blemished32_ptr.txt)
arith_blemished64_ptr=$(cat $SCRATCH_DIR/arith_blemished64_ptr.txt)
arith_blemishedconst_ptr=$(cat $SCRATCH_DIR/arith_blemishedconst_ptr.txt)
arith_dirty_ptr=$(cat $SCRATCH_DIR/arith_dirty_ptr.txt)
arith_unknown_ptr=$(cat $SCRATCH_DIR/arith_unknown_ptr.txt)
inttoptrs=$(cat $SCRATCH_DIR/inttoptrs.txt)
rm -rf $SCRATCH_DIR

RESULTS_FILE=dynamic_results.csv
echo " Category,clean,blemished16,blemished32,blemished64,blemishedconst,dirty,unknown" > $RESULTS_FILE
echo "Load addresses,$loads_clean_addr,$loads_blemished16_addr,$loads_blemished32_addr,$loads_blemished64_addr,$loads_blemishedconst_addr,$loads_dirty_addr,$loads_unknown_addr" >> $RESULTS_FILE
echo "Store addresses,$stores_clean_addr,$stores_blemished16_addr,$stores_blemished32_addr,$stores_blemished64_addr,$stores_blemishedconst_addr,$stores_dirty_addr,$stores_unknown_addr" >> $RESULTS_FILE
echo "Pointers stored to memory,$stores_clean_val,$stores_blemished16_val,$stores_blemished32_val,$stores_blemished64_val,$stores_blemishedconst_val,$stores_dirty_val,$stores_unknown_val" >> $RESULTS_FILE
echo "Pointers passed as func args,$passing_clean_ptr,$passing_blemished16_ptr,$passing_blemished32_ptr,$passing_blemished64_ptr,$passing_blemishedconst_ptr,$passing_dirty_ptr,$passing_unknown_ptr" >> $RESULTS_FILE
echo "Pointers returned from funcs,$returning_clean_ptr,$returning_blemished16_ptr,$returning_blemished32_ptr,$returning_blemished64_ptr,$returning_blemishedconst_ptr,$returning_dirty_ptr,$returning_unknown_ptr" >> $RESULTS_FILE
echo "Pointers we add nonzero constants to,$arith_clean_ptr,$arith_blemished16_ptr,$arith_blemished32_ptr,$arith_blemished64_ptr,$arith_blemishedconst_ptr,$arith_dirty_ptr,$arith_unknown_ptr" >> $RESULTS_FILE
echo "Pointers produced from inttoptr (assumed clean),$inttoptrs,0,0,0,0,0,0" >> $RESULTS_FILE

echo "Results saved to $RESULTS_FILE"
