// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <memory>
#include <queue>

#include "codegen/codegen-fn-ptr.h"
#include "codegen/impala-ir.h"
#include "exec/exec-node.h"
#include "runtime/descriptors.h"  // for TupleId
#include "util/tuple-row-compare.h"

namespace impala {

class MemPool;
class RuntimeState;
class TopNNode;
class Tuple;

class TopNPlanNode : public PlanNode {
 public:
  virtual Status Init(const TPlanNode& tnode, FragmentState* state) override;
  virtual void Close() override;
  virtual Status CreateExecNode(RuntimeState* state, ExecNode** node) const override;
  virtual void Codegen(FragmentState* state) override;

  int64_t offset() const {
    return tnode_->sort_node.__isset.offset ? tnode_->sort_node.offset : 0;
  }
  ~TopNPlanNode(){}

  /// Ordering expressions used for tuple comparison.
  std::vector<ScalarExpr*> ordering_exprs_;

  /// Cached descriptor for the materialized tuple.
  TupleDescriptor* output_tuple_desc_ = nullptr;

  /// Materialization exprs for the output tuple and their evaluators.
  std::vector<ScalarExpr*> output_tuple_exprs_;

  /// Config used to create a TupleRowComparator instance.
  TupleRowComparatorConfig* row_comparator_config_ = nullptr;

  /// Codegened version of TopNNode::InsertBatch().
  typedef void (*InsertBatchFn)(TopNNode*, RowBatch*);
  CodegenFnPtr<InsertBatchFn> codegend_insert_batch_fn_;
};

/// Node for in-memory TopN (ORDER BY ... LIMIT)
/// This handles the case where the result fits in memory.
/// This node will materialize its input rows into a new tuple using the expressions
/// in sort_tuple_slot_exprs_ in its sort_exec_exprs_ member.
/// TopN is implemented by storing rows in a priority queue.

class TopNNode : public ExecNode {
 public:
  TopNNode(ObjectPool* pool, const TopNPlanNode& pnode, const DescriptorTbl& descs);

  virtual Status Prepare(RuntimeState* state);
  virtual Status Open(RuntimeState* state);
  virtual Status GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos);
  virtual Status Reset(RuntimeState* state, RowBatch* row_batch);
  virtual void Close(RuntimeState* state);

 protected:
  virtual void DebugString(int indentation_level, std::stringstream* out) const;

 private:
  class Heap;

  friend class TupleLessThan;

  /// Inserts all the rows in 'batch' into the queue.
  void InsertBatch(RowBatch* batch);

  /// Prepare to output all of the rows in this operator in sorted order. Initializes
  /// 'sorted_top_n_' and 'get_next_iterator_'.
  void PrepareForOutput();

  // Re-materialize all tuples that reference 'tuple_pool_' and release 'tuple_pool_',
  // replacing it with a new pool.
  Status ReclaimTuplePool(RuntimeState* state);

  IR_NO_INLINE int tuple_byte_size() const {
    return output_tuple_desc_->byte_size();
  }

  /// Number of rows to skip.
  int64_t offset_;

  /// Materialization exprs for the output tuple and their evaluators.
  const std::vector<ScalarExpr*>& output_tuple_exprs_;
  std::vector<ScalarExprEvaluator*> output_tuple_expr_evals_;

  /// Cached descriptor for the materialized tuple.
  TupleDescriptor* const output_tuple_desc_;

  /// Comparator for priority_queue_.
  std::unique_ptr<TupleRowComparator> tuple_row_less_than_;

  /// After computing the TopN in the priority_queue, pop them and put them in this vector
  std::vector<Tuple*> sorted_top_n_;

  /// Tuple allocated once from tuple_pool_ and reused in InsertTupleRow to
  /// materialize input tuples if necessary. After materialization, tmp_tuple_ may be
  /// copied into the tuple pool and inserted into the priority queue.
  Tuple* tmp_tuple_ = nullptr;

