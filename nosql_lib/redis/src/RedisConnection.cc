/**
 *
 *  @file RedisConnection.cc
 *  @author An Tao
 *
 *  Copyright 2018, An Tao.  All rights reserved.
 *  https://github.com/an-tao/drogon
 *  Use of this source code is governed by a MIT license
 *  that can be found in the License file.
 *
 *  Drogon
 *
 */

#include "RedisConnection.h"
#include "RedisResultImpl.h"
#include <future>

using namespace drogon::nosql;
RedisConnection::RedisConnection(const trantor::InetAddress &serverAddress,
                                 const std::string &password,
                                 trantor::EventLoop *loop)
    : serverAddr_(serverAddress), password_(password), loop_(loop)
{
    assert(loop_);
    loop_->queueInLoop([this]() { startConnectionInLoop(); });
}

void RedisConnection::startConnectionInLoop()
{
    loop_->assertInLoopThread();
    assert(!redisContext_);

    redisContext_ =
        ::redisAsyncConnect(serverAddr_.toIp().c_str(), serverAddr_.toPort());
    redisContext_->ev.addWrite = addWrite;
    redisContext_->ev.delWrite = delWrite;
    redisContext_->ev.addRead = addRead;
    redisContext_->ev.delRead = delRead;
    redisContext_->ev.cleanup = cleanup;
    redisContext_->ev.data = this;

    channel_ = std::make_unique<trantor::Channel>(loop_, redisContext_->c.fd);
    channel_->setReadCallback([this]() { handleRedisRead(); });
    channel_->setWriteCallback([this]() { handleRedisWrite(); });
    redisAsyncSetConnectCallback(
        redisContext_, [](const redisAsyncContext *context, int status) {
            auto thisPtr = static_cast<RedisConnection *>(context->ev.data);
            if (status != REDIS_OK)
            {
                LOG_ERROR << "Failed to connect to "
                          << thisPtr->serverAddr_.toIpPort() << "! "
                          << context->errstr;
                thisPtr->handleDisconnect();
                if (thisPtr->disconnectCallback_)
                {
                    thisPtr->disconnectCallback_(thisPtr->shared_from_this(),
                                                 status);
                }
            }
            else
            {
                LOG_TRACE << "Connected successfully to "
                          << thisPtr->serverAddr_.toIpPort();
                thisPtr->connected_ = ConnectStatus::kConnected;
                if (thisPtr->connectCallback_)
                {
                    thisPtr->connectCallback_(thisPtr->shared_from_this(),
                                              status);
                }
            }
        });
    redisAsyncSetDisconnectCallback(
        redisContext_, [](const redisAsyncContext *context, int status) {
            auto thisPtr = static_cast<RedisConnection *>(context->ev.data);
            thisPtr->handleDisconnect();
            if (thisPtr->disconnectCallback_)
            {
                thisPtr->disconnectCallback_(thisPtr->shared_from_this(),
                                             status);
            }

            LOG_TRACE << "Disconnected from "
                      << thisPtr->serverAddr_.toIpPort();
        });
}

void RedisConnection::handleDisconnect()
{
    connected_ = ConnectStatus::kEnd;
    if (channel_)
    {
        channel_->disableAll();
        channel_->remove();
        channel_.reset();
    }
}
void RedisConnection::addWrite(void *userData)
{
    auto thisPtr = static_cast<RedisConnection *>(userData);
    assert(thisPtr->channel_);
    thisPtr->channel_->enableWriting();
}
void RedisConnection::delWrite(void *userData)
{
    auto thisPtr = static_cast<RedisConnection *>(userData);
    assert(thisPtr->channel_);
    thisPtr->channel_->disableWriting();
}
void RedisConnection::addRead(void *userData)
{
    auto thisPtr = static_cast<RedisConnection *>(userData);
    assert(thisPtr->channel_);
    thisPtr->channel_->enableReading();
}
void RedisConnection::delRead(void *userData)
{
    auto thisPtr = static_cast<RedisConnection *>(userData);
    assert(thisPtr->channel_);
    thisPtr->channel_->disableReading();
}
void RedisConnection::cleanup(void *userData)
{
    LOG_TRACE << "cleanup";
}

void RedisConnection::handleRedisRead()
{
    redisAsyncHandleRead(redisContext_);
}
void RedisConnection::handleRedisWrite()
{
    if (redisContext_->c.flags == REDIS_DISCONNECTING)
    {
        channel_->disableAll();
        channel_->remove();
    }
    redisAsyncHandleWrite(redisContext_);
}

void RedisConnection::sendCommandInloop(
    const std::string &command,
    std::function<void(const RedisResult &)> &&callback,
    std::function<void(const std::exception &)> &&exceptionCallback)
{
    commandCallbacks_.emplace(std::move(callback));
    exceptionCallbacks_.emplace(std::move(exceptionCallback));
    command_ = command;

    redisAsyncFormattedCommand(
        redisContext_,
        [](redisAsyncContext *context, void *r, void *userData) {
            auto thisPtr = static_cast<RedisConnection *>(context->ev.data);
            thisPtr->handleResult(static_cast<redisReply *>(r));
        },
        nullptr,
        command.c_str(),
        command.length());
}

void RedisConnection::handleResult(redisReply *result)
{
    auto commandCallback = std::move(commandCallbacks_.front());
    commandCallbacks_.pop();
    auto exceptionCallback = std::move(exceptionCallbacks_.front());
    exceptionCallbacks_.pop();
    if (result->type != REDIS_REPLY_ERROR)
    {
        commandCallback(RedisResultImpl(result));
    }
    else
    {
        exceptionCallback(std::runtime_error(result->str));
    }
}

void RedisConnection::disconnect()
{
    std::promise<int> pro;
    auto f = pro.get_future();
    auto thisPtr = shared_from_this();
    loop_->runInLoop([thisPtr, &pro]() {
        redisAsyncDisconnect(thisPtr->redisContext_);
        pro.set_value(1);
    });
    f.get();
}
