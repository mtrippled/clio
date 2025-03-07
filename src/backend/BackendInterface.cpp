#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/STLedgerEntry.h>
#include <backend/BackendInterface.h>
namespace Backend {
bool
BackendInterface::finishWrites(uint32_t ledgerSequence)
{
    auto commitRes = doFinishWrites();
    if (commitRes)
    {
        updateRange(ledgerSequence);
    }
    return commitRes;
}
void
BackendInterface::writeLedgerObject(
    std::string&& key,
    uint32_t seq,
    std::string&& blob)
{
    assert(key.size() == sizeof(ripple::uint256));
    ripple::uint256 key256 = ripple::uint256::fromVoid(key.data());
    doWriteLedgerObject(std::move(key), seq, std::move(blob));
}

std::optional<LedgerRange>
BackendInterface::hardFetchLedgerRangeNoThrow() const
{
    BOOST_LOG_TRIVIAL(debug) << __func__;
    while (true)
    {
        try
        {
            return hardFetchLedgerRange();
        }
        catch (DatabaseTimeout& t)
        {
            ;
        }
    }
}
// *** state data methods
std::optional<Blob>
BackendInterface::fetchLedgerObject(
    ripple::uint256 const& key,
    uint32_t sequence) const
{
    auto obj = cache_.get(key, sequence);
    if (obj)
    {
        BOOST_LOG_TRIVIAL(debug)
            << __func__ << " - cache hit - " << ripple::strHex(key);
        return *obj;
    }
    else
    {
        BOOST_LOG_TRIVIAL(debug)
            << __func__ << " - cache miss - " << ripple::strHex(key);
        auto dbObj = doFetchLedgerObject(key, sequence);
        if (!dbObj)
            BOOST_LOG_TRIVIAL(debug)
                << __func__ << " - missed cache and missed in db";
        else
            BOOST_LOG_TRIVIAL(debug)
                << __func__ << " - missed cache but found in db";
        return dbObj;
    }
}

std::vector<Blob>
BackendInterface::fetchLedgerObjects(
    std::vector<ripple::uint256> const& keys,
    uint32_t sequence) const
{
    std::vector<Blob> results;
    results.resize(keys.size());
    std::vector<ripple::uint256> misses;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        auto obj = cache_.get(keys[i], sequence);
        if (obj)
            results[i] = *obj;
        else
            misses.push_back(keys[i]);
    }
    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " - cache hits = " << keys.size() - misses.size()
        << " - cache misses = " << misses.size();

