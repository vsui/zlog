#include "db_impl.h"

SharedNodeRef NodePtr::fetch(std::vector<std::pair<int64_t, int>>& trace)
{
  assert(curr_db);
  return curr_db->fetch(trace, csn_, offset_);
}
