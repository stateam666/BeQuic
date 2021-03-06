#include "net/tools/quic/be_quic_client.h"
#include "net/tools/quic/be_quic_fake_proof_verifier.h"
#include "net/tools/quic/be_quic_spdy_client_stream.h"
#include "net/base/net_errors.h"
#include "net/base/privacy_mode.h"
#include "net/base/address_list.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/ct_log_verifier.h"
#include "net/cert/ct_policy_enforcer.h"
#include "net/cert/multi_log_ct_verifier.h"
#include "net/http/transport_security_state.h"
#include "net/quic/crypto/proof_verifier_chromium.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/dns/host_resolver_proc.h"
#include "net/tools/quic/synchronous_host_resolver.h"
#include "net/third_party/quiche/src/quic/core/quic_error_codes.h"
#include "net/third_party/quiche/src/quic/core/quic_packets.h"
#include "net/third_party/quiche/src/quic/core/quic_server_id.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_socket_address.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_string_piece.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_text_utils.h"
#include "net/third_party/quiche/src/quic/tools/quic_client_base.h"
#include "net/third_party/quiche/src/spdy/core/spdy_header_block.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "url/gurl.h"

using net::CertVerifier;
using net::CTVerifier;
using net::MultiLogCTVerifier;
using quic::ProofVerifier;
using net::ProofVerifierChromium;
using quic::QuicStringPiece;
using quic::QuicTextUtils;
using net::TransportSecurityState;
using spdy::SpdyHeaderBlock;

#define AVSEEK_SIZE     0x10000
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

namespace net {

const int kReadBlockSize = 32768;

BeQuicClient::BeQuicClient(int handle)
    : base::SimpleThread("BeQuic"),
      handle_(handle),
      busy_(false),
      running_(false),
      istream_(&response_buff_),
      ostream_(&response_buff_) {
    LOG(INFO) << "BeQuicClient created " << handle_ << std::endl;
}

BeQuicClient::~BeQuicClient() {
    LOG(INFO) << "BeQuicClient deleted " << handle_ << std::endl;
}

int BeQuicClient::open(
    const std::string& url,
    const char *ip,
    unsigned short port,
    const std::string& method,
    std::vector<InternalQuicHeader> headers,
    const std::string& body,
    bool verify_certificate,
    int ietf_draft_version,
    int handshake_version,
    int transport_version,
    int block_size,
    int block_consume,
    int timeout) {
    int ret = 0;
    do {
        if (url.empty()) {
            ret = kBeQuicErrorCode_Invalid_Param;
            break;
        }

        if (busy_) {
            ret = kBeQuicErrorCode_Invalid_State;
            break;
        }

        //Save parameters.
        url_                = url;
        mapped_ip_          = (ip == NULL) ? "" : ip;
        mapped_port_        = port;
        method_             = method;
        headers_            = headers;
        body_               = body;
        verify_certificate_ = verify_certificate;
        ietf_draft_version_ = ietf_draft_version;
        handshake_version_  = handshake_version;
        transport_version_  = transport_version;
        block_size_         = block_size;
        block_consume_      = block_consume;

        //Create promise for blocking wait.
        IntPromisePtr open_promise;
        if (timeout != 0) {
            open_promise_.reset(new std::promise<int>);
            open_promise = open_promise_;
        }

        //Start thread.
        Start();

        //Set busy flag.
        busy_ = true;

        //If won't blocking.
        if (!open_promise_)  {
            break;
        }

        //Wait forever if timeout set to -1.
        IntFuture future = open_promise_->get_future();
        if (timeout < 0) {
            ret = future.get(); //Blocking.
            break;
        }

        //Wait for certain time.
        std::future_status status =
            future.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout));
        if (status == std::future_status::ready) {
            ret = future.get();
            break;
        }

        ret = kBeQuicErrorCode_Timeout;
    } while (0);
    return ret;
}

