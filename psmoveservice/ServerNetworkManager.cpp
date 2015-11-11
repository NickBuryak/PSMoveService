//-- includes -----
#include "ServerNetworkManager.h"
#include "packedmessage.h"
#include "DataFrameInterface.h"
#include "PSMoveDataFrame.pb.h"
#include <cassert>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <deque>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/cstdint.hpp>
#include <boost/enable_shared_from_this.hpp>

//-- pre-declarations -----
using namespace std;
namespace asio = boost::asio;
using asio::ip::tcp;
using asio::ip::udp;
using boost::uint8_t;

class ClientConnection;
typedef boost::shared_ptr<ClientConnection> ClientConnectionPtr;

typedef map<int, ClientConnectionPtr> t_client_connection_map;
typedef map<int, ClientConnectionPtr>::iterator t_client_connection_map_iter;
typedef std::pair<int, ClientConnectionPtr> t_id_client_connection_pair;

//-- constants -----
#define DEBUG true

//-- private implementation -----
// -ClientConnection-
// Maintains TCP and UDP connection state to a single client.
// Handles async socket callbacks on the connection.
// Routes requests through the request handler
class ClientConnection : public boost::enable_shared_from_this<ClientConnection>
{
public:
    virtual ~ClientConnection()
    {
        // Socket should have been closed by this point
        assert(!m_tcp_socket.is_open());
    }

    static ClientConnectionPtr create(
        asio::io_service& io_service_ref,
        udp::socket& udp_socket_ref, 
        ServerRequestHandler &request_handler_ref)
    {
        return ClientConnectionPtr(new ClientConnection(io_service_ref, udp_socket_ref, request_handler_ref));
    }

    int get_connection_id() const
    {
        return m_connection_id;
    }

    tcp::socket& get_tcp_socket()
    {
        return m_tcp_socket;
    }

    void start()
    {
        m_connection_stopped= false;

        // Send the connection ID to the client 
        // so that it can send it back to us to establish a UDP connection
        send_connection_info();

        // Wait for incoming requests from the client
        start_tcp_read_request_header();
    }

    void stop()
    {
        if (m_tcp_socket.is_open())
        {
            m_tcp_socket.shutdown(asio::socket_base::shutdown_both);

            boost::system::error_code error;
            m_tcp_socket.close(error);

            if (error)
            {
                DEBUG && (cerr << "Problem closing the tcp socket: " << error.value() << endl);
            }
        }

        m_connection_stopped= true;
        m_has_pending_tcp_write= false;
        m_has_pending_udp_write= false;
    }

    void bind_udp_remote_endpoint(const udp::endpoint &connecting_remote_endpoint)
    {
        m_udp_remote_endpoint= connecting_remote_endpoint;
    }

    bool has_pending_udp_write() const
    {
        return m_has_pending_udp_write;
    }

    bool has_queued_controller_data_frames() const
    {
        return m_pending_dataframes.size() > 0;
    }

    void add_tcp_response_to_write_queue(ResponsePtr response)
    {
        m_pending_responses.push_back(response);
    }

    bool start_tcp_write_queued_response()
    {
        bool write_in_progress= false;

        if (!m_connection_stopped)
        {
            if (!m_has_pending_tcp_write)
            {
                if (m_pending_responses.size() > 0)
                {
                    ResponsePtr response= m_pending_responses.front();

                    m_packed_response.set_msg(response);
                    m_packed_response.pack(m_response_write_buffer);

                    DEBUG && (cout << "start_tcp_write_queued_response() - Sending TCP response:\n");
                    DEBUG && (cout << "  " << show_hex(m_response_write_buffer) << endl);
                    DEBUG && (cout << m_packed_response.get_msg()->ByteSize() << " bytes" << endl);

                    // The queue should prevent us from writing more than one request as once
                    assert(!m_has_pending_tcp_write);
                    m_has_pending_tcp_write= true;
                    write_in_progress= true;

                    // Start an asynchronous operation to send a heartbeat message.
                    // NOTE: Even if the write completes immediate, the callback will only be called from io_service::poll()
                    boost::asio::async_write(
                        m_tcp_socket, 
                        boost::asio::buffer(m_response_write_buffer),
                        boost::bind(&ClientConnection::handle_write_response_complete, this, _1));
                }
            }
            else
            {
                write_in_progress= true;
            }
        }

        return write_in_progress;
    }

    void add_controller_data_frame_to_write_queue(ControllerDataFramePtr data_frame)
    {
        m_pending_dataframes.push_back(data_frame);
    }

