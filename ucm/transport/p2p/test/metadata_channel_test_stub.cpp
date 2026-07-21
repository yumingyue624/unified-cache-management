#include <utility>
#include "control/metadata_channel.h"

namespace transport {

MetadataChannel::SocketHandle::SocketHandle() : socket_(-1) {}
MetadataChannel::SocketHandle::SocketHandle(int socket) : socket_(socket) {}
MetadataChannel::SocketHandle::~SocketHandle() = default;

MetadataChannel::SocketHandle::SocketHandle(SocketHandle&& other) noexcept
    : socket_(other.Release())
{
}

MetadataChannel::SocketHandle& MetadataChannel::SocketHandle::operator=(
    SocketHandle&& other) noexcept
{
    if (this != &other) { socket_ = other.Release(); }
    return *this;
}

bool MetadataChannel::SocketHandle::Valid() const { return socket_ >= 0; }
int MetadataChannel::SocketHandle::Get() const { return socket_; }

int MetadataChannel::SocketHandle::Release()
{
    const int socket = socket_;
    socket_ = -1;
    return socket;
}

void MetadataChannel::SocketHandle::Reset(int socket) { socket_ = socket; }

MetadataChannel::MetadataChannel() = default;
MetadataChannel::~MetadataChannel() { Close(); }

Status MetadataChannel::Init(const Endpoint& endpoint, MetadataRequestHandler handler)
{
    if (endpoint.host.empty() || endpoint.port == 0 || !handler) { return Status::InvalidArgument; }
    endpoint_ = endpoint;
    metadata_request_handler_ = std::move(handler);
    return Status::Ok;
}

Status MetadataChannel::ExchangeMetadata(const Endpoint&, const Metadata&, Metadata&)
{
    return Status::Failed;
}

void MetadataChannel::Close()
{
    metadata_request_handler_ = {};
    listen_socket_.Reset();
    socket_.Reset();
}

}  // namespace transport
