#include <iostream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <cstring>

/*
 * Basic instrumentation for getting the VmRss (resident memory)
 * in Mac and Linux
 * Used to measure the memory at the transmission of every clip
 */
#if defined(__APPLE__)
  #include <mach/mach.h>
#elif defined(__linux__)
  #include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void init_stats();
void close_stats();
long compute_stats();

#ifdef __cplusplus
}
#endif
using namespace std;

ofstream gMemoryStatFile;

void init_stats() {
  gMemoryStatFile.open("memory_usage.txt");
}

void close_stats() {
  gMemoryStatFile.close();
}

long compute_stats() {
  auto timenow =
      chrono::system_clock::to_time_t(chrono::system_clock::now());

#if defined(__APPLE__)
  struct task_basic_info t_info;
  mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

  if (KERN_SUCCESS==task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t) &t_info,
                              &t_info_count)) {
    gMemoryStatFile << strtok(ctime(&timenow), "\n") << " Memory resident size: " << t_info.resident_size <<  "(bytes)" << endl;
  }

#elif defined(__linux__)
  ifstream stat_stream("/proc/self/stat",ios_base::in);
     long rss;
     string ignore_stat; //stats that we do not need
     for(int i = 0 ; i< 23 ; i++ ) {
      stat_stream >> ignore_stat;
     }
     stat_stream >> rss;   // resident nemory stats
     stat_stream.close();
     long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024;
     //myfile << strtok(ctime(&timenow), "\n") << " Memory resident size " << rss * page_size_kb << "(kB)" << endl;
     //std::cout << strtok(ctime(&timenow), "\n") << " Memory resident size " << rss * page_size_kb << "(kB)" << endl;
     return ( rss * page_size_kb );
#endif

}