    bool start_udp_write_queued_controller_data_frame()
    {
        bool write_in_progress= false;

        if (!m_connection_stopped)
        {
            if (!m_has_pending_udp_write)
            {
                if (m_pending_dataframes.size() > 0)
                {
                    ControllerDataFramePtr dataframe= m_pending_dataframes.front();

                    m_packed_dataframe.set_msg(dataframe);
                    if (m_packed_dataframe.pack(m_dataframe_write_buffer, sizeof(m_dataframe_write_buffer)))
                    {
                        int msg_size= m_packed_dataframe.get_msg()->ByteSize();

                        DEBUG && (cout << "start_udp_write_queued_controller_data_frame() - Sending UDP DataFrame:" << endl);
                        DEBUG && (cout << "  " << show_hex(m_dataframe_write_buffer, HEADER_SIZE+msg_size) << endl);
                        DEBUG && (cout << msg_size << " bytes" << endl);

                        // The queue should prevent us from writing more than one data frame at once
                        assert(!m_has_pending_udp_write);
                        m_has_pending_udp_write= true;
                        write_in_progress= true;

                        // Start an asynchronous operation to send the data frame
                        // NOTE: Even if the write completes immediate, the callback will only be called from io_service::poll()
                        m_udp_socket_ref.async_send_to(
                            boost::asio::buffer(m_dataframe_write_buffer, sizeof(m_dataframe_write_buffer)),
                            m_udp_remote_endpoint,
                            boost::bind(&ClientConnection::handle_udp_write_controller_data_frame_complete, this, _1));
                    }
                    else
                    {
                        DEBUG && (cout << "start_udp_write_queued_controller_data_frame() - DataFrame too big to fit in packet!" << endl);
                    }
                }
            }
            else
            {
                write_in_progress= true;
            }
        }

        return write_in_progress;
    }

private:
    static int next_connection_id;

    int m_connection_id;

    ServerRequestHandler &m_request_handler_ref;
    tcp::socket m_tcp_socket;
    udp::socket &m_udp_socket_ref;
    udp::endpoint m_udp_remote_endpoint;

    vector<uint8_t> m_request_read_buffer;
    PackedMessage<PSMoveDataFrame::Request> m_packed_request;

    vector<uint8_t> m_response_write_buffer;
    PackedMessage<PSMoveDataFrame::Response> m_packed_response;

    uint8_t m_dataframe_write_buffer[HEADER_SIZE+MAX_DATA_FRAME_MESSAGE_SIZE];
    PackedMessage<PSMoveDataFrame::ControllerDataFrame> m_packed_dataframe;

    deque<ResponsePtr> m_pending_responses;
    deque<ControllerDataFramePtr> m_pending_dataframes;
    
    bool m_connection_stopped;
    bool m_has_pending_tcp_write;
    bool m_has_pending_udp_write;

    ClientConnection(
        asio::io_service& io_service_ref,
        udp::socket& udp_socket_ref , 
        ServerRequestHandler &request_handler_ref)
        : m_connection_id(next_connection_id)
        , m_request_handler_ref(request_handler_ref)
        , m_tcp_socket(io_service_ref)
        , m_udp_socket_ref(udp_socket_ref)
        , m_udp_remote_endpoint()
        , m_request_read_buffer()
        , m_packed_request(boost::shared_ptr<PSMoveDataFrame::Request>(new PSMoveDataFrame::Request()))
        , m_response_write_buffer()
        , m_packed_response()
        , m_dataframe_write_buffer()
        , m_packed_dataframe()
        , m_pending_responses()
        , m_pending_dataframes()
        , m_connection_stopped(false)
        , m_has_pending_tcp_write(false)
        , m_has_pending_udp_write(false)
    {
        next_connection_id++;
    }

    void send_connection_info()
    {
        DEBUG && (cout << "send_connection_info() - Sending connection id to client: " << m_connection_id << endl);
        ResponsePtr response(new PSMoveDataFrame::Response);

        response->set_type(PSMoveDataFrame::Response_ResponseType_CONNECTION_INFO);
        response->set_request_id(-1); // This is a notification (no corresponding request)
        response->set_result_code(PSMoveDataFrame::Response_ResultCode_RESULT_OK);
        response->mutable_result_connection_info()->set_tcp_connection_id(m_connection_id);

        add_tcp_response_to_write_queue(response);
        start_tcp_write_queued_response();
    }

