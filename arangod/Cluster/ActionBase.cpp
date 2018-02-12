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

#include "ActionBase.h"

using namespace arangodb;
using namespace arangodb::maintenance;

ActionBase::ActionBase(
  ActionDescription const& d, ActionModel const& m) :
  _description(d), _model(m) {}

ActionBase::~ActionBase() {}

ActionDescription ActionBase::describe() const {
  return _description;
}

Result arangodb::actionError(int errorCode, std::string const& errorMessage) {
  LOG_TOPIC(ERR, Logger::MAINTENANCE) << errorMessage;
  return Result(errorCode, errorMessage);
}

Result arangodb::actionWarn(int errorCode, std::string const& errorMessage) {
  LOG_TOPIC(WARN, Logger::MAINTENANCE) << errorMessage;
  return Result(errorCode, errorMessage);
}
