#include <vector>
#include <atomic>
#include "zlog/backend.h"
#include "zlog/backend/ram.h"

namespace zlog {
namespace storage {
namespace ram {

RAMBackend::~RAMBackend()
{
}

int RAMBackend::Initialize(
    const std::map<std::string, std::string>& opts)
{
  return 0;
}

std::map<std::string, std::string> RAMBackend::meta()
{
  return options_;
}

int RAMBackend::uniqueId(const std::string& hoid, uint64_t *id)
{
  if (hoid.empty()) {
    return -EINVAL;
  }

  static std::atomic<uint64_t> __unique_id(0);
  *id = __unique_id++;

  return 0;
}

int RAMBackend::CreateLog(const std::string& name, const std::string& view,
    std::string *hoid_out, std::string *prefix_out)
{
  if (name.empty()) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  ProjectionObject proj_obj;
  proj_obj.epoch = 1;
  proj_obj.projections.emplace(proj_obj.epoch, view);
  auto ret = objects_.emplace(name, proj_obj);
  if (!ret.second) {
    return -EEXIST;
  }

  if (hoid_out) {
    *hoid_out = name;
  }

  if (prefix_out) {
    *prefix_out = name;
  }

  return 0;
}

int RAMBackend::OpenLog(const std::string& name, std::string *hoid_out,
    std::string *prefix_out)
{
  if (name.empty()) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  auto it = objects_.find(name);
  if (it == objects_.end()) {
    return -ENOENT;
  }

  if (hoid_out) {
    *hoid_out = name;
  }

  if (prefix_out) {
    *prefix_out = name;
  }

  return 0;
}

int RAMBackend::ReadViews(const std::string& hoid, uint64_t epoch,
    uint32_t max_views, std::map<uint64_t, std::string> *views_out)
{
  if (hoid.empty())
    return -EINVAL;

  if (epoch == 0) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  auto it = objects_.find(hoid);
  if (it == objects_.end()) {
    return -ENOENT;
  }

  std::map<uint64_t, std::string> views;
  auto& proj_obj = boost::get<ProjectionObject>(it->second);
  if (epoch > proj_obj.epoch) {
    views_out->swap(views);
    return 0;
  }

  // TODO shouldn't this just return empty views?
  auto it2 = proj_obj.projections.find(epoch);
  if (it2 == proj_obj.projections.end()) {
    return -EIO;
  }

  uint32_t count = 0;
  while (true) {
    if (count == max_views) {
      break;
    }

    if (it2 == proj_obj.projections.end()) {
      break;
    }

    assert(it2->first == epoch);
    views.emplace(epoch, it2->second);

    it2++;
    epoch++;
    count++;
  }

  views_out->swap(views);

  return 0;
}

int RAMBackend::ProposeView(const std::string& hoid,
    uint64_t epoch, const std::string& view)
{
  if (hoid.empty()) {
    return -EINVAL;
  }

  if (epoch == 0) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  auto it = objects_.find(hoid);
  if (it == objects_.end()) {
    return -ENOENT;
  }

  ProjectionObject& proj_obj = boost::get<ProjectionObject>(it->second);
  const auto required_epoch = proj_obj.epoch + 1;
  if (epoch != required_epoch) {
    return -ESPIPE;
  }

  auto ret = proj_obj.projections.emplace(epoch, view);
  if (!ret.second) {
    return -EEXIST;
  }

  proj_obj.epoch = epoch;

  return 0;
}

int RAMBackend::Read(const std::string& oid, uint64_t epoch,
    uint64_t position, std::string *data)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  if (epoch == 0) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  LogObject *lobj = nullptr;
  int ret = CheckEpoch(epoch, oid, false, lobj);
  if (ret) {
    return ret;
  }

  if (lobj) {
    const auto it = lobj->entries.find(position);
    if (it == lobj->entries.end())
      return -ERANGE;

    const LogEntry& entry = it->second;
    if (entry.trimmed || entry.invalidated)
      return -ENODATA;

    data->assign(entry.data);
    return 0;
  } else {
    return -ENOENT;
  }
}

int RAMBackend::Write(const std::string& oid, const Slice& data,
    uint64_t epoch, uint64_t position)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  if (epoch == 0) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  LogObject *lobj = nullptr;
  int ret = CheckEpoch(epoch, oid, false, lobj);
  if (ret) {
    return ret;
  }

