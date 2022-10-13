/**
##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2019 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################
**/

#include "string.h"
#include <queue>
#include <mutex>

class ClipQueue
{
 private:
  std::queue<std::string> queue_;
  std::mutex mutex_;

 public:
  std::string remove()
  {
    std::unique_lock<std::mutex> mlock(mutex_);
    auto clipname = queue_.front();
    queue_.pop();
    return clipname;
  }

  void add(const std::string& clipname)
  {
    std::unique_lock<std::mutex> mlock(mutex_);
    queue_.push(clipname);
  }

  int size(){
    return queue_.size();
  }

  void clear()
  {
    std::unique_lock<std::mutex> mlock(mutex_);
    while(queue_.size()  > 0) {
      queue_.pop();
    }
  }

};

