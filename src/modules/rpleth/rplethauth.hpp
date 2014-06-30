/**
 * \file rplethauth.hpp
 * \author Thibault Schueller <ryp.sqrt@gmail.com>
 * \brief rpleth compatibility module
 */

#ifndef RPLETHAUTH_HPP
#define RPLETHAUTH_HPP

#include <list>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>

#include "modules/iauthmodule.hpp"
#include "network/isocket.hpp"
#include "network/circularbuffer.hpp"

class RplethAuth : public IAuthModule
{
    static const Rezzo::ISocket::Port   DefaultPort = 9559;
    static const long                   DefaultTimeoutMs = 500;
    static const std::size_t            RingBufferSize = 512;

    typedef struct s_client {
        Rezzo::ISocket* socket;
        CircularBuffer  buffer;
        explicit s_client(Rezzo::ISocket* sock) : socket(sock), buffer(RingBufferSize) {}
    } Client;

public:
    typedef std::vector<Byte> CardId;

public:
    explicit RplethAuth(ICore& core, const std::string& name);
    ~RplethAuth() = default;

    RplethAuth(const RplethAuth& other) = delete;
    RplethAuth& operator=(const RplethAuth& other) = delete;

public:
    virtual const std::string&  getName() const override;
    virtual ModuleType          getType() const override;
    virtual void                serialize(ptree& node) override;
    virtual void                deserialize(const ptree& node) override;
    virtual void                authenticate(const AuthRequest& ar) override;

private:
    void    run();
    void    buildSelectParams();
    void    handleClientMessage(Client& client);
    void    handleCardIdQueue();

private:
    ICore&                  _core;
    const std::string       _name;
    std::atomic<bool>       _isRunning;
    std::thread             _networkThread;
    Rezzo::ISocket*         _serverSocket;
    std::queue<CardId>      _cardIdQueue;
    std::mutex              _cardIdQueueMutex;
    Rezzo::ISocket::Port    _port;
    std::list<Client>       _clients;
    fd_set                  _rSet;
    int                     _fdMax;
    long                    _timeout;
    struct timeval          _timeoutStruct;
    Byte                    _buffer[RingBufferSize];
};

#endif // RPLETHAUTH_HPP