    void start_tcp_read_request_header()
    {
        m_request_read_buffer.resize(HEADER_SIZE);
        asio::async_read(
            m_tcp_socket, 
            asio::buffer(m_request_read_buffer),
            boost::bind(
                &ClientConnection::handle_tcp_read_request_header, 
                shared_from_this(),
                asio::placeholders::error));
    }

    void handle_tcp_read_request_header(const boost::system::error_code& error)
    {
        if (!error) 
        {
            DEBUG && (cout << "handle_tcp_read_request_header() - Read request header:" << endl);
            DEBUG && (cout << "  " << show_hex(m_request_read_buffer) << endl);
            unsigned msg_len = m_packed_request.decode_header(m_request_read_buffer);
            DEBUG && (cout << msg_len << "  Body Size =  bytes\n");
            start_read_body(msg_len);
        }
        else
        {
            DEBUG && (cerr << "handle_tcp_read_request_header() - Failed to read header: " << error.message() << endl);
        }
    }

    void start_read_body(unsigned msg_len)
    {
        // m_readbuf already contains the header in its first HEADER_SIZE
        // bytes. Expand it to fit in the body as well, and start async
        // read into the body.
        //
        m_request_read_buffer.resize(HEADER_SIZE + msg_len);
        asio::mutable_buffers_1 buf = asio::buffer(&m_request_read_buffer[HEADER_SIZE], msg_len);
        asio::async_read(
            m_tcp_socket, buf,
            boost::bind(
                &ClientConnection::handle_read_body, 
                shared_from_this(),
                asio::placeholders::error));
    }

    void handle_read_body(const boost::system::error_code& error)
    {
        DEBUG && (cerr << "handle body " << error << '\n');
        if (!error) 
        {
            DEBUG && (cout << "handle_tcp_read_request_header() - Read request body:" << endl);
            DEBUG && (cout << "  " << show_hex(m_request_read_buffer) << endl);
            handle_request();
            start_tcp_read_request_header();
        }
        else
        {
            DEBUG && (cout << "handle_read_body() - Failed to read body: " << error.message() << endl);
        }
    }

    // Called when enough data was read into m_readbuf for a complete request
    // message. 
    // Parse the request, execute it and send back a response.
    //
    void handle_request()
    {
        if (m_packed_request.unpack(m_request_read_buffer))
        {
            RequestPtr request = m_packed_request.get_msg();
            ResponsePtr response = m_request_handler_ref.handle_request(m_connection_id, request);
            
            add_tcp_response_to_write_queue(response);
            start_tcp_write_queued_response();
        }
    }

    void handle_write_response_complete(const boost::system::error_code& ec)
    {
        if (m_connection_stopped)
            return;

        if (!ec)
        {
            // no longer is there a pending write
            m_has_pending_tcp_write= false;

            // Remove the response from the pending send queue now that it's sent
            m_pending_responses.pop_front();

            // If there are more requests waiting to be sent, start sending the next one
            start_tcp_write_queued_response();
        }
        else
        {
            DEBUG && (cerr << "Error on request send: " << ec.message() << endl);
            stop();
        }
    }

    void handle_udp_write_controller_data_frame_complete(const boost::system::error_code& ec)
    {
        if (m_connection_stopped)
            return;

        if (!ec)
        {
            // no longer is there a pending write
            m_has_pending_udp_write= false;

            // Remove the dataframe from the pending send queue now that it's sent
            m_pending_dataframes.pop_front();
        }
        else
        {
            DEBUG && (cerr << "Error on data frame send: " << ec.message() << endl);
            stop();
        }
    }
};
int ClientConnection::next_connection_id = 0;

// -NetworkManagerImpl-
// Internal implementation of the network manager.
class ServerNetworkManagerImpl
{
public:
    ServerNetworkManagerImpl(unsigned int port, ServerRequestHandler &requestHandler)
        : request_handler_ref(requestHandler)
        , io_service()
        , tcp_acceptor(io_service, tcp::endpoint(tcp::v4(), port))
        , udp_socket(io_service, udp::endpoint(udp::v4(), port))
        , connections()
    {
    }

    virtual ~ServerNetworkManagerImpl()
    {
        // All connections should have been closed at this point
        assert(connections.empty());
    }

