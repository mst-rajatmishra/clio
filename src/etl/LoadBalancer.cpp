//------------------------------------------------------------------------------
/*
    This file is part of clio: https://github.com/XRPLF/clio
    Copyright (c) 2023, the clio developers.

    Permission to use, copy, modify, and distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL,  DIRECT,  INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <backend/DBHelpers.h>
#include <etl/ETLService.h>
#include <etl/NFTHelpers.h>
#include <etl/ProbingSource.h>
#include <etl/Source.h>
#include <log/Logger.h>
#include <rpc/RPCHelpers.h>
#include <util/Profiler.h>

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/protocol/STLedgerEntry.h>
#include <boost/asio/strand.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>
#include <boost/json/src.hpp>

#include <thread>

using namespace clio;

std::unique_ptr<Source>
LoadBalancer::make_Source(
    clio::Config const& config,
    boost::asio::io_context& ioContext,
    std::shared_ptr<BackendInterface> backend,
    std::shared_ptr<SubscriptionManager> subscriptions,
    std::shared_ptr<NetworkValidatedLedgers> networkValidatedLedgers,
    LoadBalancer& balancer)
{
    auto src =
        std::make_unique<ProbingSource>(config, ioContext, backend, subscriptions, networkValidatedLedgers, balancer);

    src->run();

    return src;
}

std::shared_ptr<LoadBalancer>
LoadBalancer::make_LoadBalancer(
    clio::Config const& config,
    boost::asio::io_context& ioc,
    std::shared_ptr<BackendInterface> backend,
    std::shared_ptr<SubscriptionManager> subscriptions,
    std::shared_ptr<NetworkValidatedLedgers> validatedLedgers)
{
    return std::make_shared<LoadBalancer>(config, ioc, backend, subscriptions, validatedLedgers);
}

LoadBalancer::LoadBalancer(
    clio::Config const& config,
    boost::asio::io_context& ioContext,
    std::shared_ptr<BackendInterface> backend,
    std::shared_ptr<SubscriptionManager> subscriptions,
    std::shared_ptr<NetworkValidatedLedgers> nwvl)
{
    if (auto value = config.maybeValue<uint32_t>("num_markers"); value)
        downloadRanges_ = std::clamp(*value, 1u, 256u);
    else if (backend->fetchLedgerRange())
        downloadRanges_ = 4;

    for (auto const& entry : config.array("etl_sources"))
    {
        std::unique_ptr<Source> source = make_Source(entry, ioContext, backend, subscriptions, nwvl, *this);

        sources_.push_back(std::move(source));
        log_.info() << "Added etl source - " << sources_.back()->toString();
    }
}

std::pair<std::vector<std::string>, bool>
LoadBalancer::loadInitialLedger(uint32_t sequence, bool cacheOnly)
{
    std::vector<std::string> response;
    auto const success = execute(
        [this, &response, &sequence, cacheOnly](auto& source) {
            auto [data, res] = source->loadInitialLedger(sequence, downloadRanges_, cacheOnly);

            if (!res)
                log_.error() << "Failed to download initial ledger."
                             << " Sequence = " << sequence << " source = " << source->toString();
            else
                response = std::move(data);

            return res;
        },
        sequence);
    return {std::move(response), success};
}

LoadBalancer::DataType
LoadBalancer::fetchLedger(uint32_t ledgerSequence, bool getObjects, bool getObjectNeighbors)
{
    RawDataType response;
    bool success = execute(
        [&response, ledgerSequence, getObjects, getObjectNeighbors, log = log_](auto& source) {
            auto [status, data] = source->fetchLedger(ledgerSequence, getObjects, getObjectNeighbors);
            response = std::move(data);
            if (status.ok() && response.validated())
            {
                log.info() << "Successfully fetched ledger = " << ledgerSequence
                           << " from source = " << source->toString();
                return true;
            }
            else
            {
                log.warn() << "Could not fetch ledger " << ledgerSequence << ", Reply: " << response.DebugString()
                           << ", error_code: " << status.error_code() << ", error_msg: " << status.error_message()
                           << ", source = " << source->toString();
                return false;
            }
        },
        ledgerSequence);
    if (success)
        return response;
    else
        return {};
}

std::optional<boost::json::object>
LoadBalancer::forwardToRippled(
    boost::json::object const& request,
    std::string const& clientIp,
    boost::asio::yield_context& yield) const
{
    srand((unsigned)time(0));
    auto sourceIdx = rand() % sources_.size();
    auto numAttempts = 0;

    while (numAttempts < sources_.size())
    {
        if (auto res = sources_[sourceIdx]->forwardToRippled(request, clientIp, yield))
            return res;

        sourceIdx = (sourceIdx + 1) % sources_.size();
        ++numAttempts;
    }

    return {};
}

bool
LoadBalancer::shouldPropagateTxnStream(Source* in) const
{
    for (auto& src : sources_)
    {
        assert(src);

        // We pick the first Source encountered that is connected
        if (src->isConnected())
            return *src == *in;
    }

    // If no sources connected, then this stream has not been forwarded
    return true;
}

boost::json::value
LoadBalancer::toJson() const
{
    boost::json::array ret;
    for (auto& src : sources_)
        ret.push_back(src->toJson());

    return ret;
}

template <class Func>
bool
LoadBalancer::execute(Func f, uint32_t ledgerSequence)
{
    srand((unsigned)time(0));
    auto sourceIdx = rand() % sources_.size();
    auto numAttempts = 0;

    while (true)
    {
        auto& source = sources_[sourceIdx];

        log_.debug() << "Attempting to execute func. ledger sequence = " << ledgerSequence
                     << " - source = " << source->toString();
        // Originally, it was (source->hasLedger(ledgerSequence) || true)
        /* Sometimes rippled has ledger but doesn't actually know. However,
        but this does NOT happen in the normal case and is safe to remove
        This || true is only needed when loading full history standalone */
        if (source->hasLedger(ledgerSequence))
        {
            bool res = f(source);
            if (res)
            {
                log_.debug() << "Successfully executed func at source = " << source->toString()
                             << " - ledger sequence = " << ledgerSequence;
                break;
            }
            else
            {
                log_.warn() << "Failed to execute func at source = " << source->toString()
                            << " - ledger sequence = " << ledgerSequence;
            }
        }
        else
        {
            log_.warn() << "Ledger not present at source = " << source->toString()
                        << " - ledger sequence = " << ledgerSequence;
        }
        sourceIdx = (sourceIdx + 1) % sources_.size();
        numAttempts++;
        if (numAttempts % sources_.size() == 0)
        {
            log_.info() << "Ledger sequence " << ledgerSequence << " is not yet available from any configured sources. "
                        << "Sleeping and trying again";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    return true;
}