  assert(lobj);
  {
    auto ret = objects_.emplace(oid, LogObject());
    lobj = &boost::get<LogObject>(ret.first->second);
  }

  auto it = lobj->entries.find(position);
  if (it == lobj->entries.end()) {
    LogEntry entry;
    entry.data = data.ToString();
    lobj->entries.emplace(position, entry);
    lobj->maxpos = std::max(lobj->maxpos, position);
    return 0;
  } else {
    return -EROFS;
  }
}

int RAMBackend::Trim(const std::string& oid, uint64_t epoch,
    uint64_t position)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  if (epoch == 0) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  LogObject *lobj = nullptr;
  int ret = CheckEpoch(epoch, oid, false, lobj);
  if (ret) {
    return ret;
  }

  if (!lobj) {
    auto ret = objects_.emplace(oid, LogObject());
    lobj = &boost::get<LogObject>(ret.first->second);
  }

  auto it = lobj->entries.find(position);
  if (it == lobj->entries.end()) {
    LogEntry entry;
    entry.trimmed = true;
    entry.invalidated = true;
    lobj->entries.emplace(position, entry);
    lobj->maxpos = std::max(lobj->maxpos, position);
  } else {
    auto& entry = it->second;
    entry.trimmed = true;
    entry.data.clear();
    lobj->maxpos = std::max(lobj->maxpos, position);
  }

  return 0;
}

int RAMBackend::Fill(const std::string& oid, uint64_t epoch,
    uint64_t position)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  if (epoch == 0) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  LogObject *lobj = nullptr;
  int ret = CheckEpoch(epoch, oid, false, lobj);
  if (ret) {
    return ret;
  }

  if (!lobj) {
    auto ret = objects_.emplace(oid, LogObject());
    lobj = &boost::get<LogObject>(ret.first->second);
  }

  auto it = lobj->entries.find(position);
  if (it == lobj->entries.end()) {
    LogEntry entry;
    entry.trimmed = true;
    entry.invalidated = true;
    lobj->entries.emplace(position, entry);
    lobj->maxpos = std::max(lobj->maxpos, position);
    return 0;
  } else {
    auto& entry = it->second;
    if (entry.trimmed || entry.invalidated) {
      return 0;
    }
    return -EROFS;
  }
}

int RAMBackend::Seal(const std::string& oid, uint64_t epoch)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  if (epoch == 0) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  auto ret = objects_.emplace(oid, LogObject());
  auto& obj = boost::get<LogObject>(ret.first->second);

  // if exists, verify the new epoch is larger
  if (!ret.second) {
    if (epoch <= obj.epoch) {
      return -ESPIPE;
    }
  }

  obj.epoch = epoch;

  return 0;
}

int RAMBackend::MaxPos(const std::string& oid, uint64_t epoch,
    uint64_t *pos, bool *empty)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  if (epoch == 0) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> lk(lock_);

  LogObject *lobj = nullptr;
  int ret = CheckEpoch(epoch, oid, true, lobj);
  if (ret) {
    return ret;
  }

  if (lobj) {
    bool is_empty = lobj->entries.empty();
    if (!is_empty)
      *pos = lobj->maxpos;
    *empty = is_empty;
  } else {
    *empty = true;
  }

  return 0;
}

int RAMBackend::CheckEpoch(uint64_t epoch, const std::string& oid,
    bool eq, LogObject*& lobj)
{
  auto it = objects_.find(oid);
  if (it == objects_.end()) {
    return -ENOENT;
  }

  lobj = &boost::get<LogObject>(it->second);

  if (eq) { 
    if (epoch != lobj->epoch) {
      return -ESPIPE;
    }
  } else if (epoch < lobj->epoch) {
    return -ESPIPE;
  }
  return 0;
}

extern "C" Backend *__backend_allocate(void)
{
  auto b = new RAMBackend();
  return b;
}

extern "C" void __backend_release(Backend *p)
{
  RAMBackend *backend = (RAMBackend*)p;
  delete backend;
}

}
}
}
