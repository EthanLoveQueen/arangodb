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
///     vpp://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "VppCommTask.h"

#include "Basics/HybridLogicalClock.h"
#include "GeneralServer/GeneralServer.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "GeneralServer/RestHandler.h"
#include "GeneralServer/RestHandlerFactory.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "VocBase/ticks.h"

#include <velocypack/Validator.h>
#include <velocypack/velocypack-aliases.h>

#include <stdexcept>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

namespace {
std::size_t findAndValidateVPacks(char const* vpHeaderStart,
                                  char const* chunkEnd) {
  VPackValidator validator;
  // check for slice start to the end of Chunk
  // isSubPart allows the slice to be shorter than the checked buffer.
  validator.validate(vpHeaderStart, std::distance(vpHeaderStart, chunkEnd),
                     /*isSubPart =*/true);

  // check if there is payload and locate the start
  VPackSlice vpHeader(vpHeaderStart);
  auto vpHeaderLen = vpHeader.byteSize();
  auto vpPayloadStart = vpHeaderStart + vpHeaderLen;
  if (vpPayloadStart == chunkEnd) {
    return 0;  // no payload available
  }

  // validate Payload VelocyPack
  validator.validate(vpPayloadStart, std::distance(vpPayloadStart, chunkEnd),
                     /*isSubPart =*/false);
  return std::distance(vpHeaderStart, vpPayloadStart);
}

std::unique_ptr<basics::StringBuffer> createChunkForNetworkDetail(
    char* start, char* end, bool isFirstChunk, uint32_t chunk, uint64_t id,
    uint32_t totalMessageLength = 0) {
  using basics::StringBuffer;
  bool firstOfMany = false;
  if (isFirstChunk && chunk == 1) {
    firstOfMany = true;
  }

  chunk <<= 1;
  chunk |= isFirstChunk ? 0x0 : 0x1;

  uint32_t dataLength = std::distance(start, end);
  uint32_t chunkLength = dataLength;
  chunkLength += (sizeof(chunkLength) + sizeof(chunk) + sizeof(id));
  if (firstOfMany) {
    chunkLength += sizeof(totalMessageLength);
  }

  auto buffer =
      std::make_unique<StringBuffer>(TRI_UNKNOWN_MEM_ZONE, chunkLength, false);
  buffer->appendInteger(chunkLength);
  buffer->appendInteger(chunk);  // chunkX //contains is first
  buffer->appendInteger(id);

  if (firstOfMany) {
    TRI_ASSERT(totalMessageLength != 0);
    buffer->appendInteger(totalMessageLength);
  }
  buffer->appendText(std::string(start, dataLength));

  return buffer;
}

std::unique_ptr<basics::StringBuffer> createChunkForNetworkSingle(char* start,
                                                                  char* end,
                                                                  uint64_t id) {
  return createChunkForNetworkDetail(start, end, true, 1, id, 0 /*unused*/);
}

std::unique_ptr<basics::StringBuffer> createChunkForNetworkMultiFirst(
    char* start, char* end, uint64_t id, uint32_t numberOfChunks,
    uint32_t totalMessageLength) {
  return createChunkForNetworkDetail(start, end, true, numberOfChunks, id,
                                     totalMessageLength);
}

std::unique_ptr<basics::StringBuffer> createChunkForNetworkMultiFollow(
    char* start, char* end, uint64_t id, uint32_t chunkNumber,
    uint32_t totalMessageLength) {
  return createChunkForNetworkDetail(start, end, false, chunkNumber, id, 0);
}
}

VppCommTask::VppCommTask(GeneralServer* server, TRI_socket_t sock,
                         ConnectionInfo&& info, double timeout)
    : Task("VppCommTask"),
      GeneralCommTask(server, sock, std::move(info), timeout) {
  _protocol = "vpp";
  // connectionStatisticsAgentSetVpp();
}

void VppCommTask::addResponse(VppResponse* response, bool isError) {
  if (isError) {
    // FIXME (obi)
    // what do we need to do?
    // clean read buffer? reset process read cursor
  }

  VPackMessageNoOwnBuffer response_message = response->prepareForNetwork();
  uint64_t& id = response_message._id;

  std::vector<VPackSlice> slices;
  slices.push_back(response_message._header);

  // if payload != Slice()
  slices.push_back(response_message._payload);

  uint32_t message_length = 0;

  for (auto const& slice : slices) {
    message_length += slice.byteSize();
  }

  // FIXME (obi)
  // If the message is big we will create many small chunks in a loop.
  // For the first tests we just send single Messages
  StringBuffer tmp(TRI_UNKNOWN_MEM_ZONE, message_length, false);

  for (auto const& slice : slices) {
    tmp.appendText(slice.startAs<char>(), slice.byteSize());
  }

  // adds chunk header infromation and creates SingBuffer* that can be
  // used with _writeBuffers
  auto buffer = createChunkForNetworkSingle(tmp.begin(), tmp.end(), id);
  _writeBuffers.push_back(buffer.get());
  buffer.release();

  fillWriteBuffer();  // move data from _writebuffers to _writebuffer
                      // implemented in base
}

