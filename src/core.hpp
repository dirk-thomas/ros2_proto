#ifndef ROS2_CORE_HPP_
#define ROS2_CORE_HPP_

#include <vector>
#include <deque>

#include <boost/scoped_array.hpp>
#include <boost/bind.hpp>
#include <boost/thread/condition_variable.hpp>

#include "transport_tcp.h"
#include "ros2_comm/TopicRequest.h"

#define DISALLOW_COPY_AND_ASSIGN(Type) \
  Type(const Type &other);             \
  void operator=(const Type &other);

namespace ros2 {

uint64_t usectime() {
  timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  uint32_t sec = now.tv_sec;
  uint32_t nsec = now.tv_nsec;
  uint64_t time = sec * 1000000ULL + nsec / 1000U;
  return time;
}

class Message {
public:
  Message(uint32_t sz) : sz_(sz) {
    bytes_.reset(new uint8_t[sz]);
  }

  uint32_t size() const { return sz_; }
  const uint8_t *bytes() const { return bytes_.get(); }
  uint8_t *bytes() { return bytes_.get(); }

  Message* Copy() const {
    Message *msg = new Message(sz_);
    std::copy(bytes_.get(), bytes_.get() + sz_, msg->bytes_.get());
    return msg;
  }

private:
  DISALLOW_COPY_AND_ASSIGN(Message);
  uint32_t sz_;
  boost::scoped_array<uint8_t> bytes_;
};

template<typename T>
class MessageQueue {
public:
  MessageQueue(size_t max_size) : max_size_(max_size) {}

  void push(T* msg) {
    boost::unique_lock<boost::mutex> lock(mutex_);
    queue_.push_back(msg);
    if (max_size_ != 0 && queue_.size() > max_size_) {
      delete queue_.front();
      queue_.pop_front();
    }
    cond_.notify_one();
  }

  T* pop(double msec_wait) {
    boost::unique_lock<boost::mutex> lock(mutex_);
    boost::system_time stop = boost::get_system_time() + boost::posix_time::milliseconds(msec_wait);
    while (queue_.empty()) {
      if (!cond_.timed_wait(lock, stop)) {
        return NULL;
      }
    }

    T* front = queue_.front();
    queue_.pop_front();
    return front;
  }

private:
  boost::mutex mutex_;
  boost::condition_variable cond_;
  std::deque<T*> queue_;
  size_t max_size_;
};

//============================= ServiceManager ==============================//

template <typename M>
class ServiceManager {
public:
  typedef typename M::Request Request;
  typedef typename M::Response Response;

  typedef boost::function<void(const Request&, Response*)> Callback;

  ServiceManager(TransportTCPPtr server) : server_(server) {

  }

  ~ServiceManager() {
    server_->close();
  }

  const TransportTCP& socket() {
    return *server_;
  }

  void init(int port) {
    if (!server_->listen(port, 5, boost::bind(&ServiceManager::onAccept, this, _1))) {
      ROS_ERROR("Failed to listen");
      ROS_BREAK();
    }
  }

  void bind(const Callback &cb) {
    boost::mutex::scoped_lock lock(mutex_);
    cb_ = cb;
  }

  void onAccept(const TransportTCPPtr &tcp) {
    boost::mutex::scoped_lock lock(mutex_);
    ROS_INFO("Got a connection from %s", tcp->getClientURI().c_str());
    tcp->enableMessagePass();
    tcp->enableRead();
    tcp->setMsgCallback(boost::bind(&ServiceManager::onReceive, this, tcp, _1));
    tcp->setDisconnectCallback(boost::bind(&ServiceManager::onDisconnect, this, _1));
    connections_.push_back(tcp);
  }

  void onReceive(const TransportTCPPtr &tcp,
                 const std::vector<Frame> &frames) {
    if (frames.size() != 1) {
      ROS_ERROR("Got %zu frames; expected 1", frames.size());
      ROS_BREAK();
    }
    const boost::shared_array<uint8_t> &bytes = frames[0].data;
    uint32_t sz = frames[0].size;

    ROS_INFO("Received a message!");
    Request req;
    Response rep;

    ros::serialization::IStream istream(bytes.get(), sz);
    ros::serialization::Serializer<Request>::read(istream, req);

    {
      boost::mutex::scoped_lock lock(mutex_);
      cb_(req, &rep);
    }
    uint32_t rep_sz = ros::serialization::serializationLength(rep);
    boost::shared_array<uint8_t> rep_bytes(new uint8_t[rep_sz]);
    ros::serialization::OStream ostream(rep_bytes.get(), rep_sz);
    ros::serialization::serialize(ostream, rep);
    ROS_INFO_STREAM("Sending response: " << rep);
    tcp->sendMessage(rep_bytes, rep_sz);
  }

  void onDisconnect(const TransportPtr &tcp) {
    boost::mutex::scoped_lock lock(mutex_);

    for (std::list<TransportTCPPtr>::iterator it = connections_.begin();
         it != connections_.end(); ++it) {
      if (*it == tcp) {
        connections_.erase(it);
        return;
      }
    }
    ROS_ERROR("Disconnect from TransportPtr, but not in list!");
  }

private:
  boost::mutex mutex_;
  TransportTCPPtr server_;
  std::list<TransportTCPPtr> connections_;
  Callback cb_;
};

template <typename M>
class ServiceClient {
public:
  ServiceClient() : req_rep_(NULL), done_(true) {

  }

  void call(boost::shared_ptr<TransportTCP> server, M *req_rep) {
    server->enableMessagePass();
    server->enableRead();
    server->setMsgCallback(boost::bind(&ServiceClient::onResponse, this, _1));

    uint32_t req_sz = ros::serialization::serializationLength(req_rep->request);
    boost::shared_array<uint8_t> req_bytes(new uint8_t[req_sz]);
    ros::serialization::OStream ostream(req_bytes.get(), req_sz);
    ros::serialization::serialize(ostream, req_rep->request);
    server->sendMessage(req_bytes, req_sz);
    {
      boost::unique_lock<boost::mutex> lock(mutex_);
      req_rep_ = req_rep;
      done_ = false;
      while (done_ == false) {
        cv_.wait(lock);
      }
    }
    req_rep_ = NULL;
  }

private:
  void onResponse(const std::vector<Frame> &frames) {
    boost::unique_lock<boost::mutex> lock(mutex_);
    if (frames.size() != 1) {
      ROS_ERROR("Got %zu frames; expected 1", frames.size());
      ROS_BREAK();
    }
    const boost::shared_array<uint8_t> &bytes = frames[0].data;
    uint32_t sz = frames[0].size;


    if (done_ != false) {
      ROS_ERROR("done_ is not false; unexpected request?");
      ROS_BREAK();
    }
    ros::serialization::IStream istream(bytes.get(), sz);
    ros::serialization::Serializer<typename M::Response>::read(istream, req_rep_->response);
    done_ = true;
    cv_.notify_all();
  }

  boost::mutex mutex_;
  boost::condition_variable cv_;
  M *req_rep_;
  bool done_;
};

} // ros2

#endif /* ROS2_CORE_HPP_ */
