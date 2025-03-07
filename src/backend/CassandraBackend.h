#ifndef RIPPLE_APP_REPORTING_CASSANDRABACKEND_H_INCLUDED
#define RIPPLE_APP_REPORTING_CASSANDRABACKEND_H_INCLUDED

#include <ripple/basics/base_uint.h>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/json.hpp>
#include <boost/log/trivial.hpp>
#include <atomic>
#include <backend/BackendInterface.h>
#include <backend/DBHelpers.h>
#include <cassandra.h>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace Backend {

class CassandraPreparedStatement
{
private:
    CassPrepared const* prepared_ = nullptr;

public:
    CassPrepared const*
    get() const
    {
        return prepared_;
    }

    bool
    prepareStatement(std::stringstream const& query, CassSession* session)
    {
        return prepareStatement(query.str().c_str(), session);
    }

    bool
    prepareStatement(std::string const& query, CassSession* session)
    {
        return prepareStatement(query.c_str(), session);
    }

    bool
    prepareStatement(char const* query, CassSession* session)
    {
        if (!query)
            throw std::runtime_error("prepareStatement: null query");
        if (!session)
            throw std::runtime_error("prepareStatement: null sesssion");
        CassFuture* prepareFuture = cass_session_prepare(session, query);
        /* Wait for the statement to prepare and get the result */
        CassError rc = cass_future_error_code(prepareFuture);
        if (rc == CASS_OK)
        {
            prepared_ = cass_future_get_prepared(prepareFuture);
        }
        else
        {
            std::stringstream ss;
            ss << "nodestore: Error preparing statement : " << rc << ", "
               << cass_error_desc(rc) << ". query : " << query;
            BOOST_LOG_TRIVIAL(error) << ss.str();
        }
        cass_future_free(prepareFuture);
        return rc == CASS_OK;
    }

    ~CassandraPreparedStatement()
    {
        BOOST_LOG_TRIVIAL(trace) << __func__;
        if (prepared_)
        {
            cass_prepared_free(prepared_);
            prepared_ = nullptr;
        }
    }
};

class CassandraStatement
{
    CassStatement* statement_ = nullptr;
    size_t curBindingIndex_ = 0;

public:
    CassandraStatement(CassandraPreparedStatement const& prepared)
    {
        statement_ = cass_prepared_bind(prepared.get());
        cass_statement_set_consistency(statement_, CASS_CONSISTENCY_QUORUM);
    }

    CassandraStatement(CassandraStatement&& other)
    {
        statement_ = other.statement_;
        other.statement_ = nullptr;
        curBindingIndex_ = other.curBindingIndex_;
        other.curBindingIndex_ = 0;
    }
    CassandraStatement(CassandraStatement const& other) = delete;

    CassStatement*
    get() const
    {
        return statement_;
    }

    void
    bindNextBoolean(bool val)
    {
        if (!statement_)
            throw std::runtime_error(
                "CassandraStatement::bindNextBoolean - statement_ is null");
        CassError rc = cass_statement_bind_bool(
            statement_, 1, static_cast<cass_bool_t>(val));
        if (rc != CASS_OK)
        {
            std::stringstream ss;
            ss << "Error binding boolean to statement: " << rc << ", "
               << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << __func__ << " : " << ss.str();
            throw std::runtime_error(ss.str());
        }
        curBindingIndex_++;
    }

    void
    bindNextBytes(const char* data, uint32_t size)
    {
        bindNextBytes((unsigned char*)data, size);
    }

    void
    bindNextBytes(ripple::uint256 const& data)
    {
        bindNextBytes(data.data(), data.size());
    }
    void
    bindNextBytes(std::vector<unsigned char> const& data)
    {
        bindNextBytes(data.data(), data.size());
    }
    void
    bindNextBytes(ripple::AccountID const& data)
    {
        bindNextBytes(data.data(), data.size());
    }

    void
    bindNextBytes(std::string const& data)
    {
        bindNextBytes(data.data(), data.size());
    }

    void
    bindNextBytes(void const* key, uint32_t size)
    {
        bindNextBytes(static_cast<const unsigned char*>(key), size);
    }

