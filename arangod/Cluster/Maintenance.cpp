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
/// @author Kaveh Vahedipour
/// @author Matthew Von-Maszewski
////////////////////////////////////////////////////////////////////////////////

#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Logger/Logger.h"
#include "Cluster/ActionRegistry.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/Maintenance.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Databases.h"

#include <velocypack/Compare.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

#include <algorithm>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::maintenance;
using namespace arangodb::methods;

static std::vector<std::string> cmp {
  "journalSize", "waitForSync", "doCompact", "indexBuckets"};
static std::string const currentCollections("/arango/Current/Collections");
static std::string const planCollections("/arango/Plan/Collections");
static std::string const PRIMARY("primary");


std::shared_ptr<VPackBuilder> createProps(VPackSlice const& s) {
  auto builder = std::make_shared<VPackBuilder>();
  TRI_ASSERT(s.isObject());
  { VPackObjectBuilder b(builder.get());
    for (auto const& attr : VPackObjectIterator(s)) {
      std::string const key = attr.key.copyString();
      if (key == ID || key == NAME) {
        continue;
      }
      builder->add(key, attr.value);
    }}
  return builder;
}


std::shared_ptr<VPackBuilder> compareRelevantProps (
  VPackSlice const& first, VPackSlice const& second) {
  auto result = std::make_shared<VPackBuilder>();
  { VPackObjectBuilder b(result.get());
    for (auto property : cmp) {
      auto const& planned = first.get(property);
      if (planned != second.get(property)) { // Register any change
        result->add(property,planned);
      }
    }
  }
  return result;
}


VPackBuilder compareIndexes(
  std::string const& shname, VPackSlice const& plan, VPackSlice const& local,
  std::unordered_set<std::string>& indis) {
  
  VPackBuilder builder;
  { VPackArrayBuilder a(&builder);
    for (auto const& pindex : VPackArrayIterator(plan)) {

      // Skip unique on _key
      auto const& ptype   = pindex.get(TYPE).copyString();
      if (ptype == PRIMARY || ptype == EDGE) { 
        continue;
      }
      auto const& pfields = pindex.get(FIELDS);
      indis.emplace(shname + "/" + pindex.get(ID).copyString());
      
      bool found = false;
      for (auto const& lindex : VPackArrayIterator(local)) {

        // Skip unique and edge indexes
        auto const& ltype   = lindex.get(TYPE).copyString();
        if (ltype == PRIMARY || ltype == EDGE) { 
          continue;
        }
        auto const& lfields = lindex.get(FIELDS);

        // Already have
        if (VPackNormalizedCompare::equals(pfields, lfields) && ptype == ltype) { 
          found = true;
          break;
        }
      }
      if (!found) {
        builder.add(pindex);
      }
    }
  }
  
  return builder;
}


void handlePlanShard(
  VPackSlice const& db, VPackSlice const& cprops, VPackSlice const& ldb,
  std::string const& dbname, std::string const& shname, std::string const& serverId,
  std::string const& leaderId, std::unordered_set<std::string>& colis,
  std::unordered_set<std::string>& indis,
  std::vector<ActionDescription>& actions) {

  bool shouldBeLeader = serverId == leaderId;
  
  // We only care for shards, where we find our own ID
  if (db.copyString() == serverId)  {
    colis.emplace(shname);
    auto props = createProps(cprops); // Only once might need often!

    if (ldb.hasKey(shname)) {   // Have local collection with that name
      auto const lcol = ldb.get(shname);
      auto const properties = compareRelevantProps(cprops, lcol);

      // If comparison has brought any updates
      if (properties->slice() != VPackSlice::emptyObjectSlice()) {
        actions.push_back(
          ActionDescription(
            {{NAME, "UpdateCollection"}, {DATABASE, dbname}, {COLLECTION, shname},
              {LEADER, shouldBeLeader ? std::string() : leaderId},
              {LOCAL_LEADER, lcol.get(LEADER).copyString()}},
            properties));
      }
      
      // Indexes
      if (cprops.hasKey(INDEXES)) {
        auto const& pindexes = cprops.get(INDEXES);
        auto const& lindexes = lcol.get(INDEXES);
        auto difference = compareIndexes(shname, pindexes, lindexes, indis);

        if (difference.slice().length() != 0) {
          for (auto const& index : VPackArrayIterator(difference.slice())) {
            actions.push_back(
              ActionDescription({{NAME, "EnsureIndex"}, {COLLECTION, shname},
                  {DATABASE, dbname}, {TYPE, index.get(TYPE).copyString()},
                  {FIELDS, index.get(FIELDS).toJson()}
                }, std::make_shared<VPackBuilder>(index))
              );
          }
        }
     }
    } else {                   // Create the sucker!
      actions.push_back(
        ActionDescription({
            {NAME, "CreateCollection"}, {COLLECTION, shname}, {DATABASE, dbname},
            {LEADER, shouldBeLeader ? std::string() : leaderId}}, props));
    }
  }
}            


