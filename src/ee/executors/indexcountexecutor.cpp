/* This file is part of VoltDB.
 * Copyright (C) 2008-2012 VoltDB Inc.
 *
 * VoltDB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VoltDB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "indexcountexecutor.h"

#include "common/debuglog.h"
#include "common/common.h"
#include "common/SQLException.h"
#include "common/tabletuple.h"
#include "common/ValueFactory.hpp"
#include "expressions/abstractexpression.h"
#include "indexes/tableindex.h"
#include "plannodes/indexcountnode.h"
#include "storage/temptable.h"
#include "storage/persistenttable.h"

using namespace voltdb;

bool IndexCountExecutor::p_init()
{
    VOLT_DEBUG("init IndexCount Executor");

    IndexCountPlanNode* node = dynamic_cast<IndexCountPlanNode*>(m_abstractNode);
    assert(node);
    assert(node->getPredicate() == NULL);

    //
    // Make sure that we have search keys and that they're not null
    //
    m_numOfSearchkeys = (int)node->getSearchKeyExpressions().size();
    for (int ctr = 0; ctr < m_numOfSearchkeys; ctr++)
    {
        if (node->getSearchKeyExpressions()[ctr] == NULL) {
            VOLT_ERROR("The search key expression at position '%d' is NULL for PlanNode "
                "'%s'", ctr, node->debug().c_str());
            return false;
        }
    }

    m_numOfEndkeys = (int)node->getEndKeyExpressions().size();
    if (m_numOfEndkeys != 0) {
        for (int ctr = 0; ctr < m_numOfEndkeys; ctr++)
        {
            if (node->getEndKeyExpressions()[ctr] == NULL) {
                VOLT_ERROR("The end key expression at position '%d' is NULL for PlanNode "
                    "'%s'", ctr, node->debug().c_str());
                return false;
            }
        }
    }

    //
    // Initialize local variables
    //

    // output must be a temp table
    assert(m_outputTable);
    assert(m_outputTable == node->getOutputTable());
    assert(m_outputTable == dynamic_cast<TempTable*>(m_outputTable));

    //target table should be persistenttable
    assert(m_targetTable);
    assert(m_targetTable == dynamic_cast<PersistentTable*>(m_targetTable));
    m_numOfColumns = static_cast<int>(m_outputTable->columnCount());

    assert(m_numOfColumns == 1);
    //
    // Grab the Index from our inner table
    // We'll throw an error if the index is missing
    //
    m_index = m_targetTable->index(node->getTargetIndexName());
    assert (m_index != NULL);

    // This index should have a true countable flag
    assert(m_index->isCountableIndex());

    m_searchKey.allocateTupleNoHeader(m_index->getKeySchema());
    m_lookupType = node->getLookupType();
    if (m_numOfEndkeys != 0) {
        m_endKey.allocateTupleNoHeader(m_index->getKeySchema());
        m_endType = node->getEndType();
    }

    // Need to move GTE to find (x,_) when doing a partial covering search.
    // the planner sometimes lies in this case: INDEX_LOOKUP_TYPE_EQ is incorrect.
    // INDEX_LOOKUP_TYPE_GTE is necessary. Make the change here.
    if (m_lookupType == INDEX_LOOKUP_TYPE_EQ &&
        m_searchKey.getSchema()->columnCount() > m_numOfSearchkeys)
    {
        VOLT_TRACE("Setting lookup type to GTE for partial covering key.");
        m_lookupType = INDEX_LOOKUP_TYPE_GTE;
    }

    return true;
}

bool IndexCountExecutor::p_execute()
{
    IndexCountPlanNode* node = dynamic_cast<IndexCountPlanNode*>(m_abstractNode);
    assert(node);
    // output must be a temp table
    assert(m_outputTable);
    assert(m_outputTable == m_abstractNode->getOutputTable());
    TempTable* output_temp_table = dynamic_cast<TempTable*>(m_outputTable);
    assert(output_temp_table);
    assert(m_targetTable);
    assert(m_targetTable == node->getTargetTable());

    assert (node->getPredicate() == NULL);

    assert (m_index);
    assert (m_index == m_targetTable->index(node->getTargetIndexName()));
    assert (m_index->isCountableIndex());

    VOLT_DEBUG("IndexCount: %s.%s\n", m_targetTable->name().c_str(),
               m_index->getName().c_str());

    int activeNumOfSearchKeys = m_numOfSearchkeys;
    IndexLookupType localLookupType = m_lookupType;
    bool searchKeyUnderflow = false, endKeyOverflow = false;
    // Overflow cases that can return early without accessing the index need this
    // default 0 count as their result.
    TableTuple& tmptup = m_outputTable->tempTuple();
    tmptup.setNValue(0, ValueFactory::getBigIntValue( 0 ));
    TableTuple m_dummy;

    //
    // SEARCH KEY
    //
    m_searchKey.setAllNulls();
    const std::vector<AbstractExpression*>& searchKeyExpressions = node->getSearchKeyExpressions();
    VOLT_DEBUG("<Index Count>Initial (all null) search key: '%s'", m_searchKey.debugNoHeader().c_str());
    for (int ctr = 0; ctr < activeNumOfSearchKeys; ctr++) {
        NValue candidateValue = searchKeyExpressions[ctr]->eval(&m_dummy, NULL);
        try {
            m_searchKey.setNValue(ctr, candidateValue);
        }
        catch (SQLException e) {
            // This next bit of logic handles underflow and overflow while
            // setting up the search keys.
            // e.g. TINYINT > 200 or INT <= 6000000000

            // re-throw if not an overflow or underflow
            // currently, it's expected to always be an overflow or underflow
            if ((e.getInternalFlags() & (SQLException::TYPE_OVERFLOW | SQLException::TYPE_UNDERFLOW)) == 0) {
                throw e;
            }

            // handle the case where this is a comparison, rather than equality match
            // comparison is the only place where the executor might return matching tuples
            // e.g. TINYINT < 1000 should return all values

            if ((localLookupType != INDEX_LOOKUP_TYPE_EQ) &&
                (ctr == (activeNumOfSearchKeys - 1))) {
                assert (localLookupType == INDEX_LOOKUP_TYPE_GT || localLookupType == INDEX_LOOKUP_TYPE_GTE);

                if (e.getInternalFlags() & SQLException::TYPE_OVERFLOW) {
                    output_temp_table->insertTempTuple(tmptup);
                    return true;
                } else if (e.getInternalFlags() & SQLException::TYPE_UNDERFLOW) {
                    searchKeyUnderflow = true;
                    break;
                } else {
                    throw e;
                }
            }
            // if a EQ comparision is out of range, then return no tuples
            else {
                output_temp_table->insertTempTuple(tmptup);
                return true;
            }
            break;
        }
    }

    VOLT_TRACE("Search key: '%s'", m_searchKey.debugNoHeader().c_str());

    if (m_numOfEndkeys != 0) {
        //
        // END KEY
        //
        m_endKey.setAllNulls();
        VOLT_DEBUG("Initial (all null) end key: '%s'", m_endKey.debugNoHeader().c_str());
        const std::vector<AbstractExpression*>& endKeyExpressions = node->getEndKeyExpressions();
        for (int ctr = 0; ctr < m_numOfEndkeys; ctr++) {
            NValue endKeyValue = endKeyExpressions[ctr]->eval(&m_dummy, NULL);
            try {
                m_endKey.setNValue(ctr, endKeyValue);
            }
            catch (SQLException e) {
                // This next bit of logic handles underflow and overflow while
                // setting up the search keys.
                // e.g. TINYINT > 200 or INT <= 6000000000

                // re-throw if not an overflow or underflow
                // currently, it's expected to always be an overflow or underflow
                if ((e.getInternalFlags() & (SQLException::TYPE_OVERFLOW | SQLException::TYPE_UNDERFLOW)) == 0) {
                    throw e;
                }

                if (ctr == (m_numOfEndkeys - 1)) {
                    assert (m_endType == INDEX_LOOKUP_TYPE_LT || m_endType == INDEX_LOOKUP_TYPE_LTE);
                    if (e.getInternalFlags() & SQLException::TYPE_UNDERFLOW) {
                        output_temp_table->insertTempTuple(tmptup);
                        return true;
                    } else if (e.getInternalFlags() & SQLException::TYPE_OVERFLOW) {
                        endKeyOverflow = true;
                        const ValueType type = m_endKey.getSchema()->columnType(ctr);
                        NValue tmpEndKeyValue = ValueFactory::getBigIntValue(getMaxTypeValue(type));
                        m_endKey.setNValue(ctr, tmpEndKeyValue);

                        VOLT_DEBUG("<Index count> end key out of range, MAX value: %ld...\n", (long)getMaxTypeValue(type));
                        break;
                    } else {
                        throw e;
                    }
                }
                // if a EQ comparision is out of range, then return no tuples
                else {
                    output_temp_table->insertTempTuple(tmptup);
                    return true;
                }
                break;
            }
        }
        VOLT_TRACE("End key: '%s'", m_endKey.debugNoHeader().c_str());
    }

    // An index count has two cases: unique and non-unique
    int64_t rkStart = 0, rkEnd = 0, rkRes = 0;
    int leftIncluded = 0, rightIncluded = 0;

    // Deal with multi-map
    VOLT_DEBUG("INDEX_LOOKUP_TYPE(%d) m_numSearchkeys(%d) key:%s",
               localLookupType, activeNumOfSearchKeys, m_searchKey.debugNoHeader().c_str());
    if (searchKeyUnderflow == false) {
        if (localLookupType == INDEX_LOOKUP_TYPE_GT) {
            rkStart = m_index->getCounterLET(&m_searchKey, true);
        } else if (localLookupType == INDEX_LOOKUP_TYPE_GTE) {
            if (m_index->hasKey(&m_searchKey)) {
                leftIncluded = 1;
                rkStart = m_index->getCounterLET(&m_searchKey, false);
            } else {
                rkStart = m_index->getCounterLET(&m_searchKey, true);
            }
            if (m_searchKey.getSchema()->columnCount() > activeNumOfSearchKeys) {
                // search key is not complete:
                // like: SELECT count(*) from T2 WHERE USERNAME ='XIN' AND POINTS < ?
                // like: SELECT count(*) from T2 WHERE POINTS < ?
                // but it actually finds the previous rank. (If m_searchKey is null, find 0 rank)
                // Add 1 back.
                rkStart++;
                leftIncluded = 1;
            }
        } else {
            return false;
        }
    }
    if (m_numOfEndkeys != 0) {
        if (endKeyOverflow) {
            rkEnd = m_index->getCounterGET(&m_endKey, true);
        } else {
            IndexLookupType localEndType = m_endType;
            if (localEndType == INDEX_LOOKUP_TYPE_LT) {
                rkEnd = m_index->getCounterGET(&m_endKey, false);
            } else if (localEndType == INDEX_LOOKUP_TYPE_LTE) {
                if (m_index->hasKey(&m_endKey)) {
                    rkEnd = m_index->getCounterGET(&m_endKey, true);
                    rightIncluded = 1;
                } else
                    rkEnd = m_index->getCounterGET(&m_endKey, false);
            } else {
                return false;
            }
        }
    } else {
        rkEnd = m_index->getSize();
        rightIncluded = 1;
    }
    rkRes = rkEnd - rkStart - 1 + leftIncluded + rightIncluded;
    VOLT_DEBUG("Index Count ANSWER %ld = %ld - %ld - 1 + %d + %d\n", (long)rkRes, (long)rkEnd, (long)rkStart, leftIncluded, rightIncluded);
    tmptup.setNValue(0, ValueFactory::getBigIntValue( rkRes ));
    output_temp_table->insertTempTuple(tmptup);

    VOLT_DEBUG ("Index Count :\n %s", m_outputTable->debug().c_str());
    return true;
}

IndexCountExecutor::~IndexCountExecutor() { }
