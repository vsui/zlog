#include <sstream>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "storage/ceph/protobuf_bufferlist_adapter.h"
#include "zlog/backend/ceph.h"
#include "cls_zlog_client.h"
#include "storage/ceph/cls_zlog.pb.h"

namespace zlog {
namespace storage {
namespace ceph {

CephBackend::CephBackend() :
  cluster_(nullptr),
  ioctx_(nullptr)
{
}

CephBackend::CephBackend(librados::IoCtx *ioctx) :
  cluster_(nullptr),
  ioctx_(ioctx),
  pool_(ioctx_->get_pool_name())
{
  options["scheme"] = "ceph";
  options["conf_file"] = "";
  options["pool"] = pool_;
}

CephBackend::~CephBackend()
{
  // cluster_ is only non-null when it was created via Initialize() in which
  // case the backend owns both the cluster and ioctx and needs to release them.
  if (cluster_) {
    ioctx_->close();
    delete ioctx_;
    cluster_->shutdown();
    delete cluster_;
  }
}

std::map<std::string, std::string> CephBackend::meta()
{
  return options;
}

int CephBackend::Initialize(
    const std::map<std::string, std::string>& opts)
{
  auto cluster = new librados::Rados;

  // initialize cluster
  auto it = opts.find("id");
  auto id = it == opts.end() ? nullptr : it->second.c_str();
  int ret = cluster->init(id);
  if (ret) {
    delete cluster;
    return ret;
  }

  // read conf file?
  it = opts.find("conf_file");
  if (it != opts.end()) {
    auto conf_file = it->second.empty() ? nullptr : it->second.c_str();
    ret = cluster->conf_read_file(conf_file);
    if (ret) {
      delete cluster;
      return ret;
    }
  }

  ret = cluster->connect();
  if (ret) {
    delete cluster;
    return ret;
  }

  // pool
  it = opts.find("pool");
  auto pool = it == opts.end() ? "zlog" : it->second;

  auto ioctx = new librados::IoCtx;
  ret = cluster->ioctx_create(pool.c_str(), *ioctx);
  if (ret) {
    delete ioctx;
    cluster->shutdown();
    delete cluster;
    return ret;
  }

  options = opts;
  options["scheme"] = "ceph";

  cluster_ = cluster;
  ioctx_ = ioctx;
  pool_ = ioctx_->get_pool_name();

  return 0;
}

int CephBackend::uniqueId(const std::string& hoid, uint64_t *id)
{
  if (hoid.empty()) {
    return -EINVAL;
  }

  while (true) {
    // read the stored id.
    uint64_t unique_id;
    {
      ::ceph::bufferlist bl;
      librados::ObjectReadOperation op;
      zlog::cls_zlog_read_unique_id(op);
      int ret = ioctx_->operate(hoid, &op, &bl);
      if (ret < 0) {
        if (ret == -ENOENT || ret == -ENODATA) {
          unique_id = 0;
        } else {
          return ret;
        }
      } else {
        zlog_ceph_proto::UniqueId msg;
        if (!decode(bl, &msg)) {
          return -EIO;
        }
        unique_id = msg.id();
      }
    }

    unique_id++;

    librados::ObjectWriteOperation op;
    zlog::cls_zlog_write_unique_id(op, unique_id);
    int ret = ioctx_->operate(hoid, &op);
    if (ret < 0) {
      if (ret == -ESTALE) {
        continue;
      }
      return ret;
    }

    *id = unique_id;
    return 0;
  }
}

int CephBackend::CreateLog(const std::string& name, const std::string& view,
    std::string *hoid_out, std::string *prefix_out)
{
  if (name.empty()) {
    return -EINVAL;
  }

  std::string hoid;
  std::string prefix;

  // create a uniquely named head object
  while (true) {
    boost::uuids::uuid uuid = boost::uuids::random_generator()();
    const auto key = boost::uuids::to_string(uuid);

    hoid = std::string("zlog.head.").append(key);
    // TODO: should the prefix include the trailing "."
    prefix = std::string("zlog.data.").append(key);

    int ret = InitHeadObject(hoid, prefix);
    if (ret) {
      if (ret == -EEXIST) {
        continue;
      }
      return ret;
    }

    break;
  }

  // propose the first view in the new hoid.
  int ret = ProposeView(hoid, 1, view);
  if (ret) {
    return ret;
  }

  // the head object now exists, but it is orphaned. a crash at this point is ok
  // because a gc process could later remove it. now we'll build a link from the
  // log name requested to the head object we just created.
  ret = CreateLinkObject(name, hoid);
  if (ret) {
    return ret;
  }

  if (hoid_out) {
    hoid_out->swap(hoid);
  }

  if (prefix_out) {
    prefix_out->swap(prefix);
  }

  return 0;
}

int CephBackend::OpenLog(const std::string& name, std::string *hoid_out,
    std::string *prefix_out)
{
  if (name.empty()) {
    return -EINVAL;
  }

  // read the hoid from the link object
  std::string hoid;
  {
    ::ceph::bufferlist bl;
    const auto link_oid = LinkObjectName(name);
    int ret = ioctx_->read(link_oid, bl, 0, 0);
    if (ret < 0) {
      return ret;
    }

    zlog_ceph_proto::LinkObjectHeader link;
    if (!decode(bl, &link)) {
      return -EIO;
    }

    hoid = link.hoid();
  }

  // load log metadata from the head object
  ::ceph::bufferlist bl;
  int ret = ioctx_->getxattr(hoid, HEAD_HEADER_KEY, bl);
  if (ret < 0) {
    if (ret == -ENOENT) {
      // ENOENT is returned when the log link object doesn't exist. The head
      // object may also not exist, but that would be a basic violation of
      // assumptions, so we convert this into an error that clients won't
      // confuse with a log not existing.
      return -EIO;
    }
    return ret;
  }

  zlog_ceph_proto::HeadObjectHeader head;
  if (!decode(bl, &head)) {
    return -EIO;
  }

  auto prefix = head.prefix();
  if (prefix.empty()) {
    return -EIO;
  }


  if (hoid_out) {
    hoid_out->swap(hoid);
  }

  if (prefix_out) {
    prefix_out->swap(prefix);
  }

  return 0;
}

int CephBackend::ReadViews(const std::string& hoid, uint64_t epoch,
    uint32_t max_views, std::map<uint64_t, std::string> *views_out)
{
  if (hoid.empty()) {
    return -EINVAL;
  }

  std::map<uint64_t, std::string> tmp;

  // read views starting with specified epoch
  librados::ObjectReadOperation op;
  zlog::cls_zlog_read_view(op, epoch, max_views);
  ::ceph::bufferlist bl;
  int ret = ioctx_->operate(hoid, &op, &bl);
  if (ret) {
    return ret;
  }

  zlog_ceph_proto::Views views;
  if (!decode(bl, &views)) {
    return -EIO;
  }

  // unpack into return map
  for (int i = 0; i < views.views_size(); i++) {
    auto view = views.views(i);
    std::string data(view.data().c_str(), view.data().size());
    auto res = tmp.emplace(view.epoch(), data);
    assert(res.second);
    (void)res;
  }

  views_out->swap(tmp);

  return 0;
}

int CephBackend::ProposeView(const std::string& hoid,
    uint64_t epoch, const std::string& view)
{
  if (hoid.empty()) {
    return -EINVAL;
  }

  ::ceph::bufferlist bl;
  bl.append(view.c_str(), view.size());
  librados::ObjectWriteOperation op;
  zlog::cls_zlog_create_view(op, epoch, bl);
  int ret = ioctx_->operate(hoid, &op);
  return ret;
}

int CephBackend::Read(const std::string& oid, uint64_t epoch,
    uint64_t position, std::string *data)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  librados::ObjectReadOperation op;
  zlog::cls_zlog_read(op, epoch, position);

