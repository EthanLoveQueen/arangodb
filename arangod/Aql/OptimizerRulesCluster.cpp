
////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Wilfried Goesgens
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "OptimizerRules.h"
#include "Aql/ClusterNodes.h"
#include "Aql/CollectNode.h"
#include "Aql/CollectOptions.h"
#include "Aql/Collection.h"
#include "Aql/ConditionFinder.h"
#include "Aql/DocumentProducingNode.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Function.h"
#include "Aql/IndexNode.h"
#include "Aql/ModificationNodes.h"
#include "Aql/Optimizer.h"
#include "Aql/Query.h"
#include "Aql/ShortestPathNode.h"
#include "Aql/SortCondition.h"
#include "Aql/SortNode.h"
#include "Aql/TraversalConditionFinder.h"
#include "Aql/TraversalNode.h"
#include "Aql/Variable.h"
#include "Aql/types.h"
#include "Basics/AttributeNameParser.h"
#include "Basics/SmallVector.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringBuffer.h"
#include "Cluster/ClusterInfo.h"
#include "Geo/GeoParams.h"
#include "GeoIndex/Index.h"
#include "Graph/TraverserOptions.h"
#include "Indexes/Index.h"
#include "Transaction/Methods.h"
#include "VocBase/Methods/Collections.h"

#include <boost/optional.hpp>
#include <tuple>

using namespace arangodb;
using namespace arangodb::aql;
using EN = arangodb::aql::ExecutionNode;

// in plan below
ExecutionNode* hasSingleParent(ExecutionNode* in){
  auto parents = in->getParents();
  if(parents.size() == 1){
    return parents.front();
  }
  return nullptr;
}

ExecutionNode* hasSingleParent(ExecutionNode* in, EN::NodeType type){
  auto* parent = hasSingleParent(in);
  if(parent) {
    if(parent->getType() == type){
      return parent;
    }
  }
  return nullptr;
}

ExecutionNode* hasSingleParent(ExecutionNode* in, std::vector<EN::NodeType> types){
  auto* parent = hasSingleParent(in);
  if(parent) {
    for(auto const& type : types){
      if(parent->getType() == type){
        return parent;
      }
    }
  }
  return nullptr;
}

// in plan above
ExecutionNode* hasSingleDep(ExecutionNode* in){
  auto deps = in->getDependencies();
  if(deps.size() == 1){
    return deps.front();
  }
  return nullptr;
}

ExecutionNode* hasSingleDep(ExecutionNode* in, EN::NodeType type){
  auto* dep = hasSingleDep(in);
  if(dep) {
    if(dep->getType() == type){
      return dep;
    }
  }
  return nullptr;
}

ExecutionNode* hasSingleDep(ExecutionNode* in, std::vector<EN::NodeType> types){
  auto* dep = hasSingleDep(in);
  if(dep) {
    for(auto const& type : types){
      if(dep->getType() == type){
        return dep;
      }
    }
  }
  return nullptr;
}

Index* hasSingleIndexHandle(ExecutionNode* node){
  TRI_ASSERT(node->getType() == EN::INDEX);
  IndexNode* indexNode = static_cast<IndexNode*>(node);
  auto indexHandleVec = indexNode->getIndexes();
  if (indexHandleVec.size() == 1){
    return indexHandleVec.front().getIndex().get();
  }
  return nullptr;
}

Index* hasSingleIndexHandle(ExecutionNode* node, Index::IndexType type){
  auto* idx = hasSingleIndexHandle(node);
  if (idx->type() == type ){
    return idx;
  }
  return nullptr;
}

std::vector<AstNode const*> hasBinaryCompare(ExecutionNode* node){
  // returns any AstNode in the expression that is
  // a binary comparison.
  TRI_ASSERT(node->getType() == EN::INDEX);
  IndexNode* indexNode = static_cast<IndexNode*>(node);
  AstNode const* cond = indexNode->condition()->root();
  std::vector<AstNode const*> result;

  auto preVisitor = [&result] (AstNode const* node) {
    if (node == nullptr) {
      return false;
    };

    if(node->type == NODE_TYPE_OPERATOR_BINARY_EQ){
      result.push_back(node);
      return false;
    }

    //skip over NARY AND/OR
    if(node->type == NODE_TYPE_OPERATOR_NARY_OR ||
       node->type == NODE_TYPE_OPERATOR_NARY_AND) {
      return true;
    } else {
      return false;
    }

  };
  auto postVisitor = [](AstNode const*){};
  Ast::traverseReadOnly(cond, preVisitor, postVisitor);

  return result;
}