VppCommTask::ChunkHeader VppCommTask::readChunkHeader() {
  VppCommTask::ChunkHeader header;

  auto cursor = _readBuffer->begin();

  std::memcpy(&header._chunkLength, cursor, sizeof(header._chunkLength));
  cursor += sizeof(header._chunkLength);

  uint32_t chunkX;
  std::memcpy(&chunkX, cursor, sizeof(chunkX));
  cursor += sizeof(chunkX);

  header._isFirst = chunkX & 0x1;
  header._chunk = chunkX >> 1;

  std::memcpy(&header._messageID, cursor, sizeof(header._messageID));
  cursor += sizeof(header._messageID);

  // extract total len of message
  if (header._isFirst && header._chunk > 1) {
    std::memcpy(&header._messageLength, cursor, sizeof(header._messageLength));
    cursor += sizeof(header._messageLength);
  } else {
    header._messageLength = 0;  // not needed
  }

  header._headerLength = std::distance(_readBuffer->begin(), cursor);

  return header;
}

bool VppCommTask::isChunkComplete(char* start) {
  std::size_t length = std::distance(start, _readBuffer->end());
  auto& prv = _processReadVariables;

  if (!prv._currentChunkLength && length < sizeof(uint32_t)) {
    return false;
  }
  if (!prv._currentChunkLength) {
    // read chunk length
    std::memcpy(&prv._currentChunkLength, start, sizeof(uint32_t));
  }
  if (length < prv._currentChunkLength) {
    // chunk not complete
    return false;
  }

  return true;
}

// reads data from the socket
bool VppCommTask::processRead() {
  // TODO FIXME
  // - in case of error send an operation failed to all incomplete messages /
  //   operation and close connection (implement resetState/resetCommtask)

  auto& prv = _processReadVariables;
  if (!prv._readBufferCursor) {
    prv._readBufferCursor = _readBuffer->begin();
  }

  auto chunkBegin = prv._readBufferCursor;
  if (chunkBegin == nullptr || !isChunkComplete(chunkBegin)) {
    return true;  // no data or incomplete
  }

  ChunkHeader chunkHeader = readChunkHeader();
  auto chunkEnd = chunkBegin + chunkHeader._chunkLength;
  auto vpackBegin = chunkBegin + chunkHeader._headerLength;
  bool do_execute = false;
  VPackMessage message;  // filled in CASE 1 or CASE 2b

  // CASE 1: message is in one chunk
  if (chunkHeader._isFirst && chunkHeader._chunk == 1) {
    std::size_t payloadOffset = findAndValidateVPacks(vpackBegin, chunkEnd);
    message._id = chunkHeader._messageID;
    message._buffer.append(vpackBegin, std::distance(vpackBegin, chunkEnd));
    message._header = VPackSlice(message._buffer.data());
    if (payloadOffset) {
      message._payload = VPackSlice(message._buffer.data() + payloadOffset);
    }
    VPackValidator val;
    val.validate(message._header.begin(), message._header.byteSize());

    do_execute = true;
  }
  // CASE 2:  message is in multiple chunks
  auto incompleteMessageItr = _incompleteMessages.find(chunkHeader._messageID);

  // CASE 2a: chunk starts new message
  if (chunkHeader._isFirst) {  // first chunk of multi chunk message
    if (incompleteMessageItr != _incompleteMessages.end()) {
      throw std::logic_error(
          "Message should be first but is already in the Map of incomplete "
          "messages");
    }

    IncompleteVPackMessage message(chunkHeader._messageLength,
                                   chunkHeader._chunk /*number of chunks*/);
    message._buffer.append(vpackBegin, std::distance(vpackBegin, chunkEnd));
    auto insertPair = _incompleteMessages.emplace(
        std::make_pair(chunkHeader._messageID, std::move(message)));
    if (!insertPair.second) {
      throw std::logic_error("insert failed");
    }

    // CASE 2b: chunk continues a message
  } else {  // followup chunk of some mesage
    if (incompleteMessageItr == _incompleteMessages.end()) {
      throw std::logic_error("found message without previous part");
    }
    auto& im = incompleteMessageItr->second;  // incomplete Message
    im._currentChunk++;
    assert(im._currentChunk == chunkHeader._chunk);
    im._buffer.append(vpackBegin, std::distance(vpackBegin, chunkEnd));
    // check buffer longer than length

    // MESSAGE COMPLETE
    if (im._currentChunk == im._numberOfChunks) {
      std::size_t payloadOffset = findAndValidateVPacks(
          reinterpret_cast<char const*>(im._buffer.data()),
          reinterpret_cast<char const*>(im._buffer.data() +
                                        im._buffer.byteSize()));
      message._id = chunkHeader._messageID;
      message._buffer = std::move(im._buffer);
      message._header = VPackSlice(message._buffer.data());
      if (payloadOffset) {
        message._payload = VPackSlice(message._buffer.data() + payloadOffset);
      }
      _incompleteMessages.erase(incompleteMessageItr);
      // check length

      do_execute = true;
    }
  }

  // clean buffer up to length of chunk
  prv._readBufferCursor = chunkEnd;
  std::size_t processedDataLen =
      std::distance(_readBuffer->begin(), prv._readBufferCursor);
  if (processedDataLen > prv._cleanupLength) {
    _readBuffer->move_front(prv._cleanupLength);
    prv._readBufferCursor = nullptr;  // the positon will be set at the
                                      // begin of this function
  }

  if (!do_execute) {
    return true;  // we have no complete request, so we return early
  }

  // for now we can handle only one request at a time
  // lock _request???? REVIEW (fc)
  LOG(ERR) << message._header.toJson();
  _request = new VppRequest(_connectionInfo, std::move(message));
  GeneralServerFeature::HANDLER_FACTORY->setRequestContext(_request);
  _request->setClientTaskId(_taskId);
  _protocolVersion = _request->protocolVersion();
  executeRequest(_request,
                 new VppResponse(GeneralResponse::ResponseCode::SERVER_ERROR,
                                 chunkHeader._messageID));
  return true;
}

