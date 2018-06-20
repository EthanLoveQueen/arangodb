////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_EXECUTION_BLOCK_H
#define ARANGOD_AQL_EXECUTION_BLOCK_H 1

#include "Aql/BlockCollector.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionState.h"
#include "Aql/Variable.h"

#include <deque>

#if 0

#define DEBUG_BEGIN_BLOCK() try {  //
#define DEBUG_END_BLOCK()                                                     \
  }                                                                           \
  catch (arangodb::basics::Exception const& ex) {                             \
    LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "arango exception caught in " << __FILE__ << ":" << __LINE__ \
              << ":" << ex.what();                                            \
    throw;                                                                    \
  }                                                                           \
  catch (std::exception const& ex) {                                          \
    LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "std exception caught in " << __FILE__ << ":" << __LINE__    \
              << ": " << ex.what();                                           \
    throw;                                                                    \
  }                                                                           \
  catch (...) {                                                               \
    LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "exception caught in " << __FILE__ << ":" << __LINE__;       \
    throw;                                                                    \
  }  //

#else

#define DEBUG_BEGIN_BLOCK()  //
#define DEBUG_END_BLOCK()    //

#endif

namespace arangodb {
struct ClusterCommResult;

namespace transaction {
class Methods;
}

namespace aql {
class AqlItemBlock;
class ExecutionEngine;
struct QueryProfile;

class ExecutionBlock {
 public:
  ExecutionBlock(ExecutionEngine*, ExecutionNode const*);

  virtual ~ExecutionBlock();

  /// @brief type of the block. only the blocks actually instantiated are
  // needed, so e.g. ModificationBlock or ExecutionBlock are omitted.
  enum class Type {
    _UNDEFINED,
    CALCULATION,
    COUNT_COLLECT,
    DISTINCT_COLLECT,
    ENUMERATE_COLLECTION,
    ENUMERATE_LIST,
    FILTER,
    HASHED_COLLECT,
    INDEX,
    LIMIT,
    NO_RESULTS,
    REMOTE,
    RETURN,
    SHORTEST_PATH,
    SINGLETON,
    SORT,
    SORTED_COLLECT,
    SORTING_GATHER,
    SUBQUERY,
    TRAVERSAL,
    UNSORTING_GATHER,
    REMOVE,
    INSERT,
    UPDATE,
    REPLACE,
    UPSERT,
    SCATTER,
    DISTRIBUTE,
    IRESEARCH_VIEW,
    IRESEARCH_VIEW_ORDERED,
    IRESEARCH_VIEW_UNORDERED,
  };
  // omitted in this list are (because):
  // WaitingExecutionBlockMock (mock)
  // ExecutionBlockMock (mock)
  // ModificationBlock (insert, update, etc.)
  // BlockWithClients (scatter, distribute)
  // IResearchViewBlockBase (IResearchView*)

 public:
  /// @brief batch size value
  static constexpr inline size_t DefaultBatchSize() { return 1000; }

  /// @brief returns the register id for a variable id
  /// will return ExecutionNode::MaxRegisterId for an unknown variable
  RegisterId getRegister(VariableId id) const;
  RegisterId getRegister(Variable const* variable) const;

  /// @brief whether or not the query was killed
  bool isKilled() const;

  /// @brief throw an exception if query was killed
  void throwIfKilled();

  /// @brief add a dependency
  TEST_VIRTUAL void addDependency(ExecutionBlock* ep) { 
    TRI_ASSERT(ep != nullptr);
    _dependencies.emplace_back(ep); 
  }

  /// @brief get all dependencies
  std::vector<ExecutionBlock*> getDependencies() const { return _dependencies; }

  /// @brief remove a dependency, returns true if the pointer was found and
  /// removed, please note that this does not delete ep!
  bool removeDependency(ExecutionBlock* ep);
  
  /// @brief Methods for execution
  /// Lifecycle is:
  ///    CONSTRUCTOR
  ///    then the ExecutionEngine automatically calls
  ///      initialize() once, including subqueries
  ///    possibly repeat many times:
  ///      initializeCursor(...)   (optionally with bind parameters)
  ///      // use cursor functionality
  ///    then the ExecutionEngine automatically calls
  ///      shutdown()
  ///    DESTRUCTOR

  /// @brief initializeCursor, could be called multiple times
  virtual std::pair<ExecutionState, Result> initializeCursor(AqlItemBlock* items, size_t pos);

  /// @brief shutdown, will be called exactly once for the whole query
  virtual int shutdown(int);

  /// @brief getSome, gets some more items, semantic is as follows: not
  /// more than atMost items may be delivered. The method tries to
  /// return a block of at most atMost items, however, it may return
  /// less (for example if there are not enough items to come). However,
  /// if it returns an actual block, it must contain at least one item.
  /// getSome() also takes care of tracing and clearing registers; don't do it
  /// in getOrSkipSome() implementations.
  virtual std::pair<ExecutionState, std::unique_ptr<AqlItemBlock>> getSome(
      size_t atMost);

  void traceGetSomeBegin(size_t atMost);
  void traceGetSomeEnd(AqlItemBlock const*, ExecutionState state) const;
 