    void start_tcp_accept()
    {
        DEBUG && (cout << "start_tcp_accept() - Start waiting for a new TCP connection"<< endl);

        // Create a new connection to handle a client. 
        // Passing a reference to a request handler to each connection poses no problem 
        // since the server is single-threaded.
        ClientConnectionPtr new_connection = 
            ClientConnection::create(tcp_acceptor.get_io_service(), udp_socket, request_handler_ref);

        // Add the connection to the list
        t_id_client_connection_pair map_entry(new_connection->get_connection_id(), new_connection);
        connections.insert(map_entry);

        // Asynchronously wait to accept a new tcp client
        tcp_acceptor.async_accept(
            new_connection->get_tcp_socket(),
            boost::bind(&ServerNetworkManagerImpl::handle_tcp_accept, this, new_connection, asio::placeholders::error));

        // Asynchronously wait to accept a new udp clients
        // These should always come after a tcp connection is accepted
        start_udp_receive_connection_id();
    }

    void poll()
    {
        bool keep_polling= true;
        int iteration_count= 0;
        const static int k_max_iteration_count= 32;

        while (keep_polling && iteration_count < k_max_iteration_count)
        {
            // Start any pending writes on the UDP socket that can be started
            start_udp_queued_data_frame_write();

            // This call can execute any of the following callbacks:
            // * TCP request has finished reading
            // * TCP response has finished writing
            // * UDP data frame has finished writing
            io_service.poll();

            // In the event that a UDP data frame write completed immediately,
            // we should start another UDP data frame write.
            keep_polling= has_queued_controller_data_frames_ready_to_start();

            // ... but don't re-run this too many times
            ++iteration_count;
        }
    }

    void close_all_connections()
    {
        for (t_client_connection_map_iter iter= connections.begin(); iter != connections.end(); ++iter)
        {
            ClientConnectionPtr clientConnection= iter->second;

            clientConnection->stop();
        }

        connections.clear();
    }

    void send_notification(int connection_id, ResponsePtr response)
    {
        t_client_connection_map_iter entry = connections.find(connection_id);

        // Notifications have an invalid response ID
        response->set_request_id(-1);
        
        if (entry != connections.end())
        {
            ClientConnectionPtr connection= entry->second;

            connection->add_tcp_response_to_write_queue(response);
            connection->start_tcp_write_queued_response();
        }
    }

    void send_notification_to_all_clients(ResponsePtr response)
    {
        // Notifications have an invalid response ID
        response->set_request_id(-1);

        for (t_client_connection_map_iter iter= connections.begin(); iter != connections.end(); ++iter)
        {
            ClientConnectionPtr connection= iter->second;

            connection->add_tcp_response_to_write_queue(response);
            connection->start_tcp_write_queued_response();
        }
    }

    void send_controller_data_frame(int connection_id, ControllerDataFramePtr data_frame)
    {
        t_client_connection_map_iter entry = connections.find(connection_id);

        if (entry != connections.end())
        {
            ClientConnectionPtr connection= entry->second;

            connection->add_controller_data_frame_to_write_queue(data_frame);

            start_udp_queued_data_frame_write();
        }
    }

private:
    // Process and responds to incoming PSMoveService request
    ServerRequestHandler &request_handler_ref;
    
    // Core i/o functionality for TCP/UDP sockets
    asio::io_service io_service;

    // Handles waiting for and accepting new TCP connections
    tcp::acceptor tcp_acceptor;

    // UDP socket shared amongst all of the client connections
    udp::socket udp_socket;

    // The endpoint of the next connecting 
    udp::endpoint udp_connecting_remote_endpoint;

    // A pending udp request from the client
    int m_udp_connection_id_read_buffer;

    // A pending udp result sent to the client
    bool m_udp_connection_result_write_buffer;

    // A mapping from connection_id -> ClientConnectionPtr
    t_client_connection_map connections;

    void handle_tcp_accept(ClientConnectionPtr connection, const boost::system::error_code& error)
    {        
        // A new client has connected
        //
        if (!error)
        {
            DEBUG && (cout << "handle_tcp_accept() - Accepting a new connection" << endl);

            // Start the connection
            connection->start();

            // Accept another client
            start_tcp_accept();
        }
        else
        {
            DEBUG && (cout << "handle_tcp_accept() - Failed to accept new connection: " << error.message() << endl);
        }
    }

    void start_udp_receive_connection_id()
    {
        DEBUG && (cout << "start_udp_receive_connection_id() - waiting for UDP connection id" << endl);

        udp_socket.async_receive_from(
            boost::asio::buffer(&m_udp_connection_id_read_buffer, sizeof(m_udp_connection_id_read_buffer)),
            udp_connecting_remote_endpoint,
            boost::bind(&ServerNetworkManagerImpl::handle_udp_read_connection_id, this, boost::asio::placeholders::error));
    }