void VppCommTask::completedWriteBuffer() {
  //  _writeBuffer = nullptr;
  //  _writeLength = 0;
  //
  //  if (_writeBufferStatistics != nullptr) {
  //    _writeBufferStatistics->_writeEnd = TRI_StatisticsTime();
  //    TRI_ReleaseRequestStatistics(_writeBufferStatistics);
  //    _writeBufferStatistics = nullptr;
  //  }
  //
  //  fillWriteBuffer();
  //
  //  if (!_clientClosed && _closeRequested && !hasWriteBuffer() &&
  //      _writeBuffers.empty() && !_isChunked) {
  //    _clientClosed = true;
  //  }
}

void VppCommTask::resetState(bool close) {
  // REVIEW (fc)
  // is there a case where we do not want to close the connection
  replyToIncompleteMessages();

  /*
  if (close) {
    clearRequest();

    _requestPending = false;
    _isChunked = false;
    _closeRequested = true;

    _readPosition = 0;
    _bodyPosition = 0;
    _bodyLength = 0;
  } else {
    _requestPending = true;

    bool compact = false;

    if (_sinceCompactification > RunCompactEvery) {
      compact = true;
    } else if (_readBuffer->length() > MaximalPipelineSize) {
      compact = true;
    }

    if (compact) {
      _readBuffer->erase_front(_bodyPosition + _bodyLength);

      _sinceCompactification = 0;
      _readPosition = 0;
    } else {
      _readPosition = _bodyPosition + _bodyLength;

      if (_readPosition == _readBuffer->length()) {
        _sinceCompactification = 0;
        _readPosition = 0;
        _readBuffer->reset();
      }
    }

    _bodyPosition = 0;
    _bodyLength = 0;
  }

  _newRequest = true;
  _readRequestBody = false;
  */
}

// GeneralResponse::ResponseCode VppCommTask::authenticateRequest() {
//   auto context = (_request == nullptr) ? nullptr :
//   _request->requestContext();
//
//   if (context == nullptr && _request != nullptr) {
//     bool res =
//         GeneralServerFeature::HANDLER_FACTORY->setRequestContext(_request);
//
//     if (!res) {
//       return GeneralResponse::ResponseCode::NOT_FOUND;
//     }
//
//     context = _request->requestContext();
//   }
//
//   if (context == nullptr) {
//     return GeneralResponse::ResponseCode::SERVER_ERROR;
//   }
//
//   return context->authenticate();
// }

// convert internal GeneralRequest to VppRequest
VppRequest* VppCommTask::requestAsVpp() {
  VppRequest* request = dynamic_cast<VppRequest*>(_request);
  if (request == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
  }
  return request;
};