    if (misses.size())
    {
        auto objs = doFetchLedgerObjects(misses, sequence);
        for (size_t i = 0, j = 0; i < results.size(); ++i)
        {
            if (results[i].size() == 0)
            {
                results[i] = objs[j];
                ++j;
            }
        }
    }
    return results;
}
// Fetches the successor to key/index
std::optional<ripple::uint256>
BackendInterface::fetchSuccessorKey(
    ripple::uint256 key,
    uint32_t ledgerSequence) const
{
    auto succ = cache_.getSuccessor(key, ledgerSequence);
    if (succ)
        BOOST_LOG_TRIVIAL(debug)
            << __func__ << " - cache hit - " << ripple::strHex(key);
    else
        BOOST_LOG_TRIVIAL(debug)
            << __func__ << " - cache miss - " << ripple::strHex(key);
    return succ ? succ->key : doFetchSuccessorKey(key, ledgerSequence);
}
std::optional<LedgerObject>
BackendInterface::fetchSuccessorObject(
    ripple::uint256 key,
    uint32_t ledgerSequence) const
{
    auto succ = fetchSuccessorKey(key, ledgerSequence);
    if (succ)
    {
        auto obj = fetchLedgerObject(*succ, ledgerSequence);
        assert(obj);
        return {{*succ, *obj}};
    }
    return {};
}
BookOffersPage
BackendInterface::fetchBookOffers(
    ripple::uint256 const& book,
    uint32_t ledgerSequence,
    std::uint32_t limit,
    std::optional<ripple::uint256> const& cursor) const
{
    // TODO try to speed this up. This can take a few seconds. The goal is
    // to get it down to a few hundred milliseconds.
    BookOffersPage page;
    const ripple::uint256 bookEnd = ripple::getQualityNext(book);
    ripple::uint256 uTipIndex = book;
    bool done = false;
    std::vector<ripple::uint256> keys;
    auto getMillis = [](auto diff) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(diff)
            .count();
    };
    auto begin = std::chrono::system_clock::now();
    uint32_t numSucc = 0;
    uint32_t numPages = 0;
    long succMillis = 0;
    long pageMillis = 0;
    while (keys.size() < limit)
    {
        auto mid1 = std::chrono::system_clock::now();
        auto offerDir = fetchSuccessorObject(uTipIndex, ledgerSequence);
        auto mid2 = std::chrono::system_clock::now();
        numSucc++;
        succMillis += getMillis(mid2 - mid1);
        if (!offerDir || offerDir->key > bookEnd)
        {
            BOOST_LOG_TRIVIAL(debug) << __func__ << " - offerDir.has_value() "
                                     << offerDir.has_value() << " breaking";
            break;
        }
        while (keys.size() < limit)
        {
            ++numPages;
            uTipIndex = offerDir->key;
            ripple::STLedgerEntry sle{
                ripple::SerialIter{
                    offerDir->blob.data(), offerDir->blob.size()},
                offerDir->key};
            auto indexes = sle.getFieldV256(ripple::sfIndexes);
            keys.insert(keys.end(), indexes.begin(), indexes.end());
            auto next = sle.getFieldU64(ripple::sfIndexNext);
            if (!next)
            {
                BOOST_LOG_TRIVIAL(debug)
                    << __func__ << " next is empty. breaking";
                break;
            }
            auto nextKey = ripple::keylet::page(uTipIndex, next);
            auto nextDir = fetchLedgerObject(nextKey.key, ledgerSequence);
            assert(nextDir);
            offerDir->blob = *nextDir;
            offerDir->key = nextKey.key;
        }
        auto mid3 = std::chrono::system_clock::now();
        pageMillis += getMillis(mid3 - mid2);
    }
    auto mid = std::chrono::system_clock::now();
    auto objs = fetchLedgerObjects(keys, ledgerSequence);
    for (size_t i = 0; i < keys.size() && i < limit; ++i)
    {
        BOOST_LOG_TRIVIAL(debug)
            << __func__ << " key = " << ripple::strHex(keys[i])
            << " blob = " << ripple::strHex(objs[i])
            << " ledgerSequence = " << ledgerSequence;
        assert(objs[i].size());
        page.offers.push_back({keys[i], objs[i]});
    }
    auto end = std::chrono::system_clock::now();
    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " "
        << "Fetching " << std::to_string(keys.size()) << " offers took "
        << std::to_string(getMillis(mid - begin))
        << " milliseconds. Fetching next dir took "
        << std::to_string(succMillis) << " milliseonds. Fetched next dir "
        << std::to_string(numSucc) << " times"
        << " Fetching next page of dir took " << std::to_string(pageMillis)
        << " milliseconds"
        << ". num pages = " << std::to_string(numPages)
        << ". Fetching all objects took "
        << std::to_string(getMillis(end - mid))
        << " milliseconds. total time = "
        << std::to_string(getMillis(end - begin)) << " milliseconds";

    return page;
}

LedgerPage
BackendInterface::fetchLedgerPage(
    std::optional<ripple::uint256> const& cursor,
    std::uint32_t ledgerSequence,
    std::uint32_t limit,
    std::uint32_t limitHint) const
{
    LedgerPage page;

    std::vector<ripple::uint256> keys;
    while (keys.size() < limit)
    {
        ripple::uint256 const& curCursor =
            keys.size() ? keys.back() : cursor ? *cursor : firstKey;
        auto succ = fetchSuccessorKey(curCursor, ledgerSequence);
        if (!succ)
            break;
        keys.push_back(std::move(*succ));
    }

    auto objects = fetchLedgerObjects(keys, ledgerSequence);
    for (size_t i = 0; i < objects.size(); ++i)
    {
        assert(objects[i].size());
        page.objects.push_back({std::move(keys[i]), std::move(objects[i])});
    }
    if (page.objects.size() >= limit)
        page.cursor = page.objects.back().key;
    return page;
}

std::optional<ripple::Fees>
BackendInterface::fetchFees(std::uint32_t seq) const
{
    ripple::Fees fees;

    auto key = ripple::keylet::fees().key;
    auto bytes = fetchLedgerObject(key, seq);

    if (!bytes)
    {
        BOOST_LOG_TRIVIAL(error) << __func__ << " - could not find fees";
        return {};
    }

    ripple::SerialIter it(bytes->data(), bytes->size());
    ripple::SLE sle{it, key};

    if (sle.getFieldIndex(ripple::sfBaseFee) != -1)
        fees.base = sle.getFieldU64(ripple::sfBaseFee);

    if (sle.getFieldIndex(ripple::sfReferenceFeeUnits) != -1)
        fees.units = sle.getFieldU32(ripple::sfReferenceFeeUnits);

    if (sle.getFieldIndex(ripple::sfReserveBase) != -1)
        fees.reserve = sle.getFieldU32(ripple::sfReserveBase);

    if (sle.getFieldIndex(ripple::sfReserveIncrement) != -1)
        fees.increment = sle.getFieldU32(ripple::sfReserveIncrement);

    return fees;
}

}  // namespace Backend