  /// @brief skipSome, skips some more items, semantic is as follows: not
  /// more than atMost items may be skipped. The method tries to
  /// skip a block of at most atMost items, however, it may skip
  /// less (for example if there are not enough items to come). The number of
  /// elements skipped is returned.
  virtual std::pair<ExecutionState, size_t> skipSome(size_t atMost);

  // Used in aql rest handler hasMore
  // Needs to be accurate
  // TODO change the documentation of all hasMore implementations
  virtual ExecutionState hasMoreState();
  
  ExecutionNode const* getPlanNode() const { return _exeNode; }
  
  transaction::Methods* transaction() const { return _trx; }

  // @brief Will be called on the querywakeup callback with the
  // result collected over the network. Needs to be implemented
  // on all nodes that use this mechanism.
  virtual bool handleAsyncResult(ClusterCommResult* result) { 
    // This indicates that a node uses async functionality
    // but does not react to the response.
    TRI_ASSERT(false);
    return true;
  }

  RegisterId getNrInputRegisters() const;

  RegisterId getNrOutputRegisters() const;

  virtual Type getType() const = 0;

 protected:
  /// @brief request an AqlItemBlock from the memory manager
  AqlItemBlock* requestBlock(size_t nrItems, RegisterId nrRegs);

  /// @brief return an AqlItemBlock to the memory manager
  void returnBlock(AqlItemBlock*& block);

  /// @brief copy register data from one block (src) into another (dst)
  /// register values are cloned
  void inheritRegisters(AqlItemBlock const* src, AqlItemBlock* dst, size_t row);

  void inheritRegisters(AqlItemBlock const* src, AqlItemBlock* dst, size_t,
                        size_t);

  /// @brief the following is internal to pull one more block and append it to
  /// our _buffer deque. Returns true if a new block was appended and false if
  /// the dependent node is exhausted.
  std::pair<ExecutionState, bool> getBlock(size_t atMost);

  /// @brief getSomeWithoutRegisterClearout, same as above, however, this
  /// is the actual worker which does not clear out registers at the end
  /// the idea is that somebody who wants to call the generic functionality
  /// in a derived class but wants to modify the results before the register
  /// cleanup can use this method, internal use only
  std::pair<ExecutionState, std::unique_ptr<AqlItemBlock>>
  getSomeWithoutRegisterClearout(size_t atMost);

  /// @brief clearRegisters, clears out registers holding values that are no
  /// longer needed by later nodes
  void clearRegisters(AqlItemBlock* result);
  

  /// @brief generic method to get or skip some
  /// Does neither do tracing (traceGetSomeBegin/~End), nor call
  /// clearRegisters() - both is done in getSome(), which calls this via
  /// getSomeWithoutRegisterClearout(). The same must hold for all overriding
  /// implementations.
  virtual std::pair<ExecutionState, Result> getOrSkipSome(size_t atMost, bool skipping,
                                                          AqlItemBlock*& result, size_t& skipped);

  /// @brief Returns the success return start of this block.
  ///        Can either be HASMORE or DONE.
  ///        Guarantee is that if DONE is returned every subsequent call
  ///        to get/skipSome will NOT find mor documents.
  ///        HASMORE is allowed to lie, so a next call to get/skipSome could return
  ///        no more results.
  virtual ExecutionState getHasMoreState();

 protected:
  /// @brief the execution engine
  ExecutionEngine* _engine;

  /// @brief the transaction for this query
  transaction::Methods* _trx;

  /// @brief our corresponding ExecutionNode node
  ExecutionNode const* _exeNode;

  /// @brief our dependent nodes
  std::vector<ExecutionBlock*> _dependencies;

  /// @brief this is our buffer for the items, it is a deque of AqlItemBlocks.
  /// We keep the following invariant between this and the other two variables
  /// _pos and _done: If _buffer.size() != 0, then 0 <= _pos <
  /// _buffer[0]->size()
  /// and _buffer[0][_pos] is the next item to be handed on. If _done is true,
  /// then no more documents will ever be returned. _done will be set to
  /// true if and only if we have no more data ourselves (i.e.
  /// _buffer.size()==0)
  /// and we have unsuccessfully tried to get another block from our dependency.
  std::deque<AqlItemBlock*> _buffer;

  /// @brief current working position in the first entry of _buffer
  size_t _pos;
  
  /// @brief if this is set, we are done, this is reset to false by execute()
  bool _done;

  /// @brief profiling level
  uint32_t _profile;
  
  /// @brief getSome begin point in time
  double _getSomeBegin;

  /// @brief the execution state of the dependency
  ///        used to determine HASMORE or DONE better
  ExecutionState _upstreamState;

 private:
  /// @brief The number of skipped/processed rows in getOrSkipSome, used to keep
  /// track of it despite WAITING interruptions. As
  /// ExecutionBlock::getOrSkipSome is called directly in some overriden
  /// implementations of ::getOrSkipSome, these implementations need their own
  /// _skipped counter.
  size_t _skipped;

  /// @brief Collects result blocks during ExecutionBlock::getOrSkipSome. Must
  /// be a member variable due to possible WAITING interruptions.
  aql::BlockCollector _collector;
};

}  // namespace arangodb::aql
}  // namespace arangodb

#endif