    void handle_udp_read_connection_id(const boost::system::error_code& error)
    {
        if (!error) 
        {
            // Find the connection with the matching id
            t_client_connection_map_iter iter= connections.find(m_udp_connection_id_read_buffer);

            if (iter != connections.end())
            {
                DEBUG && (cout << "handle_udp_read_connection_id() - Found UDP client connected with matching connection_id: " << m_udp_connection_id_read_buffer << endl);
                ClientConnectionPtr connection= iter->second;

                // Associate this udp remote endpoint with the given connection id
                connection->bind_udp_remote_endpoint(udp_connecting_remote_endpoint);

                // Tell the client that this was a valid connection id
                start_udp_send_connection_result(true);
            }
            else
            {
                DEBUG && (cerr << "Error: UDP client connected with INVALID connection_id: " << m_udp_connection_id_read_buffer << endl);

                // Tell the client that this was an invalid connection id
                start_udp_send_connection_result(false);
            }
        }
        else
        {
            DEBUG && (cerr << "handle_udp_read_connection_id() - ERROR: Failed to receive UDP connection id: " << error.message() << '\n');
        }
    }

    void start_udp_send_connection_result(bool success)
    {
        DEBUG && (cout << "start_udp_send_connection_result() - Send result: " << success << endl);
        m_udp_connection_result_write_buffer= success;
        udp_socket.async_send_to(
            boost::asio::buffer(&m_udp_connection_result_write_buffer, sizeof(m_udp_connection_result_write_buffer)), 
            udp_connecting_remote_endpoint,
            boost::bind(&ServerNetworkManagerImpl::handle_udp_write_connection_result, this, boost::asio::placeholders::error));
    }

    void handle_udp_write_connection_result(const boost::system::error_code& error)
    {
        if (error) 
        {
            DEBUG && (cerr << "handle_udp_write_connection_result() - Failed to send UDP connection response: " << error.message() << '\n');
        }

        // Start waiting for the next connection result
        start_udp_receive_connection_id();
    }

    void start_udp_queued_data_frame_write()
    {
        for (t_client_connection_map_iter iter= connections.begin(); iter != connections.end(); ++iter)
        {
            ClientConnectionPtr connection= iter->second;

            if (connection->start_udp_write_queued_controller_data_frame())
            {
                // Don't start a write on any other connection until this one is finished 
                break;
            }
        }        
    }

    bool has_queued_controller_data_frames_ready_to_start()
    {
        bool has_queued_write_ready_to_start= false;
        bool udp_socket_available= true;

        for (t_client_connection_map_iter iter= connections.begin(); iter != connections.end(); ++iter)
        {
            ClientConnectionPtr connection= iter->second;

            if (connection->has_pending_udp_write())
            {
                // Can't start any new udp write until any current udp write is done
                udp_socket_available= false;
                break;
            }

            if (connection->has_queued_controller_data_frames())
            {
                // Found a connection with a pending udp write ready to go
                has_queued_write_ready_to_start= true;
            }
        }

        return udp_socket_available && has_queued_write_ready_to_start;
    }
};

//-- public interface -----
ServerNetworkManager *ServerNetworkManager::m_instance = NULL;

ServerNetworkManager::ServerNetworkManager(unsigned port, ServerRequestHandler &requestHandler)
    : implementation_ptr(new ServerNetworkManagerImpl(port, requestHandler))
{
}

ServerNetworkManager::~ServerNetworkManager()
{
    assert(m_instance == NULL);
    delete implementation_ptr;
}

bool ServerNetworkManager::startup()
{
    m_instance= this;
    implementation_ptr->start_tcp_accept();

    return true;
}

void ServerNetworkManager::update()
{
    implementation_ptr->poll();
}

void ServerNetworkManager::shutdown()
{
    implementation_ptr->close_all_connections();
    m_instance= NULL;
}

void ServerNetworkManager::send_notification(int connection_id, ResponsePtr response)
{
    implementation_ptr->send_notification(connection_id, response);
}

void ServerNetworkManager::send_notification_to_all_clients(ResponsePtr response)
{
    implementation_ptr->send_notification_to_all_clients(response);
}

void ServerNetworkManager::send_controller_data_frame(int connection_id, ControllerDataFramePtr data_frame)
{
    implementation_ptr->send_controller_data_frame(connection_id, data_frame);
}