    void
    bindNextBytes(const unsigned char* data, uint32_t size)
    {
        if (!statement_)
            throw std::runtime_error(
                "CassandraStatement::bindNextBytes - statement_ is null");
        CassError rc = cass_statement_bind_bytes(
            statement_,
            curBindingIndex_,
            static_cast<cass_byte_t const*>(data),
            size);
        if (rc != CASS_OK)
        {
            std::stringstream ss;
            ss << "Error binding bytes to statement: " << rc << ", "
               << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << __func__ << " : " << ss.str();
            throw std::runtime_error(ss.str());
        }
        curBindingIndex_++;
    }

    void
    bindNextUInt(uint32_t value)
    {
        if (!statement_)
            throw std::runtime_error(
                "CassandraStatement::bindNextUInt - statement_ is null");
        BOOST_LOG_TRIVIAL(trace)
            << std::to_string(curBindingIndex_) << " " << std::to_string(value);
        CassError rc =
            cass_statement_bind_int32(statement_, curBindingIndex_, value);
        if (rc != CASS_OK)
        {
            std::stringstream ss;
            ss << "Error binding uint to statement: " << rc << ", "
               << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << __func__ << " : " << ss.str();
            throw std::runtime_error(ss.str());
        }
        curBindingIndex_++;
    }

    void
    bindNextInt(uint32_t value)
    {
        bindNextInt((int64_t)value);
    }

    void
    bindNextInt(int64_t value)
    {
        if (!statement_)
            throw std::runtime_error(
                "CassandraStatement::bindNextInt - statement_ is null");
        CassError rc =
            cass_statement_bind_int64(statement_, curBindingIndex_, value);
        if (rc != CASS_OK)
        {
            std::stringstream ss;
            ss << "Error binding int to statement: " << rc << ", "
               << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << __func__ << " : " << ss.str();
            throw std::runtime_error(ss.str());
        }
        curBindingIndex_++;
    }

    void
    bindNextIntTuple(uint32_t first, uint32_t second)
    {
        CassTuple* tuple = cass_tuple_new(2);
        CassError rc = cass_tuple_set_int64(tuple, 0, first);
        if (rc != CASS_OK)
        {
            std::stringstream ss;
            ss << "Error binding int to tuple: " << rc << ", "
               << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << __func__ << " : " << ss.str();
            throw std::runtime_error(ss.str());
        }
        rc = cass_tuple_set_int64(tuple, 1, second);
        if (rc != CASS_OK)
        {
            std::stringstream ss;
            ss << "Error binding int to tuple: " << rc << ", "
               << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << __func__ << " : " << ss.str();
            throw std::runtime_error(ss.str());
        }
        rc = cass_statement_bind_tuple(statement_, curBindingIndex_, tuple);
        if (rc != CASS_OK)
        {
            std::stringstream ss;
            ss << "Error binding tuple to statement: " << rc << ", "
               << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << __func__ << " : " << ss.str();
            throw std::runtime_error(ss.str());
        }
        cass_tuple_free(tuple);
        curBindingIndex_++;
    }

    ~CassandraStatement()
    {
        if (statement_)
            cass_statement_free(statement_);
    }
};

class CassandraResult
{
    CassResult const* result_ = nullptr;
    CassRow const* row_ = nullptr;
    CassIterator* iter_ = nullptr;
    size_t curGetIndex_ = 0;

public:
    CassandraResult() : result_(nullptr), row_(nullptr), iter_(nullptr)
    {
    }

    CassandraResult&
    operator=(CassandraResult&& other)
    {
        result_ = other.result_;
        row_ = other.row_;
        iter_ = other.iter_;
        curGetIndex_ = other.curGetIndex_;
        other.result_ = nullptr;
        other.row_ = nullptr;
        other.iter_ = nullptr;
        other.curGetIndex_ = 0;
        return *this;
    }

    CassandraResult(CassandraResult const& other) = delete;
    CassandraResult&
    operator=(CassandraResult const& other) = delete;

    CassandraResult(CassResult const* result) : result_(result)
    {
        if (!result_)
            throw std::runtime_error("CassandraResult - result is null");
        iter_ = cass_iterator_from_result(result_);
        if (cass_iterator_next(iter_))
        {
            row_ = cass_iterator_get_row(iter_);
        }
    }

    bool
    isOk()
    {
        return result_ != nullptr;
    }

