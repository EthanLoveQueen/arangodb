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

#include "CreateCollection.h"
#include "MaintenanceFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/FollowerInfo.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Databases.h"

#include <velocypack/Compare.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>


using namespace arangodb::application_features;
using namespace arangodb::maintenance;
using namespace arangodb::methods;

constexpr auto WAIT_FOR_SYNC_REPL = "waitForSyncReplication";
constexpr auto ENF_REPL_FACT = "enforceReplicationFactor";

CreateCollection::CreateCollection(
  std::shared_ptr<MaintenanceFeature> feature, ActionDescription const& d)
  : ActionBase(feature, d) {
  TRI_ASSERT(d.has(COLLECTION));
  TRI_ASSERT(d.has(DATABASE));
  TRI_ASSERT(d.has(ID));
  TRI_ASSERT(d.has(LEADER));
  TRI_ASSERT(d.properties().hasKey(TYPE));
  TRI_ASSERT(d.properties().get(TYPE).isInteger());  
}

CreateCollection::~CreateCollection() {};

arangodb::Result CreateCollection::run(
  std::chrono::duration<double> const&, bool& finished) {

  arangodb::Result res;

  auto const& database = _description.get(DATABASE);
  auto const& collection = _description.get(COLLECTION);
  auto const& planId = _description.get(ID);
  auto const& leader = _description.get(LEADER);
  auto const& properties = _description.properties();

  LOG_TOPIC(DEBUG, Logger::MAINTENANCE)
    << "creating local shard '" << database << "/" << collection
    << "' for central '" << database << "/" << planId << "'";
  
  auto vocbase = Databases::lookup(database);
  if (vocbase == nullptr) {
    std::string errorMsg("CreateCollection: Failed to lookup database ");
    errorMsg += database;
    return actionError(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND, errorMsg);
  }

  auto cluster =
    ApplicationServer::getFeature<ClusterFeature>("Cluster");
  
  bool waitForRepl =
    (properties.hasKey(WAIT_FOR_SYNC_REPL) &&
     properties.get(WAIT_FOR_SYNC_REPL).isBool()) ?
    properties.get(WAIT_FOR_SYNC_REPL).getBool() :
    cluster->createWaitsForSyncReplication();
  
  bool enforceReplFact = 
    (properties.hasKey(ENF_REPL_FACT) &&
     properties.get(ENF_REPL_FACT).isBool()) ?
    properties.get(ENF_REPL_FACT).getBool() : true;

  TRI_col_type_e type(properties.get(TYPE).getNumericValue<TRI_col_type_e>());
  
  VPackBuilder docket;
  for (auto const& i : VPackObjectIterator(properties)) {
    auto const& key = i.key.copyString();
    if (key == ID || key == NAME || key == GLOB_UID || key == OBJECT_ID) {
      if (key == GLOB_UID || key == OBJECT_ID) {
        LOG_TOPIC(WARN, Logger::MAINTENANCE)
          << "unexpected " << key << " in " << properties.toJson();
      } else if (key == ID) {
        docket.add("planId", i.value);
      }
      continue;
    }
    docket.add(key, i.value);
  }
  
  res = Collections::create(
    vocbase, collection, type, docket.slice(), waitForRepl, enforceReplFact,
    [=](LogicalCollection* col) {
      LOG_TOPIC(DEBUG, Logger::MAINTENANCE) << "local collection " << database
        << "/" << collection << " successfully created";
      col->followers()->setTheLeader(leader);
      if (leader.empty()) {
        col->followers()->clear();
      }
    });

  if (res.fail()) {
    LOG_TOPIC(ERR, Logger::MAINTENANCE)
      << "creating local shard '" << database << "/" << collection
      << "' for central '" << database << "/" << planId << "' failed: "
      << res;
  }
  
  return res;
}

arangodb::Result CreateCollection::kill(Signal const& signal) {
  return actionError(
    TRI_ERROR_ACTION_OPERATION_UNABORTABLE, "Cannot kill CreateCollection action");
}

arangodb::Result CreateCollection::progress(double& progress) {
  progress = 0.5;
  return arangodb::Result(TRI_ERROR_NO_ERROR);
}


