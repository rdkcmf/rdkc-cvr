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