    bool
    hasResult()
    {
        return row_ != nullptr;
    }

    bool
    operator!()
    {
        return !hasResult();
    }

    size_t
    numRows()
    {
        return cass_result_row_count(result_);
    }

    bool
    nextRow()
    {
        curGetIndex_ = 0;
        if (cass_iterator_next(iter_))
        {
            row_ = cass_iterator_get_row(iter_);
            return true;
        }
        row_ = nullptr;
        return false;
    }

    std::vector<unsigned char>
    getBytes()
    {
        if (!row_)
            throw std::runtime_error("CassandraResult::getBytes - no result");
        cass_byte_t const* buf;
        std::size_t bufSize;
        CassError rc = cass_value_get_bytes(
            cass_row_get_column(row_, curGetIndex_), &buf, &bufSize);
        if (rc != CASS_OK)
        {
            std::stringstream msg;
            msg << "CassandraResult::getBytes - error getting value: " << rc
                << ", " << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << msg.str();
            throw std::runtime_error(msg.str());
        }
        curGetIndex_++;
        return {buf, buf + bufSize};
    }
    /*
    uint32_t
    getNumBytes()
    {
        if (!row_)
            throw std::runtime_error("CassandraResult::getBytes - no result");
        cass_byte_t const* buf;
        std::size_t bufSize;
        CassError rc = cass_value_get_bytes(
            cass_row_get_column(row_, curGetIndex_), &buf, &bufSize);
        if (rc != CASS_OK)
        {
            std::stringstream msg;
            msg << "CassandraResult::getBytes - error getting value: " << rc
                << ", " << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << msg.str();
            throw std::runtime_error(msg.str());
        }
        return bufSize;
    }*/

    ripple::uint256
    getUInt256()
    {
        if (!row_)
            throw std::runtime_error("CassandraResult::uint256 - no result");
        cass_byte_t const* buf;
        std::size_t bufSize;
        CassError rc = cass_value_get_bytes(
            cass_row_get_column(row_, curGetIndex_), &buf, &bufSize);
        if (rc != CASS_OK)
        {
            std::stringstream msg;
            msg << "CassandraResult::getuint256 - error getting value: " << rc
                << ", " << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << msg.str();
            throw std::runtime_error(msg.str());
        }
        curGetIndex_++;
        return ripple::uint256::fromVoid(buf);
    }

    int64_t
    getInt64()
    {
        if (!row_)
            throw std::runtime_error("CassandraResult::getInt64 - no result");
        cass_int64_t val;
        CassError rc =
            cass_value_get_int64(cass_row_get_column(row_, curGetIndex_), &val);
        if (rc != CASS_OK)
        {
            std::stringstream msg;
            msg << "CassandraResult::getInt64 - error getting value: " << rc
                << ", " << cass_error_desc(rc);
            BOOST_LOG_TRIVIAL(error) << msg.str();
            throw std::runtime_error(msg.str());
        }
        ++curGetIndex_;
        return val;
    }

    uint32_t
    getUInt32()
    {
        return (uint32_t)getInt64();
    }

    std::pair<int64_t, int64_t>
    getInt64Tuple()
    {
        if (!row_)
            throw std::runtime_error(
                "CassandraResult::getInt64Tuple - no result");
        CassValue const* tuple = cass_row_get_column(row_, curGetIndex_);
        CassIterator* tupleIter = cass_iterator_from_tuple(tuple);
        if (!cass_iterator_next(tupleIter))
            throw std::runtime_error(
                "CassandraResult::getInt64Tuple - failed to iterate tuple");
        CassValue const* value = cass_iterator_get_value(tupleIter);
        int64_t first;
        cass_value_get_int64(value, &first);
        if (!cass_iterator_next(tupleIter))
            throw std::runtime_error(
                "CassandraResult::getInt64Tuple - failed to iterate tuple");
        value = cass_iterator_get_value(tupleIter);
        int64_t second;
        cass_value_get_int64(value, &second);
        ++curGetIndex_;
        return {first, second};
    }