  ::ceph::bufferlist bl;
  int ret = ioctx_->operate(oid, &op, &bl);
  if (ret) {
    return ret;
  }

  data->assign(bl.c_str(), bl.length());

  return 0;
}

int CephBackend::Write(const std::string& oid, const std::string& data,
    uint64_t epoch, uint64_t position)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  ::ceph::bufferlist data_bl;
  data_bl.append(data.data(), data.size());

  librados::ObjectWriteOperation op;
  zlog::cls_zlog_write(op, epoch, position, data_bl);

  return ioctx_->operate(oid, &op);
}

int CephBackend::Fill(const std::string& oid, uint64_t epoch,
    uint64_t position)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  librados::ObjectWriteOperation op;
  zlog::cls_zlog_invalidate(op, epoch, position, false);
  return ioctx_->operate(oid, &op);
}

int CephBackend::Trim(const std::string& oid, uint64_t epoch,
    uint64_t position)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  librados::ObjectWriteOperation op;
  zlog::cls_zlog_invalidate(op, epoch, position, true);
  return ioctx_->operate(oid, &op);
}

int CephBackend::Seal(const std::string& oid, uint64_t epoch)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  librados::ObjectWriteOperation op;
  zlog::cls_zlog_seal(op, epoch);
  return ioctx_->operate(oid, &op);
}

int CephBackend::MaxPos(const std::string& oid, uint64_t epoch,
    uint64_t *position_out, bool *empty_out)
{
  if (oid.empty()) {
    return -EINVAL;
  }

  librados::ObjectReadOperation op;
  zlog::cls_zlog_max_position(op, epoch);

  ::ceph::bufferlist bl;
  int ret = ioctx_->operate(oid, &op, &bl);
  if (ret) {
    return ret;
  }

  zlog_ceph_proto::MaxPos reply;
  if (!decode(bl, &reply)) {
    return -EIO;
  }

  *empty_out = !reply.has_pos();
  if (reply.has_pos()) {
    *position_out = reply.pos();
  }

  return 0;
}

std::string CephBackend::LinkObjectName(const std::string& name)
{
  std::stringstream ss;
  ss << "zlog.link." << name;
  return ss.str();
}

int CephBackend::CreateLinkObject(const std::string& name,
    const std::string& hoid)
{
  assert(!name.empty());
  assert(!hoid.empty());

  zlog_ceph_proto::LinkObjectHeader meta;
  meta.set_hoid(hoid);

  ::ceph::bufferlist bl;
  encode(bl, meta);

  librados::ObjectWriteOperation op;
  op.create(true);
  op.write_full(bl);

  auto loid = LinkObjectName(name);

  int ret = ioctx_->operate(loid, &op);
  return ret;
}

int CephBackend::InitHeadObject(const std::string& hoid,
    const std::string& prefix)
{
  librados::ObjectWriteOperation op;
  zlog::cls_zlog_init_head(op, prefix);
  return ioctx_->operate(hoid, &op);
}

extern "C" Backend *__backend_allocate(void)
{
  auto b = new CephBackend();
  return b;
}

extern "C" void __backend_release(Backend *p)
{
  CephBackend *backend = (CephBackend*)p;
  delete backend;
}

}
}
}
