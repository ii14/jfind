#include "../include/item_sorter.hpp"
#include "../include/util.hpp"
#include "../include/item_matcher.hpp"
#include "../include/thread_manager.hpp"
#include <unordered_map>
#include <cstring>
#include <climits>

using namespace std::chrono_literals;

ItemSorter::ItemSorter() {
    m_isSorted = false;
    m_queryChanged = false;
    m_heuristicIdx = 0;
    m_sortIdx = 0;

    m_dispatch.subscribe(this, QUERY_CHANGE_EVENT);
    m_dispatch.subscribe(this, NEW_ITEMS_EVENT);
    m_dispatch.subscribe(this, QUIT_EVENT);
}

bool sortFunc(Item& l, Item& r) {
    if (l.heuristic == r.heuristic) {
        if (strlen(l.text) == strlen(r.text)) {
            return l.index < r.index;
        }
        return strlen(l.text) < strlen(r.text);
    }
    return l.heuristic > r.heuristic;
}

bool sortEmptyFunc(Item& l, Item& r) {
    return l.index < r.index;
}

void ItemSorter::sort(int sortIdx) {
    if (sortIdx <= m_sortIdx) {
        return;
    }
    if (sortIdx > m_items.size()) {
        sortIdx = m_items.size();
    }
    std::function<bool(Item& l, Item &r)> f;
    f = m_isSorted ? sortFunc : sortEmptyFunc;

    m_logger.log("sorting from %d to %d", m_sortIdx, sortIdx);

    std::partial_sort(m_items.begin() + m_sortIdx,
            m_items.begin() + sortIdx, m_items.end(), f);
    m_sortIdx = sortIdx;
}

int ItemSorter::size() {
    return m_items.size();
}

void ItemSorter::setQuery() {
    m_queryChanged = false;
    if (m_query == m_newQuery) {
        return;
    }
    bool shrunk = m_newQuery.size() < m_query.size();
    bool backspaced = shrunk || (!shrunk && !m_newQuery.starts_with(m_query));

    if (backspaced) {
        m_heuristicIdx = 0;
    }

    m_query = m_newQuery;
}

void ItemSorter::calcHeuristics(bool queryChanged) {
    if (queryChanged && m_heuristicIdx) {
        m_logger.log("fast calcHeuristics for %d items", m_heuristicIdx);
        calcHeuristics(false, 0, m_heuristicIdx);
    }
    int n = m_items.size();
    if (n && n > m_heuristicIdx) {
        m_logger.log("slow calcHeuristics for %d items", n - m_heuristicIdx);
        calcHeuristics(true, m_heuristicIdx, n);
    }
    m_heuristicIdx = n;
}

void ItemSorter::calcHeuristics(bool newItems, int start, int end)
{
    std::function<void(Item*, int)> f;
    ItemMatcher matcher;
    std::vector<std::string> words = split(m_query, ' ');

    m_isSorted = m_query.size() > 0;

    if (m_isSorted) {
        f = [&] (Item *item, int n) {
            for (int i = 0; i < n; i++) {
                if (m_queryChanged) {
                    return;
                }
                if (!newItems && item->heuristic == BAD_HEURISTIC) {
                    item++;
                    continue;
                }
                (*item).heuristic = matcher.calc((*item).text, words);
                item++;
            }
        };
    }
    else {
        f = [] (Item *item, int n) {
            for (int i = 0; i < n; i++) {
                item->heuristic = 0;
                item++;
            }
        };
    }

    ThreadManager<Item> manager(f);
    manager.setNumThreads(4);
    manager.setThreshold(1024);
    manager.run(m_items.data() + start, end - start);

    m_sortIdx = 0;
}

int ItemSorter::copyItems(Item *buffer, int idx, int n) {
    if (idx + n < 256) {
        if (idx + n > m_firstItemsSize) {
            n = m_firstItemsSize - idx;
        }
        for (int i = 0; i < n; i++) {
            buffer[i] = m_firstItems[idx + i];
        }
        return n;
    }

    std::unique_lock items_lock(m_items_mut);
    if (idx + n > m_items.size()) {
        n = m_items.size() - idx;
    }

    if (idx + n > m_sortIdx) {
        sort(idx + n);
    }

    for (int i = 0; i < n; i++) {
        buffer[i] = m_items[idx + i];
    }

    return n;
}

void ItemSorter::sorterThread() {
    std::unique_lock items_lock(m_items_mut);
    while (m_sorterThreadActive) {
        addNewItems();
        sortItems();
        {
            std::unique_lock lock(m_sorter_mut);
            items_lock.unlock();
            if (m_sorterThreadActive && !m_queryChanged && !m_hasNewItems) {
                m_sorter_cv.wait(lock);
            }
            else {
                std::this_thread::yield();
            }
            items_lock.lock();
        }
    }
}

void ItemSorter::onStart() {
    m_sorterThreadActive = true;
    m_sorterThread = new std::thread(&ItemSorter::sorterThread, this);
}

void ItemSorter::onLoop() {
    return awaitEvent();
}

void ItemSorter::onEvent(std::shared_ptr<Event> event) {
    m_logger.log("received %s", getEventNames()[event->getType()]);
    switch (event->getType()) {
        case QUERY_CHANGE_EVENT: {
            QueryChangeEvent *queryChangeEvent
                    = (QueryChangeEvent*)event.get();
            std::unique_lock lock(m_sorter_mut);
            m_newQuery = queryChangeEvent->getQuery();
            m_queryChanged = true;
            m_sorter_cv.notify_one();
            break;
        }

        case NEW_ITEMS_EVENT: {
            NewItemsEvent *newItemsEvent = (NewItemsEvent*)event.get();
            std::unique_lock lock(m_sorter_mut);
            m_newItems = newItemsEvent->getItems();
            m_hasNewItems = true;
            m_sorter_cv.notify_one();
            break;
        }

        case QUIT_EVENT:
            endSorterThread();
            break;

        default:
            break;
    }
}

void ItemSorter::endSorterThread() {
    if (!m_sorterThreadActive) {
        return;
    }
    {
        std::unique_lock lock(m_sorter_mut);
        m_queryChanged = true;
        m_sorterThreadActive = false;
    }
    m_sorter_cv.notify_one();
    m_sorterThread->join();
    delete m_sorterThread;
}

void ItemSorter::addNewItems() {
    std::unique_lock sorter_lock(m_sorter_mut);
    if (!m_hasNewItems) {
        return;
    }
    m_hasNewItems = false;
    m_items.insert(m_items.end(), m_newItems->data(),
            m_newItems->data() + m_newItems->size());
    m_dispatch.dispatch(std::make_shared<ItemsAddedEvent>());
}

void ItemSorter::sortItems() {
    bool queryChanged;
    {
        std::unique_lock lock(m_sorter_mut);
        queryChanged = m_queryChanged;
        if (queryChanged) {
            setQuery();
        }
    }

    // calc will cancel upon query change
    // this way, jfind can quickly restart calc with new query
    calcHeuristics(queryChanged);

    if (!m_queryChanged) {
        // sort the first few items on the sorter thread. this is to remove the
        // delay on the main thread, which the user could notice
        m_firstItemsSize = m_items.size() < 256 ? m_items.size() : 256;
        sort(m_firstItemsSize);
        std::copy(m_items.begin(), m_items.begin() + m_firstItemsSize,
                  m_firstItems);
        m_dispatch.dispatch(std::make_shared<ItemsSortedEvent>());
    }
}