void BeQuicClient::close() {
    //Stop thread.
    if (!busy_) {
        return;
    }

    //Trick, wait until thread started.
    while (!running_) {
        base::PlatformThread::Sleep(base::TimeDelta::FromMilliseconds(50));
    }

    //Stop message loop.
    running_ = false;
    if (message_loop_ != NULL && run_loop_ != NULL) {
        message_loop_->task_runner()->PostTask(FROM_HERE, run_loop_->QuitClosure());
    }

    //Wait for thread exit.
    Join();

    //Set busy flag, the invoke thread can now call open again.
    busy_ = false;
}

int BeQuicClient::read_buffer(unsigned char *buf, int size, int timeout) {
    int ret = 0;
    do {
        if (!running_) {
            ret = kBeQuicErrorCode_Invalid_State;
            break;
        }

        if (buf == NULL || size == 0) {
            ret = kBeQuicErrorCode_Invalid_Param;
            break;
        }

        //TBD:Chunk?
        if (content_length_ > 0 && read_offset_ >= content_length_) {
            ret = kBeQuicErrorCode_Eof;
            break;
        }

        std::unique_lock<std::mutex> lock(data_mutex_);
        while (!is_buffer_sufficient()) {
            if (timeout > 0) {
                //Wait for certain time.
                //LOG(INFO) << "buf size 0 will wait " << timeout << "ms" << std::endl;
                data_cond_.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(timeout));
            } else if (timeout < 0) {
                //Wait forever.
                data_cond_.wait(lock);
            }
            break;
        }
        size_t read_len = std::min<size_t>((size_t)size, response_buff_.size());
        if (read_len == 0) {
            break;
        }

        istream_.read((char*)buf, read_len);
        read_offset_ += read_len;
        ret = (int)read_len;
        //LOG(INFO) << "buf read " << ret << "byte" << std::endl;
    } while (0);
    return ret;
}

int64_t BeQuicClient::seek(int64_t off, int whence) {
    int64_t ret = -1;
    do {
        if (!running_) {
            ret = kBeQuicErrorCode_Invalid_State;
            break;
        }

        if (message_loop_ == NULL) {
            ret = kBeQuicErrorCode_Null_Pointer;
            break;
        }

        IntPromisePtr promise(new IntPromise);
        message_loop_->task_runner()->PostTask(
            FROM_HERE,
            base::BindOnce(
                &BeQuicClient::seek_internal,
                base::Unretained(this),
                off,
                whence,
                promise));

        IntFuture future = promise->get_future();
        ret = future.get();
        LOG(INFO) << "Seek " << off << " " << whence << " return " << ret << std::endl;
    } while (0);
    return ret;
}

int BeQuicClient::get_stats(BeQuicStats *stats) {
    int ret = 0;
    do {
        if (stats == NULL) {
            ret = kBeQuicErrorCode_Invalid_Param;
            break;
        }

        if (!running_) {
            ret = kBeQuicErrorCode_Invalid_State;
            break;
        }

        if (message_loop_ == NULL) {
            ret = kBeQuicErrorCode_Null_Pointer;
            break;
        }

        IntPromisePtr promise(new IntPromise);
        message_loop_->task_runner()->PostTask(
            FROM_HERE,
            base::BindOnce(
                &BeQuicClient::get_stats_internal,
                base::Unretained(this),
                stats,
                promise));

        IntFuture future = promise->get_future();
        ret = future.get();
    } while (0);
    return ret;
}

void BeQuicClient::on_data(quic::QuicSpdyClientStream *stream, char *buf, int size) {
    if (stream == NULL) {
        return;
    }

    if (current_stream_id_ == 0) {
        current_stream_id_ = stream->id();
        LOG(INFO) << "Bound to stream " << current_stream_id_ << std::endl;
    }

    if (!got_first_data_) {
        quic::BeQuicSpdyClientStream* bequic_stream = static_cast<quic::BeQuicSpdyClientStream*>(stream);
        content_length_     = bequic_stream->check_content_length();
        first_data_time_    = base::Time::Now();
        got_first_data_     = true;
    }

    if (buf != NULL && size > 0) {
        std::unique_lock<std::mutex> lock(data_mutex_);
        ostream_.write(buf ,size);
        if (is_buffer_sufficient()) {
            //LOG(INFO) << "buf write one block " << response_buff_.size() << std::endl;
            data_cond_.notify_all();
        }
    }
}

