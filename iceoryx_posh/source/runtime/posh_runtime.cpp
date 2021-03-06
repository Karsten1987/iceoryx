// Copyright (c) 2019 by Robert Bosch GmbH. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "iceoryx_posh/runtime/posh_runtime.hpp"

#include "iceoryx_posh/iceoryx_posh_types.hpp"
#include "iceoryx_posh/internal/log/posh_logging.hpp"
#include "iceoryx_posh/internal/runtime/message_queue_message.hpp"
#include "iceoryx_posh/runtime/runnable.hpp"
#include "iceoryx_utils/posix_wrapper/timer.hpp"

#include <cstdint>

namespace iox
{
namespace runtime
{
std::function<PoshRuntime&(const std::string& name)> PoshRuntime::s_runtimeFactory =
    PoshRuntime::defaultRuntimeFactory;


PoshRuntime& PoshRuntime::defaultRuntimeFactory(const std::string& name) noexcept
{
    static PoshRuntime instance(name);
    return instance;
}

// singleton access
PoshRuntime& PoshRuntime::getInstance(const std::string& name) noexcept
{
    return PoshRuntime::s_runtimeFactory(name);
}

PoshRuntime::PoshRuntime(const std::string& name, const bool doMapSharedMemoryIntoThread) noexcept
    : m_appName(verifyInstanceName(name))
    , m_kaThread_run(true)
    , m_MqInterface(MQ_ROUDI_NAME, name, PROCESS_WAITING_FOR_ROUDI_TIMEOUT)
    , m_ShmInterface(m_MqInterface.getShmBaseAddr(),
                     doMapSharedMemoryIntoThread,
                     m_MqInterface.getShmTopicSize(),
                     m_MqInterface.getSegmentManagerAddr())
    , m_applicationPort(getMiddlewareApplication(Interfaces::INTERNAL))
{
    // start thread after shm-interface was initialized & 'this'-pointer is finally valid
    // if started earlier we run into alive-messages at the roudi before the app is connected :-(
    m_kaThread = std::thread(&PoshRuntime::threadKeepaliveMain, this);
    // set thread name
    pthread_setname_np(m_kaThread.native_handle(), "KeepAlive");

    /// @todo here we could get the LogLevel and LogMode and set it on the LogManager
}

PoshRuntime::~PoshRuntime() noexcept
{
    m_kaThread_run = false;
    if (m_kaThread.joinable())
    {
        m_kaThread.join();
    }
}

const std::string& PoshRuntime::verifyInstanceName(const std::string& name) noexcept
{
    if (name.empty())
    {
        LogError() << "Cannot initialize runtime. Application name must not be empty!";
        std::terminate();
    }
    else if (name.compare(DEFAULT_RUNTIME_INSTANCE_NAME) == 0)
    {
        LogError() << "Cannot initialize runtime. Application name has not been specified!";
        std::terminate();
    }
    else if (name.front() != '/')
    {
        LogError() << "Cannot initialize runtime. Application name " << name
                   << "does not have the required leading slash '/'";
        std::terminate();
    }

    return name;
}

std::string PoshRuntime::getInstanceName() const noexcept
{
    return m_appName;
}

const std::atomic<uint64_t>* PoshRuntime::getServiceRegistryChangeCounter() noexcept
{
    MqMessage sendBuffer;
    sendBuffer << mqMessageTypeToString(MqMessageType::SERVICE_REGISTRY_CHANGE_COUNTER) << m_appName;
    MqMessage receiveBuffer;
    if (sendRequestToRouDi(sendBuffer, receiveBuffer) && (1 == receiveBuffer.getNumberOfElements()))
    {
        const std::atomic<uint64_t>* counter = const_cast<const std::atomic<uint64_t>*>(
            reinterpret_cast<std::atomic<uint64_t>*>(std::stoul(receiveBuffer.getElementAtIndex(0))));
        return counter;
    }
    else
    {
        LogError() << "unable to request service registry change counter caused by wrong response from roudi :\""
                   << sendBuffer.getMessage() << "\"";
        return nullptr;
    }
}

SenderPortType::MemberType_t* PoshRuntime::getMiddlewareSender(const capro::ServiceDescription& service,
                                                                   const Interfaces interface,
                                                                   const cxx::CString100& runnableName) noexcept
{
    MqMessage sendBuffer;
    sendBuffer << mqMessageTypeToString(MqMessageType::IMPL_SENDER) << m_appName
               << static_cast<cxx::Serialization>(service).toString() << static_cast<uint32_t>(interface)
               << runnableName;

    return requestSenderFromRoudi(sendBuffer);
}

SenderPortType::MemberType_t* PoshRuntime::requestSenderFromRoudi(const MqMessage& sendBuffer) noexcept
{
    MqMessage receiveBuffer;
    if (sendRequestToRouDi(sendBuffer, receiveBuffer) && (2 == receiveBuffer.getNumberOfElements()))
    {
        std::string mqMessage = receiveBuffer.getElementAtIndex(0);

        if (stringToMqMessageType(mqMessage.c_str()) == MqMessageType::IMPL_SENDER_ACK)

        {
            SenderPortType::MemberType_t* sender =
                reinterpret_cast<SenderPortType::MemberType_t*>(std::stoll(receiveBuffer.getElementAtIndex(1)));
            return sender;
        }
        else
        {
            LogError() << "Wrong response from message queue" << mqMessage;
            assert(false);
            return nullptr;
        }
    }
    else
    {
        LogError() << "Wrong response from message queue";
        assert(false);
        return nullptr;
    }
}

ReceiverPortType::MemberType_t* PoshRuntime::getMiddlewareReceiver(const capro::ServiceDescription& service,
                                                                       const Interfaces interface,
                                                                       const cxx::CString100& runnableName) noexcept
{
    MqMessage sendBuffer;
    sendBuffer << mqMessageTypeToString(MqMessageType::IMPL_RECEIVER) << m_appName
               << static_cast<cxx::Serialization>(service).toString() << static_cast<uint32_t>(interface)
               << runnableName;

    return requestReceiverFromRoudi(sendBuffer);
}

ReceiverPortType::MemberType_t* PoshRuntime::requestReceiverFromRoudi(const MqMessage& sendBuffer) noexcept
{
    MqMessage receiveBuffer;
    if (sendRequestToRouDi(sendBuffer, receiveBuffer) && (2 == receiveBuffer.getNumberOfElements()))
    {
        std::string mqMessage = receiveBuffer.getElementAtIndex(0);

        if (stringToMqMessageType(mqMessage.c_str()) == MqMessageType::IMPL_RECEIVER_ACK)
        {
            ReceiverPortType::MemberType_t* receiver =
                reinterpret_cast<ReceiverPortType::MemberType_t*>(std::stoll(receiveBuffer.getElementAtIndex(1)));
            return receiver;
        }
        else
        {
            LogError() << "Wrong response from message queue " << mqMessage;
            assert(false);
            return nullptr;
        }
    }
    else
    {
        LogError() << "Wrong response from message queue";
        assert(false);
        return nullptr;
    }
}

popo::InterfacePortData* PoshRuntime::getMiddlewareInterface(const Interfaces interface,
                                                                 const cxx::CString100& runnableName) noexcept
{
    MqMessage sendBuffer;
    sendBuffer << mqMessageTypeToString(MqMessageType::IMPL_INTERFACE) << m_appName << static_cast<uint32_t>(interface)
               << runnableName;

    MqMessage receiveBuffer;

    if (sendRequestToRouDi(sendBuffer, receiveBuffer) && (2 == receiveBuffer.getNumberOfElements()))
    {
        std::string mqMessage = receiveBuffer.getElementAtIndex(0);

        if (stringToMqMessageType(mqMessage.c_str()) == MqMessageType::IMPL_INTERFACE_ACK)
        {
            popo::InterfacePortData* port =
                reinterpret_cast<popo::InterfacePortData*>(std::stoll(receiveBuffer.getElementAtIndex(1)));
            return port;
        }
        else
        {
            LogError() << "Wrong response from message queue " << mqMessage;
            assert(false);
            return nullptr;
        }
    }
    else
    {
        LogError() << "Wrong response from message queue";
        assert(false);
        return nullptr;
    }
}

RunnableData* PoshRuntime::createRunnable(const RunnableProperty& runnableProperty) noexcept
{
    MqMessage sendBuffer;
    sendBuffer << mqMessageTypeToString(MqMessageType::CREATE_RUNNABLE) << m_appName
               << static_cast<std::string>(runnableProperty);

    MqMessage receiveBuffer;

    if (sendRequestToRouDi(sendBuffer, receiveBuffer) && (2 == receiveBuffer.getNumberOfElements()))
    {
        std::string mqMessage = receiveBuffer.getElementAtIndex(0);

        if (stringToMqMessageType(mqMessage.c_str()) == MqMessageType::CREATE_RUNNABLE_ACK)
        {
            RunnableData* port = reinterpret_cast<RunnableData*>(std::stoll(receiveBuffer.getElementAtIndex(1)));
            return port;
        }
        else
        {
            LogError() << "Wrong response from message queue " << mqMessage;
            assert(false);
            return nullptr;
        }
    }
    else
    {
        LogError() << "Wrong response from message queue";
        assert(false);
        return nullptr;
    }
}

void PoshRuntime::removeRunnable(const Runnable& runnable) noexcept
{
    LogError() << "removeRunnable not yet supported";
    assert(false);

    MqMessage sendBuffer;
    sendBuffer << mqMessageTypeToString(MqMessageType::REMOVE_RUNNABLE) << runnable.getRunnableName();

    if (!sendMessageToRouDi(sendBuffer))
    {
        LogError() << "unable to send runnable removal request to roudi";
        assert(false);
    }
}

void PoshRuntime::findService(const capro::ServiceDescription& serviceDescription,
                                  InstanceContainer& instanceContainer) noexcept
{
    MqMessage sendBuffer;
    sendBuffer << mqMessageTypeToString(MqMessageType::FIND_SERVICE) << m_appName
               << static_cast<cxx::Serialization>(serviceDescription).toString();

    MqMessage requestResponse;

    if (!sendRequestToRouDi(sendBuffer, requestResponse))
    {
        LogError() << "Could not send FIND_SERVICE request to RouDi\n";
    }
    else
    {
        uint32_t numberOfElements = requestResponse.getNumberOfElements();
        for (uint32_t i = 0; i < numberOfElements; ++i)
        {
            IdString instance(requestResponse.getElementAtIndex(i).c_str());
            instanceContainer.push_back(instance);
        }
    }
}

void PoshRuntime::offerService(const capro::ServiceDescription& serviceDescription) noexcept
{
    capro::CaproMessage msg(capro::CaproMessageType::OFFER, serviceDescription, capro::CaproMessageSubType::SERVICE);
    m_applicationPort.dispatchCaProMessage(msg);
}

void PoshRuntime::stopOfferService(const capro::ServiceDescription& serviceDescription) noexcept
{
    capro::CaproMessage msg(
        capro::CaproMessageType::STOP_OFFER, serviceDescription, capro::CaproMessageSubType::SERVICE);
    m_applicationPort.dispatchCaProMessage(msg);
}

popo::ApplicationPortData* PoshRuntime::getMiddlewareApplication(Interfaces interface) noexcept
{
    MqMessage sendBuffer;
    sendBuffer << mqMessageTypeToString(MqMessageType::IMPL_APPLICATION) << m_appName
               << static_cast<uint32_t>(interface);

    MqMessage receiveBuffer;

    if (sendRequestToRouDi(sendBuffer, receiveBuffer) && (2 == receiveBuffer.getNumberOfElements()))
    {
        std::string mqMessage = receiveBuffer.getElementAtIndex(0);

        if (stringToMqMessageType(mqMessage.c_str()) == MqMessageType::IMPL_APPLICATION_ACK)
        {
            popo::ApplicationPortData* port =
                reinterpret_cast<popo::ApplicationPortData*>(std::stoll(receiveBuffer.getElementAtIndex(1)));
            return port;
        }
        else
        {
            LogError() << "Wrong response from message queue" << mqMessage;
            assert(false);
            return nullptr;
        }
    }
    else
    {
        LogError() << "Wrong response from message queue";
        assert(false);
        return nullptr;
    }
}

bool PoshRuntime::sendRequestToRouDi(const MqMessage& msg, MqMessage& answer) noexcept
{
    // runtime must be thread safe
    std::lock_guard<std::mutex> g(m_appMqRequestMutex);
    return m_MqInterface.sendRequestToRouDi(msg, answer);
}

bool PoshRuntime::sendMessageToRouDi(const MqMessage& msg) noexcept
{
    // runtime must be thread safe
    std::lock_guard<std::mutex> g(m_appMqRequestMutex);
    return m_MqInterface.sendMessageToRouDi(msg);
}

void PoshRuntime::threadKeepaliveMain() noexcept
{
    static_assert(PROCESS_KEEP_ALIVE_INTERVAL > DISCOVERY_INTERVAL, "Keep alive interval too small");

    posix::Timer osTimer(PROCESS_KEEP_ALIVE_INTERVAL, [&]() {
        if (!m_MqInterface.sendKeepalive())
        {
            LogWarn() << "Error in sending keep alive";
        }
    });

    // Start the timer periodically
    osTimer.start(true);

    while (m_kaThread_run)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(DISCOVERY_INTERVAL.milliSeconds<int64_t>()));
    }
} // namespace runtime

} // namespace runtime
} // namespace iox