    std::pair<Blob, Blob>
    getBytesTuple()
    {
        cass_byte_t const* buf;
        std::size_t bufSize;

        if (!row_)
            throw std::runtime_error(
                "CassandraResult::getBytesTuple - no result");
        CassValue const* tuple = cass_row_get_column(row_, curGetIndex_);
        CassIterator* tupleIter = cass_iterator_from_tuple(tuple);
        if (!cass_iterator_next(tupleIter))
            throw std::runtime_error(
                "CassandraResult::getBytesTuple - failed to iterate tuple");
        CassValue const* value = cass_iterator_get_value(tupleIter);
        cass_value_get_bytes(value, &buf, &bufSize);
        Blob first{buf, buf + bufSize};

        if (!cass_iterator_next(tupleIter))
            throw std::runtime_error(
                "CassandraResult::getBytesTuple - failed to iterate tuple");
        value = cass_iterator_get_value(tupleIter);
        cass_value_get_bytes(value, &buf, &bufSize);
        Blob second{buf, buf + bufSize};
        ++curGetIndex_;
        return {first, second};
    }

    ~CassandraResult()
    {
        if (result_ != nullptr)
            cass_result_free(result_);
        if (iter_ != nullptr)
            cass_iterator_free(iter_);
    }
};
inline bool
isTimeout(CassError rc)
{
    if (rc == CASS_ERROR_LIB_NO_HOSTS_AVAILABLE or
        rc == CASS_ERROR_LIB_REQUEST_TIMED_OUT or
        rc == CASS_ERROR_SERVER_UNAVAILABLE or
        rc == CASS_ERROR_SERVER_OVERLOADED or
        rc == CASS_ERROR_SERVER_READ_TIMEOUT)
        return true;
    return false;
}

class CassandraBackend : public BackendInterface
{
private:
    // convenience function for one-off queries. For normal reads and writes,
    // use the prepared statements insert_ and select_
    CassStatement*
    makeStatement(char const* query, std::size_t params)
    {
        CassStatement* ret = cass_statement_new(query, params);
        CassError rc =
            cass_statement_set_consistency(ret, CASS_CONSISTENCY_QUORUM);
        if (rc != CASS_OK)
        {
            std::stringstream ss;
            ss << "nodestore: Error setting query consistency: " << query
               << ", result: " << rc << ", " << cass_error_desc(rc);
            throw std::runtime_error(ss.str());
        }
        return ret;
    }

    std::atomic<bool> open_{false};

    // mutex used for open() and close()
    std::mutex mutex_;

    std::unique_ptr<CassSession, void (*)(CassSession*)> session_{
        nullptr,
        [](CassSession* session) {
            // Try to disconnect gracefully.
            CassFuture* fut = cass_session_close(session);
            cass_future_wait(fut);
            cass_future_free(fut);
            cass_session_free(session);
        }};

    // Database statements cached server side. Using these is more efficient
    // than making a new statement
    CassandraPreparedStatement insertObject_;
    CassandraPreparedStatement insertTransaction_;
    CassandraPreparedStatement insertLedgerTransaction_;
    CassandraPreparedStatement selectTransaction_;
    CassandraPreparedStatement selectAllTransactionHashesInLedger_;
    CassandraPreparedStatement selectObject_;
    CassandraPreparedStatement selectLedgerPageKeys_;
    CassandraPreparedStatement selectLedgerPage_;
    CassandraPreparedStatement upperBound2_;
    CassandraPreparedStatement getToken_;
    CassandraPreparedStatement insertSuccessor_;
    CassandraPreparedStatement selectSuccessor_;
    CassandraPreparedStatement insertDiff_;
    CassandraPreparedStatement selectDiff_;
    CassandraPreparedStatement insertAccountTx_;
    CassandraPreparedStatement selectAccountTx_;
    CassandraPreparedStatement selectAccountTxForward_;
    CassandraPreparedStatement insertLedgerHeader_;
    CassandraPreparedStatement insertLedgerHash_;
    CassandraPreparedStatement updateLedgerRange_;
    CassandraPreparedStatement deleteLedgerRange_;
    CassandraPreparedStatement updateLedgerHeader_;
    CassandraPreparedStatement selectLedgerBySeq_;
    CassandraPreparedStatement selectLedgerByHash_;
    CassandraPreparedStatement selectLatestLedger_;
    CassandraPreparedStatement selectLedgerRange_;

    // io_context used for exponential backoff for write retries
    mutable boost::asio::io_context ioContext_;
    std::optional<boost::asio::io_context::work> work_;
    std::thread ioThread_;