std::string getFirstKey(std::vector<AstNode const*> compares){
  for(auto const* node : compares){
    AstNode const* keyNode = node->getMemberUnchecked(0);
    if(keyNode->type == AstNodeType::NODE_TYPE_ATTRIBUTE_ACCESS && keyNode->stringEquals("_key")) {
      keyNode = node->getMemberUnchecked(1);
    }
    if (keyNode->isStringValue()){
      return keyNode->getString();
    }
  }
  return "";
}

bool depIsSingletonOrConstCalc(ExecutionNode* node){
  while (node){
    node = node->getFirstDependency();
    if(node->getType() == EN::SINGLETON){
      //LOG_DEVEL << "reached singleton";
      return true;
    }

    if(node->getType() != EN::CALCULATION){
      //LOG_DEVEL << node->getTypeString() << " not a calculation node";
      return false;
    }

    if(!static_cast<CalculationNode*>(node)->arangodb::aql::ExecutionNode::getVariablesUsedHere().empty()){
      //LOG_DEVEL << "calculation not constant";
      return false;
    }
  }
  return false;
}

void replaceNode(ExecutionPlan* plan, ExecutionNode* oldNode, ExecutionNode* newNode){
  if(oldNode == plan->root()) {
    for(auto* dep : oldNode->getDependencies()) {
      newNode->addDependency(dep);
    }
    plan->root(newNode,true);
  } else {
    TRI_ASSERT(oldNode != plan->root());
    plan->replaceNode(oldNode, newNode);
  }
}

bool substituteClusterSingleDocumentOperationsIndex(Optimizer* opt,
                                                                   ExecutionPlan* plan,
                                                                   OptimizerRule const* rule) {
  bool modified = false;
  SmallVector<ExecutionNode*>::allocator_type::arena_type a;
  SmallVector<ExecutionNode*> nodes{a};
  plan->findNodesOfType(nodes, EN::INDEX, true);

  if(nodes.size() != 1){
    LOG_DEVEL << "plan has " << nodes.size() << "!=1 index nodes";
    return modified;
  }

  for(auto* node : nodes){
    LOG_DEVEL << "substitute single document operation INDEX";
    if(!depIsSingletonOrConstCalc(node)){
      LOG_DEVEL << "dependency is not singleton or const calculation";
      continue;
    }

    Index* index = hasSingleIndexHandle(node, Index::TRI_IDX_TYPE_PRIMARY_INDEX);
    if (index){
      IndexNode* indexNode = static_cast<IndexNode*>(node);
      auto binaryCompares = hasBinaryCompare(node);
      std::string key = getFirstKey(binaryCompares);
      if(key.empty()){
        LOG_DEVEL << "could not extract key from index condition";
        continue;
      }

      auto* parentModification = hasSingleParent(node,{EN::INSERT, EN::REMOVE, EN::UPDATE, EN::REPLACE});
      auto* parentSelect = hasSingleParent(node,EN::RETURN);

      if (parentModification){
        auto mod = static_cast<ModificationNode*>(parentModification);
        auto parentType = parentModification->getType();
        Variable const* update = nullptr;
        auto const& vec = mod->getVariablesUsedHere();

        LOG_DEVEL << "optimize modification node of type: "
                  << ExecutionNode::getTypeString(parentType)
                  << "  " << vec.size();

        if ( parentType == EN::REMOVE) {
          TRI_ASSERT(vec.size() == 1);
        } else if(parentType == EN::INSERT) {
          TRI_ASSERT(vec.size() == 1);
          update = vec.front();
        } else {
          TRI_ASSERT(vec.size() == 2);
          update = vec.front();
        }

        ExecutionNode* singleOperationNode = plan->registerNode(
          new SingleRemoteOperationNode(
            plan, plan->nextId(),
            parentType,
            key, mod->collection(),
            mod->getOptions(),
            update,
            nullptr,
            mod->getOutVariableOld(),
            mod->getOutVariableNew()
          )
        );

        replaceNode(plan, mod, singleOperationNode);
        plan->unlinkNode(indexNode);
        modified = true;
      } else if(parentSelect){
        LOG_DEVEL << "optimize SELECT with key: " << key;

        ExecutionNode* singleOperationNode = plan->registerNode(
            new SingleRemoteOperationNode(plan, plan->nextId()
                                         ,EN::INDEX, key, indexNode->collection(), ModificationOptions{}
                                         , nullptr /*in*/ , indexNode->outVariable() /*out*/, nullptr /*old*/, nullptr /*new*/)
        );
        replaceNode(plan, indexNode, singleOperationNode);
        modified = true;

      } else {
        LOG_DEVEL << "plan following the index node is too complex";
      }
    } else {
      LOG_DEVEL << "is not primary or has more indexes";
    }
  }
  return modified;
}