void BeQuicClient::Run() {
    LOG(INFO) << "Thread handle " << handle_ << " run." << std::endl;

    //Thread is running now.
    running_ = true;

    //Bind message loop.
    std::unique_ptr<base::MessageLoopForIO> message_loop(new base::MessageLoopForIO);
    std::unique_ptr<base::RunLoop> run_loop(new base::RunLoop);

    //For invoking from another thread.
    message_loop_   = message_loop.get();
    run_loop_       = run_loop.get();

    //Internal request.
    int ret = internal_request(
        url_,
        mapped_ip_,
        mapped_port_,
        method_,
        headers_,
        body_,
        verify_certificate_,
        ietf_draft_version_,
        handshake_version_,
        transport_version_,
        block_size_,
        block_consume_);

    //Causing invoke thread out of block after connect and handshake finished.
    if (open_promise_) {
        open_promise_->set_value(ret);
        open_promise_.reset();
    }

    //Event loop.
    run_event_loop();

    //Disconnect quic client in this thread.
    if (spdy_quic_client_) {
        spdy_quic_client_->Disconnect();
        spdy_quic_client_.reset();
    }

    //Release promise if any.
    if (open_promise_) {
        open_promise_->set_value(0);
        open_promise_.reset();
    }

    //Reset all members.
    headers_.clear();
    url_                    = "";
    method_                 = "";
    body_                   = "";
    verify_certificate_     = true;
    ietf_draft_version_     = -1;
    handshake_version_      = -1;
    transport_version_      = -1;
    message_loop_           = NULL;
    run_loop_               = NULL;
    running_                = false;

    LOG(INFO) << "Thread handle " << handle_ << " exit." << std::endl;
}

void BeQuicClient::run_event_loop() {
    run_loop_->Run();
}