    // maximum number of concurrent in flight requests. New requests will wait
    // for earlier requests to finish if this limit is exceeded
    uint32_t maxRequestsOutstanding = 10000;
    // we keep this small because the indexer runs in the background, and we
    // don't want the database to be swamped when the indexer is running
    uint32_t indexerMaxRequestsOutstanding = 10;
    mutable std::atomic_uint32_t numRequestsOutstanding_ = 0;

    // mutex and condition_variable to limit the number of concurrent in flight
    // requests
    mutable std::mutex throttleMutex_;
    mutable std::condition_variable throttleCv_;

    // writes are asynchronous. This mutex and condition_variable is used to
    // wait for all writes to finish
    mutable std::mutex syncMutex_;
    mutable std::condition_variable syncCv_;

    boost::json::object config_;

    mutable uint32_t ledgerSequence_ = 0;

public:
    CassandraBackend(boost::json::object const& config)
        : BackendInterface(config), config_(config)
    {
    }

    ~CassandraBackend() override
    {
        if (open_)
            close();
    }

    bool
    isOpen()
    {
        return open_;
    }

    // Setup all of the necessary components for talking to the database.
    // Create the table if it doesn't exist already
    // @param createIfMissing ignored
    void
    open(bool readOnly) override;

    // Close the connection to the database
    void
    close() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            work_.reset();
            ioThread_.join();
        }
        open_ = false;
    }

    AccountTransactions
    fetchAccountTransactions(
        ripple::AccountID const& account,
        std::uint32_t limit,
        bool forward,
        std::optional<AccountTransactionsCursor> const& cursor) const override;

    bool
    doFinishWrites() override
    {
        // wait for all other writes to finish
        sync();
        // write range
        if (!range)
        {
            CassandraStatement statement{updateLedgerRange_};
            statement.bindNextInt(ledgerSequence_);
            statement.bindNextBoolean(false);
            statement.bindNextInt(ledgerSequence_);
            executeSyncWrite(statement);
        }
        CassandraStatement statement{updateLedgerRange_};
        statement.bindNextInt(ledgerSequence_);
        statement.bindNextBoolean(true);
        statement.bindNextInt(ledgerSequence_ - 1);
        if (!executeSyncUpdate(statement))
        {
            BOOST_LOG_TRIVIAL(warning)
                << __func__ << " Update failed for ledger "
                << std::to_string(ledgerSequence_) << ". Returning";
            return false;
        }
        BOOST_LOG_TRIVIAL(debug) << __func__ << " Committed ledger "
                                 << std::to_string(ledgerSequence_);
        return true;
    }
    void
    writeLedger(ripple::LedgerInfo const& ledgerInfo, std::string&& header)
        override;

    std::optional<uint32_t>
    fetchLatestLedgerSequence() const override
    {
        BOOST_LOG_TRIVIAL(trace) << __func__;
        CassandraStatement statement{selectLatestLedger_};
        CassandraResult result = executeSyncRead(statement);
        if (!result.hasResult())
        {
            BOOST_LOG_TRIVIAL(error)
                << "CassandraBackend::fetchLatestLedgerSequence - no rows";
            return {};
        }
        return result.getUInt32();
    }

    std::optional<ripple::LedgerInfo>
    fetchLedgerBySequence(uint32_t sequence) const override
    {
        BOOST_LOG_TRIVIAL(trace) << __func__;
        CassandraStatement statement{selectLedgerBySeq_};
        statement.bindNextInt(sequence);
        CassandraResult result = executeSyncRead(statement);

        if (!result)
        {
            BOOST_LOG_TRIVIAL(error) << __func__ << " - no rows";
            return {};
        }
        std::vector<unsigned char> header = result.getBytes();
        return deserializeHeader(ripple::makeSlice(header));
    }

    std::optional<ripple::LedgerInfo>
    fetchLedgerByHash(ripple::uint256 const& hash) const override
    {
        CassandraStatement statement{selectLedgerByHash_};

        statement.bindNextBytes(hash);

        CassandraResult result = executeSyncRead(statement);
        if (!result.hasResult())
        {
            BOOST_LOG_TRIVIAL(debug) << __func__ << " - no rows returned";
            return {};
        }

        std::uint32_t sequence = result.getInt64();

        return fetchLedgerBySequence(sequence);
    }

    std::optional<LedgerRange>
    hardFetchLedgerRange() const override;

    std::vector<TransactionAndMetadata>
    fetchAllTransactionsInLedger(uint32_t ledgerSequence) const override;

    std::vector<ripple::uint256>
    fetchAllTransactionHashesInLedger(uint32_t ledgerSequence) const override;

    // Synchronously fetch the object with key key, as of ledger with sequence
    // sequence
    std::optional<Blob>
    doFetchLedgerObject(ripple::uint256 const& key, uint32_t sequence)
        const override;

    std::optional<int64_t>
    getToken(void const* key) const
    {
        BOOST_LOG_TRIVIAL(trace) << "Fetching from cassandra";
        CassandraStatement statement{getToken_};
        statement.bindNextBytes(key, 32);
        CassandraResult result = executeSyncRead(statement);
        if (!result)
        {
            BOOST_LOG_TRIVIAL(error) << __func__ << " - no rows";
            return {};
        }
        int64_t token = result.getInt64();
        if (token == INT64_MAX)
            return {};
        else
            return token + 1;
    }

    std::optional<TransactionAndMetadata>
    fetchTransaction(ripple::uint256 const& hash) const override
    {
        BOOST_LOG_TRIVIAL(trace) << __func__;
        CassandraStatement statement{selectTransaction_};
        statement.bindNextBytes(hash);
        CassandraResult result = executeSyncRead(statement);
        if (!result)
        {
            BOOST_LOG_TRIVIAL(error) << __func__ << " - no rows";
            return {};
        }
        return {
            {result.getBytes(),
             result.getBytes(),
             result.getUInt32(),
             result.getUInt32()}};
    }
    std::optional<ripple::uint256>
    doFetchSuccessorKey(ripple::uint256 key, uint32_t ledgerSequence)
        const override;

    std::vector<TransactionAndMetadata>
    fetchTransactions(
        std::vector<ripple::uint256> const& hashes) const override;

    std::vector<Blob>
    doFetchLedgerObjects(
        std::vector<ripple::uint256> const& keys,
        uint32_t sequence) const override;

    std::vector<LedgerObject>
    fetchLedgerDiff(uint32_t ledgerSequence) const override;

    void
    doWriteLedgerObject(std::string&& key, uint32_t seq, std::string&& blob)
        override;

    void
    writeSuccessor(std::string&& key, uint32_t seq, std::string&& successor)
        override;

    void
    writeAccountTransactions(
        std::vector<AccountTransactionsData>&& data) override;

    void
    writeTransaction(
        std::string&& hash,
        uint32_t seq,
        uint32_t date,
        std::string&& transaction,
        std::string&& metadata) override;

    void
    startWrites() override
    {
    }

    void
    sync() const
    {
        std::unique_lock<std::mutex> lck(syncMutex_);

        syncCv_.wait(lck, [this]() { return finishedAllRequests(); });
    }
    bool
    doOnlineDelete(uint32_t numLedgersToKeep) const override;

    boost::asio::io_context&
    getIOContext() const
    {
        return ioContext_;
    }

    inline void
    incremementOutstandingRequestCount() const
    {
        {
            std::unique_lock<std::mutex> lck(throttleMutex_);
            if (!canAddRequest())
            {
                BOOST_LOG_TRIVIAL(trace)
                    << __func__ << " : "
                    << "Max outstanding requests reached. "
                    << "Waiting for other requests to finish";
                throttleCv_.wait(lck, [this]() { return canAddRequest(); });
            }
        }
        ++numRequestsOutstanding_;
    }

    inline void
    decrementOutstandingRequestCount() const
    {
        // sanity check
        if (numRequestsOutstanding_ == 0)
        {
            assert(false);
            throw std::runtime_error("decrementing num outstanding below 0");
        }
        size_t cur = (--numRequestsOutstanding_);
        {
            // mutex lock required to prevent race condition around spurious
            // wakeup
            std::lock_guard lck(throttleMutex_);
            throttleCv_.notify_one();
        }
        if (cur == 0)
        {
            // mutex lock required to prevent race condition around spurious
            // wakeup
            std::lock_guard lck(syncMutex_);
            syncCv_.notify_one();
        }
    }

    inline bool
    canAddRequest() const
    {
        return numRequestsOutstanding_ < maxRequestsOutstanding;
    }
    inline bool
    finishedAllRequests() const
    {
        return numRequestsOutstanding_ == 0;
    }

    void
    finishAsyncWrite() const
    {
        decrementOutstandingRequestCount();
    }

    template <class T, class S>
    void
    executeAsyncHelper(
        CassandraStatement const& statement,
        T callback,
        S& callbackData) const
    {
        CassFuture* fut = cass_session_execute(session_.get(), statement.get());

        cass_future_set_callback(
            fut, callback, static_cast<void*>(&callbackData));
        cass_future_free(fut);
    }
    template <class T, class S>
    void
    executeAsyncWrite(
        CassandraStatement const& statement,
        T callback,
        S& callbackData,
        bool isRetry) const
    {
        if (!isRetry)
            incremementOutstandingRequestCount();
        executeAsyncHelper(statement, callback, callbackData);
    }
    template <class T, class S>
    void
    executeAsyncRead(
        CassandraStatement const& statement,
        T callback,
        S& callbackData) const
    {
        executeAsyncHelper(statement, callback, callbackData);
    }
    void
    executeSyncWrite(CassandraStatement const& statement) const
    {
        CassFuture* fut;
        CassError rc;
        do
        {
            fut = cass_session_execute(session_.get(), statement.get());
            rc = cass_future_error_code(fut);
            if (rc != CASS_OK)
            {
                std::stringstream ss;
                ss << "Cassandra sync write error";
                ss << ", retrying";
                ss << ": " << cass_error_desc(rc);
                BOOST_LOG_TRIVIAL(warning) << ss.str();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } while (rc != CASS_OK);
        cass_future_free(fut);
    }

    bool
    executeSyncUpdate(CassandraStatement const& statement) const
    {
        bool timedOut = false;
        CassFuture* fut;
        CassError rc;
        do
        {
            fut = cass_session_execute(session_.get(), statement.get());
            rc = cass_future_error_code(fut);
            if (rc != CASS_OK)
            {
                timedOut = true;
                std::stringstream ss;
                ss << "Cassandra sync update error";
                ss << ", retrying";
                ss << ": " << cass_error_desc(rc);
                BOOST_LOG_TRIVIAL(warning) << ss.str();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } while (rc != CASS_OK);
        CassResult const* res = cass_future_get_result(fut);
        cass_future_free(fut);

        CassRow const* row = cass_result_first_row(res);
        if (!row)
        {
            BOOST_LOG_TRIVIAL(error) << "executeSyncUpdate - no rows";
            cass_result_free(res);
            return false;
        }
        cass_bool_t success;
        rc = cass_value_get_bool(cass_row_get_column(row, 0), &success);
        if (rc != CASS_OK)
        {
            cass_result_free(res);
            BOOST_LOG_TRIVIAL(error)
                << "executeSyncUpdate - error getting result " << rc << ", "
                << cass_error_desc(rc);
            return false;
        }
        cass_result_free(res);
        if (success != cass_true && timedOut)
        {
            BOOST_LOG_TRIVIAL(warning)
                << __func__ << " Update failed, but timedOut is true";
        }
        // if there was a timeout, the update may have succeeded in the
        // background. We can't differentiate between an async success and
        // another writer, so we just return true here
        return success == cass_true || timedOut;
    }

    CassandraResult
    executeSyncRead(CassandraStatement const& statement) const
    {
        CassFuture* fut;
        CassError rc;
        do
        {
            fut = cass_session_execute(session_.get(), statement.get());
            rc = cass_future_error_code(fut);
            if (rc != CASS_OK)
            {
                std::stringstream ss;
                ss << "Cassandra executeSyncRead error";
                ss << ": " << cass_error_desc(rc);
                BOOST_LOG_TRIVIAL(error) << ss.str();
            }
            if (isTimeout(rc))
            {
                cass_future_free(fut);
                throw DatabaseTimeout();
            }

            if (rc == CASS_ERROR_SERVER_INVALID_QUERY)
            {
                throw std::runtime_error("invalid query");
            }
        } while (rc != CASS_OK);

        CassResult const* res = cass_future_get_result(fut);
        cass_future_free(fut);
        return {res};
    }
};

}  // namespace Backend
#endif
