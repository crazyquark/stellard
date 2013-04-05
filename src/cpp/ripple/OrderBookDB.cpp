#include <boost/foreach.hpp>

#include "Application.h"
#include "OrderBookDB.h"
#include "Log.h"

SETUP_LOG();

OrderBookDB::OrderBookDB() : mSeq(0)
{

}

void OrderBookDB::invalidate()
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	mSeq = 0;
}

void OrderBookDB::setup(Ledger::ref ledger)
{
	boost::unordered_set<uint256> mSeen;

	boost::recursive_mutex::scoped_lock sl(mLock);

	if (ledger->getLedgerSeq() == mSeq)
		return;
	mSeq = ledger->getLedgerSeq();

	LoadEvent::autoptr ev = theApp->getJobQueue().getLoadEventAP(jtOB_SETUP, "OrderBookDB::setup");

	mDestMap.clear();
	mSourceMap.clear();

	cLog(lsDEBUG) << "OrderBookDB>";

	// walk through the entire ledger looking for orderbook entries
	uint256 currentIndex = ledger->getFirstLedgerIndex();
	while (currentIndex.isNonZero())
	{
		SLE::pointer entry = ledger->getSLEi(currentIndex);
		if ((entry->getType() == ltDIR_NODE) && (entry->isFieldPresent(sfExchangeRate)) &&
			(entry->getFieldH256(sfRootIndex) == currentIndex))
		{
			const uint160& ci = entry->getFieldH160(sfTakerPaysCurrency);
			const uint160& co = entry->getFieldH160(sfTakerGetsCurrency);
			const uint160& ii = entry->getFieldH160(sfTakerPaysIssuer);
			const uint160& io = entry->getFieldH160(sfTakerGetsIssuer);

			uint256 index = Ledger::getBookBase(ci, ii, co, io);
			if (mSeen.insert(index).second)
			{
				OrderBook::pointer book = boost::make_shared<OrderBook>(boost::cref(index),
					boost::cref(ci), boost::cref(co), boost::cref(ii), boost::cref(io));

				mSourceMap[currencyIssuer_ct(ci, ii)].push_back(book);
				mDestMap[currencyIssuer_ct(co, io)].push_back(book);
			}
		}

		currentIndex = ledger->getNextLedgerIndex(currentIndex);
	}

	cLog(lsDEBUG) << "OrderBookDB<";
}

// return list of all orderbooks that want this issuerID and currencyID
void OrderBookDB::getBooksByTakerPays(const uint160& issuerID, const uint160& currencyID,
	std::vector<OrderBook::pointer>& bookRet)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	boost::unordered_map< currencyIssuer_t, std::vector<OrderBook::pointer> >::const_iterator
		it = mSourceMap.find(currencyIssuer_ct(currencyID, issuerID));
	if (it != mSourceMap.end())
		bookRet = it->second;
	else
		bookRet.clear();
}

// return list of all orderbooks that give this issuerID and currencyID
void OrderBookDB::getBooksByTakerGets(const uint160& issuerID, const uint160& currencyID,
	std::vector<OrderBook::pointer>& bookRet)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	boost::unordered_map< currencyIssuer_t, std::vector<OrderBook::pointer> >::const_iterator
		it = mDestMap.find(currencyIssuer_ct(currencyID, issuerID));
	if (it != mDestMap.end())
		bookRet = it->second;
	else
		bookRet.clear();
}

BookListeners::pointer OrderBookDB::makeBookListeners(const uint160& currencyPays, const uint160& currencyGets,
	const uint160& issuerPays, const uint160& issuerGets)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	BookListeners::pointer ret = getBookListeners(currencyPays, currencyGets, issuerPays, issuerGets);
	if (!ret)
	{
		ret = boost::make_shared<BookListeners>();
		mListeners[issuerPays][issuerGets][currencyPays][currencyGets] = ret;
	}
	return ret;
}

BookListeners::pointer OrderBookDB::getBookListeners(const uint160& currencyPays, const uint160& currencyGets,
	const uint160& issuerPays, const uint160& issuerGets)
{
	BookListeners::pointer ret;
	boost::recursive_mutex::scoped_lock sl(mLock);

	std::map<uint160, std::map<uint160, std::map<uint160, std::map<uint160, BookListeners::pointer> > > >::iterator
		it0 = mListeners.find(issuerPays);
	if(it0 == mListeners.end())
		return ret;

	std::map<uint160, std::map<uint160, std::map<uint160, BookListeners::pointer> > >::iterator
		it1 = (*it0).second.find(issuerGets);
	if(it1 == (*it0).second.end())
		return ret;

	std::map<uint160, std::map<uint160, BookListeners::pointer> >::iterator it2 = (*it1).second.find(currencyPays);
	if(it2 == (*it1).second.end())
		return ret;

	std::map<uint160, BookListeners::pointer>::iterator it3 = (*it2).second.find(currencyGets);
	if(it3 == (*it2).second.end())
		return ret;

	return (*it3).second;
}

// Based on the meta, send the meta to the streams that are listening 
// We need to determine which streams a given meta effects
void OrderBookDB::processTxn(Ledger::ref ledger, const ALTransaction& alTx, Json::Value& jvObj)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	if (alTx.getResult() == tesSUCCESS)
	{
		// check if this is an offer or an offer cancel or a payment that consumes an offer
		//check to see what the meta looks like
		BOOST_FOREACH(STObject& node, alTx.getMeta()->getNodes())
		{
			try
			{
				if (node.getFieldU16(sfLedgerEntryType) == ltOFFER)
				{
					SField* field=NULL;

					if (node.getFName() == sfModifiedNode)
					{
						field = &sfPreviousFields;
					}
					else if (node.getFName() == sfCreatedNode)
					{
						field = &sfNewFields;
					}
					else if (node.getFName() == sfDeletedNode)
					{
						field = &sfFinalFields;
					}

					if (field)
					{
						const STObject* data = dynamic_cast<const STObject*>(node.peekAtPField(*field));
						if (data)
						{
							STAmount takerGets = data->getFieldAmount(sfTakerGets);
							uint160 currencyGets = takerGets.getCurrency();
							uint160 issuerGets = takerGets.getIssuer();

							STAmount takerPays = data->getFieldAmount(sfTakerPays);
							uint160 currencyPays = takerPays.getCurrency();
							uint160 issuerPays = takerPays.getIssuer();

							// determine the OrderBook
							BookListeners::pointer book =
								getBookListeners(currencyPays, currencyGets, issuerPays, issuerGets);
							if (book)
								book->publish(jvObj);
						}
					}
				}
			}
			catch (...)
			{
				cLog(lsINFO) << "Fields not found in OrderBookDB::processTxn";
			}
		}
	}
}

void BookListeners::addSubscriber(InfoSub::ref sub)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	mListeners[sub->getSeq()] = sub;
}

void BookListeners::removeSubscriber(uint64 seq)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	mListeners.erase(seq);
}

void BookListeners::publish(Json::Value& jvObj)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	NetworkOPs::subMapType::const_iterator it = mListeners.begin();
	while (it != mListeners.end())
	{
		InfoSub::pointer p = it->second.lock();
		if (p)
		{
			p->send(jvObj, true);
			++it;
		}
		else
			it = mListeners.erase(it);
	}
}

// vim:ts=4