int BeQuicClient::internal_request(
    const std::string& url,
    const std::string& mapped_ip,
    unsigned short mapped_port,
    const std::string& method,
    std::vector<InternalQuicHeader> headers,
    const std::string& body,
    bool verify_certificate,
    int ietf_draft_version,
    int handshake_version,
    int transport_version,
    int block_size,
    int block_consume) {
    int ret = kBeQuicErrorCode_Success;
    do {
        start_time_ = base::Time::Now();

        //Parse host and port from url.
        GURL gurl(url);
        std::string host    = gurl.host();
        int port            = gurl.EffectiveIntPort();

        //Check mapped port.
        if (mapped_port > 0) {
            port = mapped_port;
        }
        
        LOG(INFO) << "BeQuicOpen " << host << ":" << port << " => " << url << "," << method << std::endl;

        net::AddressList addresses;
        if (!mapped_ip.empty()) {
            //Check mapped ip.
            std::vector<std::string> numbers = base::SplitString(mapped_ip, ".", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
            if (numbers.size() != 4) {
                ret = kBeQuicErrorCode_Invalid_Param;
                break;
            }

            IPAddress addr(atoi(numbers[0].c_str()), atoi(numbers[1].c_str()), atoi(numbers[2].c_str()), atoi(numbers[3].c_str()));
            addresses = AddressList::CreateFromIPAddress(addr, port);
        } else {
#ifdef ANDROID
            int os_error = 0;
            SystemHostResolverCall(host, ADDRESS_FAMILY_UNSPECIFIED, 0, &addresses, &os_error);
            if (os_error != 0) {
                LOG(ERROR) << "SystemHostResolverCall error " << os_error << std::endl;
                ret = kBeQuicErrorCode_Resolve_Fail;
                break;
            }
#else
            if (net::SynchronousHostResolver::Resolve(host, &addresses) != net::OK) {
                //Resolve host to address synchronously.
                ret = kBeQuicErrorCode_Resolve_Fail;
                break;
            }
#endif
        }

        base::Time resolved_time = base::Time::Now();
        base::TimeDelta resolve_time = resolved_time - start_time_;
        resolve_time_ = resolve_time.InMicroseconds();

        //Make up QuicIpAddress.
        quic::QuicIpAddress ip_addr = quic::QuicIpAddress(quic::QuicIpAddressImpl(addresses[0].address()));
        LOG(INFO) << "Resolve to " << ip_addr.ToString() << " using " << resolve_time_ / 1000 << " ms." << std::endl;

        //Make up serverid.
        quic::QuicServerId serverId(gurl.host(), gurl.EffectiveIntPort(), net::PRIVACY_MODE_DISABLED);

        //Get Quic version.
        quic::ParsedQuicVersionVector versions;
        if (transport_version == -1) {
            versions = quic::CurrentSupportedVersions();
        } else {
            versions.emplace_back(
                static_cast<quic::HandshakeProtocol>(handshake_version), 
                static_cast<quic::QuicTransportVersion>(transport_version));
        }

        for (auto iter = versions.begin(); iter != versions.end(); ++iter) {
            LOG(INFO) << "Handshake version:" << iter->handshake_protocol 
                      << ", transport version:" << iter->transport_version << std::endl;
        }

        //Create certificate verifier.
        std::unique_ptr<CertVerifier>           cert_verifier(CertVerifier::CreateDefault());
        std::unique_ptr<TransportSecurityState> transport_security_state(new TransportSecurityState);
        std::unique_ptr<MultiLogCTVerifier>     ct_verifier(new MultiLogCTVerifier());
        std::unique_ptr<net::CTPolicyEnforcer>  ct_policy_enforcer(new net::DefaultCTPolicyEnforcer());
        std::unique_ptr<quic::ProofVerifier>    proof_verifier;
        if (!verify_certificate) {
            proof_verifier.reset(new quic::BeQuicFakeProofVerifier());
        } else {
            proof_verifier.reset(new ProofVerifierChromium(
            cert_verifier.get(),
            ct_policy_enforcer.get(),
            transport_security_state.get(),
            ct_verifier.get()));
        }

        //Must create real client in this thread or tls object won't work.
        if (spdy_quic_client_ == NULL) {
            spdy_quic_client_.reset(new BeQuicSpdyClient(
                quic::QuicSocketAddress(ip_addr, port),
                serverId,
                versions,
                std::move(proof_verifier),
                shared_from_this()));
        }

        //Set MTU.
        spdy_quic_client_->set_initial_max_packet_length(quic::kDefaultMaxPacketSize);

        LOG(INFO) << "Initializing!" << std::endl;

        //Initialize quic client.
        if (!spdy_quic_client_->Initialize()) {
            ret = kBeQuicErrorCode_Fatal_Error;
            LOG(ERROR) << "Failed to initialize bequic client." << std::endl;
            break;
        }

        LOG(INFO) << "Initialized!" << std::endl;

        //Do connecting and handshaking.
        if (!spdy_quic_client_->Connect()) {
            ret = kBeQuicErrorCode_Connect_Fail;
            quic::QuicErrorCode error = spdy_quic_client_->session()->error();
            LOG(ERROR) << "BeQuic connect error " << quic::QuicErrorCodeToString(error) << std::endl;
            break;
        }

        base::Time connected_time = base::Time::Now();
        base::TimeDelta connect_time = connected_time - start_time_;
        connect_time_ = connect_time.InMicroseconds();

        LOG(INFO) << "Connected, using " << connect_time_ / 1000 << " ms." << std::endl;

        header_block_[":method"]      = method;
        header_block_[":scheme"]      = gurl.scheme();
        header_block_[":authority"]   = gurl.host();
        std::string path;
        if (gurl.has_query()) {
            path = gurl.path() + "?" + gurl.query();
        } else {
            path = gurl.path();
        }
        header_block_[":path"]        = path;
        LOG(INFO) << "path: " << path << std::endl;

        for (size_t i = 0; i < headers.size(); ++i) {
            InternalQuicHeader &header = headers[i];
            if (header.key.empty() || header.value.empty()) {
                continue;
            }  

            QuicStringPiece key     = header.key;
            QuicStringPiece value   = header.value; 
            quic::QuicTextUtils::RemoveLeadingAndTrailingWhitespace(&key);
            quic::QuicTextUtils::RemoveLeadingAndTrailingWhitespace(&value);
            header_block_[key]       = value;
        }

        spdy_quic_client_->set_store_response(true);

        LOG(INFO) << "SendRequesting!" << std::endl;

        spdy_quic_client_->SendRequest(header_block_, body, true);

        LOG(INFO) << "SendRequested!" << std::endl;

        /*
        //For small file.
        spdy_quic_client_->SendRequestsAndWaitForResponse(header_block, body, true);
        size_t response_code         = spdy_quic_client_->latest_response_code();
        std::string response_body    = spdy_quic_client_->latest_response_body();

        LOG(INFO) << "Request:"     << std::endl;
        LOG(INFO) << "headers:"     << header_block.DebugString() << std::endl;
        LOG(INFO) << "Response:"    << response_code << std::endl;
        LOG(INFO) << "headers: "    << spdy_quic_client_->latest_response_headers() << std::endl;
        LOG(INFO) << "trailers: "   << spdy_quic_client_->latest_response_trailers() << std::endl;
        */
    } while (0);
    return ret;
}

void BeQuicClient::seek_internal(int64_t off, int whence, IntPromisePtr promise) {
    int ret = -1;
    do {
        if (spdy_quic_client_ == NULL) {
            ret = kBeQuicErrorCode_Invalid_State;
            break;
        }

        int64_t target_offset = -1;
        ret = seek_in_buffer(off, whence, &target_offset);
        if (ret == kBeQuicErrorCode_Buffer_Not_Hit) {
            ret = seek_from_net(target_offset);
        }
    } while (0);

    if (promise != NULL) {
        promise->set_value(ret);
    }
}

int64_t BeQuicClient::seek_in_buffer(int64_t off, int whence, int64_t *target_off) {
    //Since calling from worker thread, lock is unnecessary.
    //std::unique_lock<std::mutex> lock(mutex_);
    int64_t ret = -1;
    do {
        if (content_length_ == -1) {
            ret = kBeQuicErrorCode_Not_Supported;
            break;
        }

        if (whence == AVSEEK_SIZE) {
            ret = content_length_;
            break;
        }

        if ((whence == SEEK_CUR && off == 0) || (whence == SEEK_SET && off == read_offset_)) {
            ret = off;
            break;
        }

        if (content_length_ == -1 && whence == SEEK_END) {
            ret = kBeQuicErrorCode_Invalid_State;
            break;
        }

        if (whence == SEEK_CUR) {
            off += read_offset_;
        } else if (whence == SEEK_END) {
            off += content_length_;
        } else if (whence != SEEK_SET) {
            ret = kBeQuicErrorCode_Invalid_Param;
            break;
        }

        if (off < 0) {
            ret = kBeQuicErrorCode_Invalid_Param;
            break;
        }

        //Check if hit the buffer.
        int64_t left_size = (int64_t)response_buff_.size();
        int64_t consume_size = off - read_offset_;

        if (consume_size > 0 && left_size > consume_size) {
            response_buff_.consume(consume_size);
            read_offset_ = off;
            ret = off;
            break;
        }

        if (target_off != NULL) {
            *target_off = off;
        }

        ret = kBeQuicErrorCode_Buffer_Not_Hit;
    } while (0);

    LOG(INFO) << "seek_in_buffer " << off << " " << whence << " return "  << ret << std::endl;
    return ret;
}

int64_t BeQuicClient::seek_from_net(int64_t off) {
    int64_t ret = -1;
    do {
        if (spdy_quic_client_ == NULL || off < 0) {
            break;
        }

        //Close current stream.
        close_current_stream();

        //Reset read offset.
        read_offset_ = off;

        //Drop all data in buffer.
        response_buff_.consume(response_buff_.size());

        //If already disconnected, reconnect now.
        if (!spdy_quic_client_->connected()) {
            LOG(INFO) << "Reconnecting." << std::endl;

            //Initialize quic client.
            if (!spdy_quic_client_->Initialize()) {
                ret = kBeQuicErrorCode_Fatal_Error;
                LOG(ERROR) << "Failed to initialize bequic client." << std::endl;
                break;
            }

            auto start_time = base::Time::Now();

            //Reconnect.
            if (spdy_quic_client_->Connect()) {
                base::Time connected_time = base::Time::Now();
                base::TimeDelta connect_time = connected_time - start_time;
                LOG(INFO) << "Reconnect success, using " << connect_time.InMicroseconds() / 1000 << " ms." << std::endl;
            } else {
                LOG(ERROR) << "Reconnect failed." << std::endl;
                break;
            }
        }

        std::ostringstream os;
        os << "bytes=" << off << "-";
        header_block_["range"] = os.str();

        spdy_quic_client_->SendRequest(header_block_, "", true);
        ret = off;
    } while (0);
    return ret;
}

void BeQuicClient::get_stats_internal(BeQuicStats *stats, IntPromisePtr promise) {
    int ret = kBeQuicErrorCode_Success;
    do {
        if (spdy_quic_client_ == NULL) {
            ret = kBeQuicErrorCode_Invalid_State;
            break;
        }

        quic::QuicSession *session = spdy_quic_client_->session();
        if (session == NULL) {
            ret = kBeQuicErrorCode_Null_Pointer;
            break;
        }

        quic::QuicConnection *connection = session->connection();
        if (connection == NULL) {
            ret = kBeQuicErrorCode_Connect_Fail;
            break;
        }

        const quic::QuicConnectionStats &quic_stats = connection->GetStats();
        stats->packets_lost             = static_cast<bequic_int64_t>(quic_stats.packets_lost);
        stats->packets_reordered        = static_cast<bequic_int64_t>(quic_stats.packets_reordered);
        stats->rtt                      = static_cast<bequic_int64_t>(quic_stats.srtt_us);
        stats->bandwidth                = static_cast<bequic_int64_t>(quic_stats.estimated_bandwidth.ToBitsPerSecond());
        stats->resolve_time             = static_cast<bequic_int64_t>(resolve_time_);
        stats->connect_time             = static_cast<bequic_int64_t>(connect_time_);

        if (!first_data_time_.is_null()) {
            base::TimeDelta first_data_delta = first_data_time_ - start_time_;
            stats->first_data_receive_time  = static_cast<bequic_int64_t>(first_data_delta.InMicroseconds());
        }
    } while (0);

    if (promise != NULL) {
        promise->set_value(ret);
    }
}

bool BeQuicClient::close_current_stream() {
    bool ret = true;
    do {
        if (spdy_quic_client_ == NULL || current_stream_id_ == 0) {
            ret = false;
            break;
        }

        quic::QuicSession *session = spdy_quic_client_->session();
        if (session == NULL) {
            ret = false;
            break;
        }

        LOG(INFO) << "Closing stream " << current_stream_id_ << std::endl;

        //Close quic stream, send Reset frame to close peer stream.
        session->SendRstStream(current_stream_id_, quic::QUIC_STREAM_CANCELLED, 0);
        session->CloseStream(current_stream_id_);

        current_stream_id_ = 0;
    } while (0);
    return ret;
}

bool BeQuicClient::is_buffer_sufficient() {
    bool ret = true;
    do {
        size_t size = response_buff_.size();
        if (content_length_ == -1) {
            //Cannot determine end of stream, so if some data exists just return true for safe.
            ret = size > 0;
            break;
        }

        if (size == 0) {
            ret = false;
            break;
        }

        if (content_length_ - read_offset_ < kReadBlockSize) {
            ret = true;
            break;
        }

        if (size < kReadBlockSize) {
            ret = false;
            break;
        }

        ret = true;
    } while (0);
    return ret;
}

}  // namespace net
