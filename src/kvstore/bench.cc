#include <sstream>
#include <thread>
#include <vector>
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <map>
#include <cstdlib>
#include <time.h>
#include <sys/time.h>
#include "zlog/db.h"
#include "zlog/backend/lmdb.h"
#include "zlog/backend/fakeseqr.h"

#if __APPLE__
static inline uint64_t getns()
{
  struct timeval tv;
  assert(gettimeofday(&tv, NULL) == 0);
  uint64_t res = tv.tv_sec * 1000000000ULL;
  return res + tv.tv_usec * 1000ULL;
}
#else
static inline uint64_t __getns(clockid_t clock)
{
  struct timespec ts;
  int ret = clock_gettime(clock, &ts);
  assert(ret == 0);
  return (((uint64_t)ts.tv_sec) * 1000000000ULL) + ts.tv_nsec;
}
static inline uint64_t getns()
{
  return __getns(CLOCK_MONOTONIC);
}
#endif

static inline std::string tostr(int value)
{
  std::stringstream ss;
  ss << std::setw(9) << std::setfill('0') << value;
  return ss.str();
}

int main(int argc, char **argv)
{
  char *db_path;

  if (argc < 2) {
    fprintf(stderr, "must provide db path\n");
    return 1;
  }

  db_path = argv[1];
  int nthreads = atoi(argv[2]);

  // setup
  auto client = new FakeSeqrClient();
  auto be = new LMDBBackend("fakepool");
  be->Init(db_path, false);
  zlog::Log *log;
  int ret = zlog::Log::OpenOrCreate(be, "log", client, &log);
  assert(ret == 0);
  client->Init(log, "fakepool", "log");
  DB *db;
  ret = DB::Open(log, true, &db);
  assert(ret == 0);

  // fill db
  int counter = 0;
  std::srand(0);
  while (true) {
    auto txn = db->BeginTransaction();
    int nkey = counter++;
    const std::string key = tostr(nkey);
    txn->Put(key, key);
    txn->Commit();
    delete txn;

    if (counter == 50000)
      break;
    if (counter % 1000 == 0)
      std::cout << counter << std::endl;
  }

  uint64_t count = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < nthreads; i++) {
    threads.push_back(std::thread([&](){
      while (true) {
        auto nkey = rand() % 50000;
        const std::string key = tostr(nkey);
        std::string value;
        assert(db->Get(key, &value) == 0);
        count++;
      }
    }));
  }

  while (true) {
    auto start = count;
    sleep(2);
    auto total = count - start;
    std::cout << (double)total / 2.0f << std::endl << std::flush;
  }

  delete db;
  delete log;
  delete client;
  be->Close();
  delete be;
}