void handleLocalShard(
  std::string const& dbname, std::string const& colname, VPackSlice const& cprops, 
  std::unordered_set<std::string>& colis, std::unordered_set<std::string>& indis,
  std::vector<ActionDescription>& actions) {
  
  bool drop = false;
  std::unordered_set<std::string>::const_iterator it;

  if (colis.empty()) {
    drop = true;
  } else {
    it = std::find(colis.begin(), colis.end(), colname);
    if (it == colis.end()) {
      drop = true;
    }
  }

  if (drop) {
    actions.push_back(
      ActionDescription({{NAME, "DropCollection"},
          {DATABASE, dbname}, {COLLECTION, colname}}));
  } else {
    colis.erase(it);

    // We only drop indexes, when collection is not being dropped already
    if (cprops.hasKey(INDEXES)) {
      for (auto const& index :
             VPackArrayIterator(cprops.get(INDEXES))) {
        auto const& type = index.get(TYPE).copyString();
        if (type != PRIMARY && type != EDGE) {
          std::string const id = index.get(ID).copyString();
          if (indis.find(id) != indis.end()) {
            indis.erase(id);
          } else {
            actions.push_back(
              ActionDescription({{NAME, "DropIndex"}, {DATABASE, dbname},
                                 {COLLECTION, colname}, {ID, id}}));
          }
        }
      }
    }

  }
}

/// @brief calculate difference between plan and local for for databases
arangodb::Result arangodb::maintenance::diffPlanLocal (
  VPackSlice const& plan, VPackSlice const& local, std::string const& serverId,
  std::vector<ActionDescription>& actions) {

  std::unordered_set<std::string> colis; // Intersection collections plan&local
  std::unordered_set<std::string> indis; // Intersection indexes plan&local
  
  arangodb::Result result;
  auto const& pdbs =
    plan.get(std::vector<std::string>{"arango", "Plan", "Collections"});

  // Plan to local mismatch ----------------------------------------------------
  // Create or modify if local collections are affected
  for (auto const& pdb : VPackObjectIterator(pdbs)) {
    auto const& dbname = pdb.key.copyString();
    if (local.hasKey(dbname)) {    // have database in both see to collections
      auto const& ldb = local.get(dbname);
      for (auto const& pcol : VPackObjectIterator(pdb.value)) {
        auto const& cprops = pcol.value;
        for (auto const& shard : VPackObjectIterator(cprops.get(SHARDS))) {
          for (auto const& db : VPackArrayIterator(shard.value)) {
            handlePlanShard(
              db, cprops, ldb, dbname, shard.key.copyString(), serverId,
              shard.value[0].copyString(), colis, indis, actions);
          }
        }
      }
    } else { // Create new database
      actions.push_back(
        ActionDescription({{NAME, "CreateDatabase"}, {DATABASE, dbname}}));
    }
  }
  

  // Compare local to plan -----------------------------------------------------
  for (auto const& db : VPackObjectIterator(local)) {
    auto const& dbname = db.key.copyString();
    if (pdbs.hasKey(dbname)) {
      for (auto const& col : VPackObjectIterator(db.value)) {
        handleLocalShard(
          dbname, col.key.copyString(), col.value, colis, indis, actions);
      }
    } else {
      actions.push_back(
        ActionDescription({{NAME, "DropDatabase"}, {DATABASE, dbname}}));
    }
  }
  
  return result;
  
}

/// @brief handle plan for local databases
arangodb::Result arangodb::maintenance::executePlan (
  VPackSlice const& plan, VPackSlice const& current, VPackSlice const& local,
  std::string const& serverId) {

  arangodb::Result result;
  //ActionRegistry* registry = ActionRegistry::instance();

  // build difference between plan and local
  std::vector<ActionDescription> actions;
  diffPlanLocal(plan, local, serverId, actions);

  // enact all
  for (auto const& action : actions) {
    //registry->dispatch(action);
  }

  return result;  
}

/// @brief add new database to current
void addDatabaseToTransactions(
  std::string const& name, Transactions& transactions) {

  // [ {"dbPath":{}}, {"dbPath":{"oldEmpty":true}} ]
  
  std::string dbPath = currentCollections + "/" + name;
  VPackBuilder operation; // create database in current
  { VPackObjectBuilder b(&operation);
    operation.add(dbPath, VPackSlice::emptyObjectSlice()); }
  VPackBuilder precondition;
  { VPackObjectBuilder b(&precondition);
    precondition.add(VPackValue(dbPath));
    { VPackObjectBuilder bb(&precondition);
      precondition.add("oldEmpty", VPackValue(true)); }}
  transactions.push_back({operation, precondition});
  
}
  
/// @brief report local to current
arangodb::Result arangodb::maintenance::diffLocalCurrent (
  VPackSlice const& local, VPackSlice const& current,
  std::string const& serverId, Transactions& transactions) {

  arangodb::Result result;
  auto const& cdbs = current.get(
    std::vector<std::string>({"arango", "Current", "Collections"}));

  // Iterate over local databases
  for (auto const& ldbo : VPackObjectIterator(local)) {
    std::string dbname = ldbo.key.copyString();
    //VPackSlice ldb = ldbo.value;

    // Current has this database
    if (cdbs.hasKey(dbname)) {

    } else {
      // Create new database in current
      addDatabaseToTransactions(dbname, transactions);
    }
  }
  
  return result;
}


/// @brief Phase one: Compare plan and local and create descriptions
arangodb::Result arangodb::maintenance::phaseOne (
  VPackSlice const& plan, VPackSlice const& cur, VPackSlice const& local,
  std::string const& serverId) {

  arangodb::Result result;

  // Execute database changes
  result = executePlan(plan, cur, local, serverId);
  if (result.fail()) {
    return result;
  }
  return result;
}


/// @brief Phase two: See, what we can report to the agency
arangodb::Result arangodb::maintenance::phaseTwo (
  VPackSlice const& plan, VPackSlice const& cur, VPackSlice const& local) {
  arangodb::Result result;

  // Synchronise shards
  result = synchroniseShards(plan, cur, local);

  return result;
}


arangodb::Result arangodb::maintenance::synchroniseShards (
  VPackSlice const&, VPackSlice const&, VPackSlice const&) {
  arangodb::Result result;
  return result;
}