bool substituteClusterSingleDocumentOperationsKeyExpressions(Optimizer* opt,
                                                             ExecutionPlan* plan,
                                                             OptimizerRule const* rule) {
  bool modified = false;
  SmallVector<ExecutionNode*>::allocator_type::arena_type a;
  SmallVector<ExecutionNode*> nodes{a};
  plan->findNodesOfType(nodes, {EN::INSERT, EN::REMOVE, EN::UPDATE, EN::REPLACE}, true);

  if(nodes.size() != 1){
    LOG_DEVEL << "plan has " << nodes.size() << "!=1 modification nodes";
    return modified;
  }

  for(auto* node : nodes){
    LOG_DEVEL << "substitute single document operation NO INDEX";

    auto mod = static_cast<ModificationNode*>(node);

    if(!depIsSingletonOrConstCalc(node)){
      LOG_DEVEL << "optimization too complex (debIsSingleOrConstCalc)";
      continue;
    }

    auto p = node->getFirstParent();
    if( p && p->getType() != EN::RETURN){
      LOG_DEVEL << "parent of modification is not a RETURN node";
      continue;
    }

    auto depType = mod->getType();
    Variable const* update = nullptr;
    Variable const* keyVar = nullptr;
    std::string key = "";
    auto const& vec = mod->getVariablesUsedHere();

    LOG_DEVEL << "optimize modification node of type: "
              << ExecutionNode::getTypeString(depType)
              << "  " << vec.size();

    if ( depType == EN::REMOVE) {
      keyVar = vec.front();
      TRI_ASSERT(vec.size() == 1);
    } else {
      update = vec.front();
      if (vec.size() >= 1) {
        keyVar = vec.back();
      }
    }

    ExecutionNode* cursor = node;
    CalculationNode* calc = nullptr;

    if(keyVar){
      std::unordered_set<Variable const*> keySet;
      keySet.emplace(keyVar);

      while(cursor){
        cursor = hasSingleDep(cursor, EN::CALCULATION);
        if(cursor){
          CalculationNode* c = static_cast<CalculationNode*>(cursor);
          if(c->setsVariable(keySet)){
           LOG_DEVEL << "found calculation that sets key-expression";
           calc = c;
           break;
          }
        }
      }

      if(!calc){
        LOG_DEVEL << "calculation missing";
        continue;
      }
      AstNode const* expr = calc->expression()->node();
      if(expr->isStringValue()){
        key = expr->getString();
      } else if (false){
        // write more code here if we
        // want to support thinks like:
        //
        // DOCUMENT("foo/bar")
        expr->dump(0);
      }

      if(key.empty()){
        LOG_DEVEL << "could not extract key";
        continue;
      }
    }

    if(!depIsSingletonOrConstCalc(cursor)){
      LOG_DEVEL << "plan too complex";
      continue;
    }

    LOG_DEVEL << mod->collection()->name();

    ExecutionNode* singleOperationNode = plan->registerNode(
      new SingleRemoteOperationNode(
        plan, plan->nextId(),
        depType,
        key, mod->collection(),
        mod->getOptions(),
        update /*in*/,
        nullptr,
        mod->getOutVariableOld(),
        mod->getOutVariableNew()
      )
    );

    replaceNode(plan, mod, singleOperationNode);
    if(calc){
      plan->unlinkNode(calc);
    }
    modified = true;
  } // for node : nodes

  return modified;
}

void arangodb::aql::substituteClusterSingleDocumentOperations(Optimizer* opt,
                                                              std::unique_ptr<ExecutionPlan> plan,
                                                              OptimizerRule const* rule) {

  LOG_DEVEL << "enter singleOperationNode rule";
  bool modified = false;
  for(auto const& fun : { &substituteClusterSingleDocumentOperationsIndex
                        , &substituteClusterSingleDocumentOperationsKeyExpressions
                        }
  ){
    modified = fun(opt, plan.get(), rule);
    if(modified){ break; }
  }

  LOG_DEVEL_IF(modified) << "applied singleOperationNode rule !!!!!!!!!!!!!!!!!";

  opt->addPlan(std::move(plan), rule, modified);
  LOG_DEVEL << "exit singleOperationNode rule";
}