  /// Stores everything referenced in priority_queue_.
  std::unique_ptr<MemPool> tuple_pool_;

  /// Iterator over elements in sorted_top_n_.
  std::vector<Tuple*>::iterator get_next_iter_;

  /// Reference to the codegened function pointer owned by the TopNPlanNode object that
  /// was used to create this instance.
  const CodegenFnPtr<TopNPlanNode::InsertBatchFn>& codegend_insert_batch_fn_;

  /// Timer for time spent in InsertBatch() function (or codegen'd version)
  RuntimeProfile::Counter* insert_batch_timer_;

  /// Number of rows to be reclaimed since tuple_pool_ was last created/reclaimed
  int64_t rows_to_reclaim_;

  /// Number of times tuple pool memory was reclaimed
  RuntimeProfile::Counter* tuple_pool_reclaim_counter_= nullptr;

  /////////////////////////////////////////
  /// BEGIN: Members that must be Reset()

  /// A heap containing up to 'limit_' + 'offset_' rows.
  std::unique_ptr<Heap> heap_;

  /// Number of rows skipped. Used for adhering to offset_.
  int64_t num_rows_skipped_;

  /// END: Members that must be Reset()
  /////////////////////////////////////////
};

/// This is the main data structure used for in-memory Top-N: a binary heap containing
/// up to 'capacity' tuples.
class TopNNode::Heap {
 public:
  Heap(int64_t capacity);

  void Reset();
  void Close();

  /// Inserts a tuple row into the priority queue if it's in the TopN.  Creates a deep
  /// copy of 'tuple_row', which it stores in 'tuple_pool'. Always inlined in IR into
  /// TopNNode::InsertBatch() because codegen relies on this for substituting exprs
  /// in the body of TopNNode.
  /// Returns true if a previous row was replaced.
  bool IR_ALWAYS_INLINE InsertTupleRow(TopNNode* node, TupleRow* input_row);

  /// Copy the elements in the priority queue into a new tuple pool, and release
  /// the previous pool.
  Status RematerializeTuples(TopNNode* node, RuntimeState* state, MemPool* new_pool);

  /// Put the tuples in the priority queue into 'sorted_top_n' in the correct order
  /// for output.
  void PrepareForOutput(
      const TopNNode& RESTRICT node, std::vector<Tuple*>* sorted_top_n) RESTRICT;

  /// Returns number of tuples currently in heap.
  int64_t num_tuples() const { return priority_queue_.size(); }

  IR_NO_INLINE int64_t heap_capacity() const { return capacity_; }

 private:
  /// Limit on capacity of 'priority_queue_'. If inserting a tuple into the queue
  /// would exceed this, a tuple is popped off the queue.
  const int64_t capacity_;

  /////////////////////////////////////////
  /// BEGIN: Members that must be Reset()

  /// The priority queue (represented by a vector and modified using
  /// push_heap()/pop_heap() to maintain ordered heap invariants) will never have more
  /// elements in it than the LIMIT + OFFSET. The order of the queue is the opposite of
  /// what the ORDER BY clause specifies, such that the top of the queue is the last
  /// sorted element.
  std::vector<Tuple*> priority_queue_;

  /// END: Members that must be Reset()
  /////////////////////////////////////////

  /// Helper methods for modifying priority_queue while maintaining ordered heap
  /// invariants
  inline static void PushHeap(std::vector<Tuple*>* priority_queue,
      const ComparatorWrapper<TupleRowComparator>& comparator, Tuple* const insert_row) {
    priority_queue->push_back(insert_row);
    std::push_heap(priority_queue->begin(), priority_queue->end(), comparator);
  }

  inline static void PopHeap(std::vector<Tuple*>* priority_queue,
      const ComparatorWrapper<TupleRowComparator>& comparator) {
    std::pop_heap(priority_queue->begin(), priority_queue->end(), comparator);
    priority_queue->pop_back();
  }
};

}; // namespace impala
