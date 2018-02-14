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

#ifndef ARANGODB_MAINTENANCE_ACTION_BASE_H
#define ARANGODB_MAINTENANCE_ACTION_BASE_H

#include "ActionDescription.h"

#include "Basics/Common.h"
#include "Basics/Result.h"
//#include "lib/Logger/LogLevel.h"
//#include "lib/Logger/Logger.h"

#include <chrono>

namespace arangodb {
namespace maintenance {

enum ActionModel { BACKGROUND, FOREGROUND };

class ActionBase {
  
public:
  ActionBase(ActionDescription const&, ActionModel const& m = BACKGROUND);

  virtual ~ActionBase();

  virtual arangodb::Result run(
    std::chrono::duration<double> const&, bool& finished) = 0;
  
  virtual arangodb::Result kill(Signal const& signal) = 0;
  
  virtual arangodb::Result progress(double& progress) = 0;

  ActionDescription describe() const;

protected:

  std::string const& get(std::string const&) const;
  VPackSlice const& properties() const;

  ActionDescription _description;
  ActionModel _model;

  
  
}; // class ActionBase

} // namespace maintenance

Result actionError(int errorCode, std::string const& errorMessage);
Result actionWarn(int errorCode, std::string const& errorMessage);


} // namespace arangodb

#endif

